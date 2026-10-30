#include "DeviceTable.h"

#include "../../util/src/LogBuffer.h"

/**
 * DeviceTable.cpp - Implementation of the mutex-free device/metric table.
 *
 * DESIGN NOTES
 *
 * Two roles share one class:
 *
 *   Owner role (used by SensorRegistry on REMOTE):
 *     registerMetric() is the only entry point. It auto-assigns a stable 1-based
 *     device index and a stable 1-based metric_id, then enforces three limits:
 *       1. Name length (<= 12 chars for names, <= 5 for units).
 *       2. Table capacity (<= 4 devices, <= 8 metrics per device).
 *       3. LoRa payload size: estimateEncodedDeviceSize() checks that the
 *          resulting loraiot_Device proto would fit in LORA_MAX_PAYLOAD_LEN bytes
 *          without running the encoder (pure arithmetic, no heap allocation).
 *     Any schema change (new device or new metric) increments rev so callers
 *     can track whether a crawl from the peer is still needed.
 *
 *   Cache role (used by PeerRegistry on LOCAL):
 *     applyDevice() absorbs a loraiot_Device received from a Describe response.
 *     It does a value-preserving merge: a metric whose id, unit and scale are
 *     all unchanged keeps its stored value; a metric that is new or whose
 *     definition changed has its value cleared. This means a cache entry survives
 *     re-crawls without data loss when only names change.
 *     applyData() absorbs a loraiot_Data DATA frame. It is a *partial* update:
 *     only the metric_ids listed in the frame are touched; every other metric in
 *     that device slot is left as-is. This is the key difference from the old
 *     PeerRegistry.storeData(), which wiped the whole lastData snapshot.
 *
 * TIMESTAMP MODEL
 *   Every MetricSlot stores its own valueMs (millis() at storage time). The
 *   per-metric timestamp is what makes partial pushes safe: two metrics that
 *   arrive in separate DATA frames each carry their own age independently.
 *   toProtoData() derives age_ms from the oldest valueMs present, which matches
 *   what a Read response consumer expects ("how long ago were these measured?").
 *
 * SIZE NOTE (DEVICE_TABLE_TEXT_BUF_SIZE = 13, i.e. 12-byte text limit)
 *   MetricSlot layout:  id(4) + name[13] + unit[6] + padding(1) + scale(4)
 *                       + union{value(4)/text[13]} padded to 16 + valueMs(4)
 *                       + hasValue(1) + isText(1) + padding(2) = 52 B
 *   sizeof(DeviceSlot)  = index(4) + name[13] + metricCount(1) + padding(2)
 *                       + metrics[8*52](416) = 436 B
 *   sizeof(DeviceTable) = devices[4*436](1744) + deviceCount(1) + padding(3)
 *                       + rev(4) = about 1752 bytes per instance
 */

// ---- nanopb size estimation constants --------------------------------------
//
// Conservative worst-case per-field overhead for protobuf varint /
// length-delimited fields. Used by estimateEncodedDeviceSize() to stay safe
// without running the encoder.
//
// loraiot_Device wire layout (proto3):
//   field 1 (device uint32):  tag 1B + varint max 5B = 6B
//   field 2 (name string):    tag 1B + len 1B + chars (max 12) = 14B
//   per metric (field 3):     tag 1B + len 1B + metric body
//     field 1 (id uint32):    tag 1B + varint max 5B = 6B
//     field 2 (name string):  tag 1B + len 1B + chars (max 12) = 14B
//     field 3 (unit string):  tag 1B + len 1B + chars (max 5) = 7B
//     field 4 (scale uint32): tag 1B + varint max 5B = 6B
//     metric body total:      33B
//   per metric wrapper:       tag(1) + len(1) + body(33) = 35B
//
// loraiot_Data / Response(Data) wire layout (proto3) worst-case. The Response
// path (REQUEST(Read) answer) is the tighter of the two, so it is the budget.
// Device index is 1..DEVICE_TABLE_MAX_DEVICES (4) and metric_id is
// 1..DEVICE_TABLE_MAX_METRICS (8), so both always encode as a 1 byte varint.
//   Response wrapper overhead: request_seq (tag 1B + varint max 5B = 6B)
//                              status (tag 1B + varint 1B = 2B)
//                              result Data wrapper (tag 1B + len max 2B = 3B)
//                              total Response wrapper = 11B
//   Data fixed overhead:       field 1 (device uint32): tag 1B + varint 1B = 2B
//                              field 3 (age_ms uint32): tag 1B + varint max 5B = 6B
//                              field 4 (alert bool):    tag 1B + varint 1B = 2B
//                              total Data fixed = 10B
//   per Measurement (field 2): tag 1B + len 1B + Measurement body
//     field 1 (metric_id):     tag 1B + varint 1B = 2B
//     oneof val (text 12B):    tag 1B + len 1B + chars 12B = 14B (worst case vs sint32 6B)
//     Measurement body total:  16B
//   per Measurement wrapper:   tag(1) + len(1) + body(16) = 18B
//   8 full-width text metrics: 11 + 10 + 8*18 = 165B <= 186B  (all 8 fit)
//   Verify Data body length:   10 + 8*18 = 154B > 127B -> Data wrapper len uses 2 bytes (correct above)

static constexpr size_t PB_DEVICE_FIXED_OVERHEAD  = 6 + 14;  // device field + name field
static constexpr size_t PB_METRIC_ENCODED_MAX_SIZE = 35;      // worst-case per metric (unchanged)
// PB_DATA_FIXED_OVERHEAD and PB_MEASUREMENT_ENCODED_MAX_SIZE are declared in DeviceTable.h
// so that Diagnostics.cpp can use them for the budget static_assert check too.

// ---- Constructor -----------------------------------------------------------

DeviceTable::DeviceTable() {
  // Zero-initialise all slots so hasValue, metricCount, and string buffers
  // are in a known state before the first registerMetric() or applyDevice().
  memset(devices, 0, sizeof(devices));
}

// ---- Owner function --------------------------------------------------------

bool DeviceTable::registerMetric(const char* deviceName, const char* metricName,
                                  const char* unit, uint32_t scale,
                                  uint32_t& outDeviceIdx, uint32_t& outMetricId) {
  // --- Name length validation (cheap; no state change needed) ---------------
  if (strlen(deviceName) > DEVICE_TABLE_NAME_LEN - 1 ||
      strlen(metricName) > DEVICE_TABLE_NAME_LEN - 1 ||
      strlen(unit)       > DEVICE_TABLE_UNIT_LEN - 1) {
    Log_warn("DeviceTable::registerMetric - name/unit too long: device='%s' metric='%s'",
             deviceName, metricName);
    return false;
  }

  // --- Device lookup or creation --------------------------------------------
  int8_t dIdx = findByName(deviceName);
  bool newDevice = (dIdx < 0);

  if (newDevice) {
    if (deviceCount >= DEVICE_TABLE_MAX_DEVICES) {
      Log_warn("DeviceTable::registerMetric - device table full (%u slots), rejected device='%s'",
               (unsigned)DEVICE_TABLE_MAX_DEVICES, deviceName);
      return false;
    }

    // Payload size check: estimate as if we already have this new device with
    // one metric. A temporary zeroed slot is used so we don't corrupt state on
    // rejection.
    DeviceSlot tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.index = static_cast<uint32_t>(deviceCount + 1);
    size_t estimatedDevSize  = estimateEncodedDeviceSize(tmp, metricName, unit, scale);
    size_t estimatedDataSize = estimateEncodedDataSize(tmp);
    if (estimatedDevSize > LORA_MAX_PAYLOAD_LEN || estimatedDataSize > LORA_MAX_PAYLOAD_LEN) {
      Log_warn("DeviceTable::registerMetric - payload limit exceeded (dev=%u, data=%u > %u), rejected metric='%s'",
               (unsigned)estimatedDevSize, (unsigned)estimatedDataSize, (unsigned)LORA_MAX_PAYLOAD_LEN, metricName);
      return false;
    }

    dIdx = static_cast<int8_t>(deviceCount++);
    memset(&devices[dIdx], 0, sizeof(DeviceSlot));
    devices[dIdx].index = static_cast<uint32_t>(dIdx + 1);  // 1-based, stable for this table's lifetime
    strncpy(devices[dIdx].name, deviceName, DEVICE_TABLE_NAME_LEN - 1);
    devices[dIdx].name[DEVICE_TABLE_NAME_LEN - 1] = '\0';
    rev++;
    Log_info("DeviceTable: new device[%u] '%s' (rev=%u)", devices[dIdx].index, deviceName, rev);
  }

  DeviceSlot& dev = devices[dIdx];

  // --- Metric lookup or creation --------------------------------------------
  int8_t mIdx = findMetricByName(dev, metricName);
  if (mIdx < 0) {
    if (dev.metricCount >= DEVICE_TABLE_MAX_METRICS) {
      Log_warn("DeviceTable::registerMetric - metric table full (%u slots) for device='%s'",
               (unsigned)DEVICE_TABLE_MAX_METRICS, deviceName);
      return false;
    }

    // Payload size check: add the new metric on top of the existing ones.
    size_t estimatedDevSize  = estimateEncodedDeviceSize(dev, metricName, unit, scale);
    size_t estimatedDataSize = estimateEncodedDataSize(dev);
    if (estimatedDevSize > LORA_MAX_PAYLOAD_LEN || estimatedDataSize > LORA_MAX_PAYLOAD_LEN) {
      Log_warn("DeviceTable::registerMetric - payload limit exceeded (dev=%u, data=%u > %u), rejected metric='%s' on device='%s'",
               (unsigned)estimatedDevSize, (unsigned)estimatedDataSize, (unsigned)LORA_MAX_PAYLOAD_LEN, metricName, deviceName);
      return false;
    }

    mIdx = static_cast<int8_t>(dev.metricCount++);
    MetricSlot& m = dev.metrics[mIdx];
    memset(&m, 0, sizeof(MetricSlot));
    m.id = static_cast<uint32_t>(mIdx + 1);  // 1-based, stable for this device's lifetime
    strncpy(m.name, metricName, DEVICE_TABLE_NAME_LEN - 1);
    m.name[DEVICE_TABLE_NAME_LEN - 1] = '\0';
    strncpy(m.unit, unit, DEVICE_TABLE_UNIT_LEN - 1);
    m.unit[DEVICE_TABLE_UNIT_LEN - 1] = '\0';
    m.scale = scale;
    rev++;
    Log_info("DeviceTable: new metric[%u] '%s' on device '%s' (rev=%u)",
             m.id, metricName, deviceName, rev);
  }

  outDeviceIdx = dev.index;
  outMetricId  = dev.metrics[mIdx].id;
  return true;
}

// ---- Cache functions -------------------------------------------------------

DeviceTableResult DeviceTable::applyDevice(const loraiot_Device& dev) {
  // Device index 0 is the node itself and is never stored in a DeviceTable.
  // Indices above MAX_DEVICES are also out of range.
  if (dev.device == 0 || dev.device > DEVICE_TABLE_MAX_DEVICES) {
    Log_warn("DeviceTable::applyDevice - device index %u out of range [1..%u]",
             (unsigned)dev.device, (unsigned)DEVICE_TABLE_MAX_DEVICES);
    return DeviceTableResult::UNKNOWN_DEVICE;
  }

  int8_t dIdx = findByIndex(dev.device);

  if (dIdx < 0) {
    // First time we see this device index - allocate the next sequential slot.
    if (deviceCount >= DEVICE_TABLE_MAX_DEVICES) {
      Log_warn("DeviceTable::applyDevice - table full, cannot cache device %u", (unsigned)dev.device);
      return DeviceTableResult::UNKNOWN_DEVICE;
    }
    dIdx = static_cast<int8_t>(deviceCount++);
    memset(&devices[dIdx], 0, sizeof(DeviceSlot));
  }

  DeviceSlot& slot = devices[dIdx];
  slot.index = dev.device;
  strncpy(slot.name, dev.name, DEVICE_TABLE_NAME_LEN - 1);
  slot.name[DEVICE_TABLE_NAME_LEN - 1] = '\0';

  // Merge metrics with value preservation:
  //   - Build the new metric list from the Describe response.
  //   - For each incoming metric, check whether the old slot has a metric with
  //     the same id. If yes, and its unit and scale are identical, copy the
  //     stored value across. Otherwise the value starts fresh (hasValue=false).
  //
  // This ensures that a re-crawl triggered by a registry_rev change does not
  // wipe values that are still valid (e.g. only a new metric was added).
  uint8_t newCount = (dev.metrics_count <= DEVICE_TABLE_MAX_METRICS)
                     ? static_cast<uint8_t>(dev.metrics_count)
                     : DEVICE_TABLE_MAX_METRICS;

  MetricSlot merged[DEVICE_TABLE_MAX_METRICS];
  memset(merged, 0, sizeof(merged));

  for (uint8_t i = 0; i < newCount; i++) {
    const loraiot_Metric& src = dev.metrics[i];
    merged[i].id    = src.id;
    merged[i].scale = src.scale;
    strncpy(merged[i].name, src.name, DEVICE_TABLE_NAME_LEN - 1);
    merged[i].name[DEVICE_TABLE_NAME_LEN - 1] = '\0';
    strncpy(merged[i].unit, src.unit, DEVICE_TABLE_UNIT_LEN - 1);
    merged[i].unit[DEVICE_TABLE_UNIT_LEN - 1] = '\0';

    // Preserve the value only when the metric definition is byte-for-byte
    // compatible (same id, same unit string, same scale divisor).
    int8_t oldIdx = findMetricById(slot, src.id);
    if (oldIdx >= 0) {
      const MetricSlot& old = slot.metrics[oldIdx];
      if (old.scale == src.scale &&
          strncmp(old.unit, src.unit, DEVICE_TABLE_UNIT_LEN) == 0) {
        if (old.isText) {
          strncpy(merged[i].text, old.text, DEVICE_TABLE_TEXT_BUF_SIZE - 1);
          merged[i].text[DEVICE_TABLE_TEXT_BUF_SIZE - 1] = '\0';
        } else {
          merged[i].value = old.value;
        }
        merged[i].valueMs  = old.valueMs;
        merged[i].hasValue = old.hasValue;
        merged[i].isText   = old.isText;
      }
    }
  }

  memcpy(slot.metrics, merged, sizeof(merged));
  slot.metricCount = newCount;
  rev++;
  return DeviceTableResult::OK;
}

DeviceTableResult DeviceTable::applyData(const loraiot_Data& data, uint32_t nowMs) {
  // The device index in the DATA frame must already be present (populated by a
  // prior Describe crawl). If it is not found, the caller should schedule a
  // crawl to retrieve the definition before trying again.
  int8_t dIdx = findByIndex(data.device);
  if (dIdx < 0) {
    return DeviceTableResult::UNKNOWN_DEVICE;
  }

  DeviceSlot& slot = devices[dIdx];
  DeviceTableResult result = DeviceTableResult::OK;

  // Partial update: iterate only over the measurements in this frame. Metrics
  // absent from the frame keep their previous value unchanged.
  for (pb_size_t i = 0; i < data.values_count; i++) {
    const loraiot_Measurement& m = data.values[i];
    int8_t mIdx = findMetricById(slot, m.metric_id);
    if (mIdx < 0) {
      // Unknown metric_id - record the error but continue applying the rest.
      // The caller will see UNKNOWN_METRIC and can schedule a re-crawl.
      result = DeviceTableResult::UNKNOWN_METRIC;
      continue;
    }
    MetricSlot& ms = slot.metrics[mIdx];
    if (m.which_val == loraiot_Measurement_num_tag) {
      ms.value    = m.val.num;
      ms.isText   = false;
      ms.valueMs  = (nowMs >= data.age_ms) ? (nowMs - data.age_ms) : 0;
      ms.hasValue = true;
    } else if (m.which_val == loraiot_Measurement_text_tag) {
      strncpy(ms.text, m.val.text, DEVICE_TABLE_TEXT_BUF_SIZE - 1);
      ms.text[DEVICE_TABLE_TEXT_BUF_SIZE - 1] = '\0';
      ms.isText   = true;
      ms.valueMs  = (nowMs >= data.age_ms) ? (nowMs - data.age_ms) : 0;
      ms.hasValue = true;
    } else {
      Log_warn("DeviceTable::applyData - unknown measurement val tag=%u for metric=%u",
               (unsigned)m.which_val, (unsigned)m.metric_id);
    }
  }

  return result;
}

// ---- Common functions ------------------------------------------------------

void DeviceTable::storeValue(uint32_t deviceIdx, uint32_t metricId,
                              int32_t scaledValue, uint32_t nowMs) {
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].index == deviceIdx) {
      for (uint8_t m = 0; m < devices[i].metricCount; m++) {
        if (devices[i].metrics[m].id == metricId) {
          MetricSlot& slot = devices[i].metrics[m];
          slot.value    = scaledValue;
          slot.valueMs  = nowMs;
          slot.hasValue = true;
          slot.isText   = false;
          return;
        }
      }
      return;  // device found, metric not found - silent no-op per spec
    }
  }
}

void DeviceTable::storeTextValue(uint32_t deviceIdx, uint32_t metricId,
                                  const char* textValue, uint32_t nowMs) {
  if (textValue == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].index == deviceIdx) {
      for (uint8_t m = 0; m < devices[i].metricCount; m++) {
        if (devices[i].metrics[m].id == metricId) {
          MetricSlot& slot = devices[i].metrics[m];
          strncpy(slot.text, textValue, DEVICE_TABLE_TEXT_BUF_SIZE - 1);
          slot.text[DEVICE_TABLE_TEXT_BUF_SIZE - 1] = '\0';
          slot.valueMs  = nowMs;
          slot.hasValue = true;
          slot.isText   = true;
          return;
        }
      }
      return;  // device found, metric not found - silent no-op per spec
    }
  }
}

bool DeviceTable::toProtoDevice(uint32_t deviceIdx, loraiot_Device& out) const {
  int8_t dIdx = findByIndex(deviceIdx);
  if (dIdx < 0) {
    return false;
  }
  const DeviceSlot& src = devices[dIdx];
  memset(&out, 0, sizeof(out));
  out.device        = src.index;
  out.metrics_count = src.metricCount;
  strncpy(out.name, src.name, sizeof(out.name) - 1);
  for (uint8_t m = 0; m < src.metricCount; m++) {
    out.metrics[m].id    = src.metrics[m].id;
    out.metrics[m].scale = src.metrics[m].scale;
    strncpy(out.metrics[m].name, src.metrics[m].name, sizeof(out.metrics[m].name) - 1);
    strncpy(out.metrics[m].unit, src.metrics[m].unit, sizeof(out.metrics[m].unit) - 1);
  }
  return true;
}

bool DeviceTable::toProtoData(uint32_t deviceIdx, loraiot_Data& out, uint32_t nowMs) const {
  int8_t dIdx = findByIndex(deviceIdx);
  if (dIdx < 0) {
    return false;
  }
  const DeviceSlot& src = devices[dIdx];
  memset(&out, 0, sizeof(out));
  out.device = src.index;

  uint8_t  valCount = 0;
  uint32_t oldestMs = nowMs;  // tracks the earliest valueMs across all metrics
  bool     hasAny   = false;

  for (uint8_t m = 0; m < src.metricCount && valCount < 16; m++) {
    if (!src.metrics[m].hasValue) {
      continue;
    }
    out.values[valCount].metric_id = src.metrics[m].id;
    if (src.metrics[m].isText) {
      out.values[valCount].which_val = loraiot_Measurement_text_tag;
      strncpy(out.values[valCount].val.text, src.metrics[m].text, sizeof(out.values[valCount].val.text) - 1);
      out.values[valCount].val.text[sizeof(out.values[valCount].val.text) - 1] = '\0';
    } else {
      out.values[valCount].which_val = loraiot_Measurement_num_tag;
      out.values[valCount].val.num   = src.metrics[m].value;
    }
    valCount++;
    // Track the oldest absolute timestamp so age_ms reflects the stalest metric.
    if (!hasAny || src.metrics[m].valueMs < oldestMs) {
      oldestMs = src.metrics[m].valueMs;
      hasAny   = true;
    }
  }
  out.values_count = valCount;
  // age_ms = how long ago the oldest metric was measured, relative to nowMs.
  out.age_ms = (hasAny && nowMs >= oldestMs) ? (nowMs - oldestMs) : 0;
  return true;
}

bool DeviceTable::copyDevice(uint32_t deviceIdx, DeviceSlot& out) const {
  int8_t dIdx = findByIndex(deviceIdx);
  if (dIdx < 0) {
    return false;
  }
  out = devices[dIdx];
  return true;
}

void DeviceTable::clear() {
  memset(devices, 0, sizeof(devices));
  deviceCount = 0;
  rev         = 0;
}

uint32_t DeviceTable::getRev() const {
  return rev;
}

void DeviceTable::setRev(uint32_t newRev) {
  rev = newRev;
}

uint8_t DeviceTable::getDeviceCount() const {
  return deviceCount;
}

// ---- Private helpers -------------------------------------------------------

int8_t DeviceTable::findByIndex(uint32_t deviceIdx) const {
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].index == deviceIdx) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

int8_t DeviceTable::findByName(const char* deviceName) const {
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (strncmp(devices[i].name, deviceName, DEVICE_TABLE_NAME_LEN) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

int8_t DeviceTable::findMetricById(const DeviceSlot& dev, uint32_t metricId) {
  for (uint8_t i = 0; i < dev.metricCount; i++) {
    if (dev.metrics[i].id == metricId) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

int8_t DeviceTable::findMetricByName(const DeviceSlot& dev, const char* metricName) {
  for (uint8_t i = 0; i < dev.metricCount; i++) {
    if (strncmp(dev.metrics[i].name, metricName, DEVICE_TABLE_NAME_LEN) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

size_t DeviceTable::estimateEncodedDeviceSize(const DeviceSlot& dev,
                                               const char* extraMetricName,
                                               const char* extraUnit,
                                               uint32_t extraScale) {
  // Conservative worst-case estimate of the nanopb-encoded loraiot_Device size.
  // Adds one hypothetical extra metric on top of the existing ones so the caller
  // can check whether adding that metric would exceed LORA_MAX_PAYLOAD_LEN before
  // actually writing it to the slot.
  // No heap allocation - pure arithmetic on character counts and tag overhead.
  (void)extraScale;  // uint32 scale fits well inside PB_METRIC_ENCODED_MAX_SIZE budget

  size_t nameLen = strnlen(dev.name, DEVICE_TABLE_NAME_LEN);
  // field 1 (device varint): tag(1) + varint(max 5) = 6
  // field 2 (name string):   tag(1) + len(1) + nameLen
  size_t total = PB_DEVICE_FIXED_OVERHEAD + nameLen;

  // Existing metrics (worst-case per metric already includes all four subfields)
  total += static_cast<size_t>(dev.metricCount) * PB_METRIC_ENCODED_MAX_SIZE;

  // The hypothetical new metric: wrapper tag+len + subfield overhead
  size_t extraNameLen = strnlen(extraMetricName, DEVICE_TABLE_NAME_LEN);
  size_t extraUnitLen = strnlen(extraUnit, DEVICE_TABLE_UNIT_LEN);
  // tag(1)+len(1) wrapper + id(6) + name(2+extraNameLen) + unit(2+extraUnitLen) + scale(6)
  total += 2 + 6 + (2 + extraNameLen) + (2 + extraUnitLen) + 6;

  return total;
}

// Compile-time proof that DEVICE_TABLE_MAX_METRICS full-width text measurements
// always fit in one LoRa payload. If any constant changes to break this, the
// build fails here before a subtle runtime overflow can occur.
static_assert(
  PB_DATA_FIXED_OVERHEAD + DEVICE_TABLE_MAX_METRICS * PB_MEASUREMENT_ENCODED_MAX_SIZE
    <= LORA_MAX_PAYLOAD_LEN,
  "8 full-width text metrics must fit in one LoRa payload (LORA_MAX_PAYLOAD_LEN)");

size_t DeviceTable::estimateEncodedDataSize(const DeviceSlot& dev) {
  // Conservative worst-case estimate of the nanopb-encoded Data (or Response(Data))
  // with dev.metricCount + 1 metrics, each holding worst-case 12-byte text.
  // The +1 accounts for the new metric being registered: after acceptance the device
  // will have metricCount+1 metrics, and all must fit in one frame.
  return PB_DATA_FIXED_OVERHEAD + static_cast<size_t>(dev.metricCount + 1) * PB_MEASUREMENT_ENCODED_MAX_SIZE;
}
