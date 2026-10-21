#include "SensorRegistry.h"

#include "../../util/src/LogBuffer.h"

// Global singleton memory allocation
SensorRegistryClass SensorRegistry;

// ---- nanopb size estimation constants --------------------------------------
// Protobuf varint / tag overhead per field (conservative worst-case per field).
// Used by estimateEncodedDeviceSize() to stay safe without running the encoder.

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

static constexpr size_t PB_DEVICE_FIXED_OVERHEAD  = 6 + 14;  // device field + name field
static constexpr size_t PB_METRIC_ENCODED_MAX_SIZE = 35;      // worst-case per metric

void SensorRegistryClass::begin() {
  memset(devices, 0, sizeof(devices));
  deviceCount = 0;
  registryRev = 0;
  mutex = xSemaphoreCreateMutex();
  if (mutex == nullptr) {
    Log_error("SensorRegistry::begin - Failed to create mutex!");
  }
}

bool SensorRegistryClass::registerMetric(const char* deviceName, const char* metricName,
                                          const char* unit, uint32_t scale,
                                          uint32_t& outDeviceIdx, uint32_t& outMetricId) {
  if (mutex == nullptr) return false;

  // --- Name length validation (outside the mutex; no state change) ---
  if (strlen(deviceName) > SENSOR_REGISTRY_NAME_LEN - 1 ||
      strlen(metricName) > SENSOR_REGISTRY_NAME_LEN - 1 ||
      strlen(unit)       > SENSOR_REGISTRY_UNIT_LEN - 1) {
    Log_warn("SensorRegistry::registerMetric - name/unit too long: device='%s' metric='%s'",
             deviceName, metricName);
    return false;
  }

  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t dIdx = findDevice(deviceName);
  bool newDevice = (dIdx < 0);

  if (newDevice) {
    if (deviceCount >= SENSOR_REGISTRY_MAX_DEVICES) {
      Log_warn("SensorRegistry::registerMetric - device table full, rejected device='%s'", deviceName);
      xSemaphoreGive(mutex);
      return false;
    }

    // Check payload size: a brand-new device with one metric
    size_t estimatedSize = estimateEncodedDeviceSize(
        // Use a temporary zeroed device to represent the new entry
        RegisteredDevice{static_cast<uint32_t>(deviceCount + 1), {}, 0, {}},
        metricName, unit, scale);
    if (estimatedSize > LORA_MAX_PAYLOAD_LEN) {
      Log_warn("SensorRegistry::registerMetric - payload limit exceeded (%u > %u), rejected metric='%s'",
               (unsigned)estimatedSize, (unsigned)LORA_MAX_PAYLOAD_LEN, metricName);
      xSemaphoreGive(mutex);
      return false;
    }

    dIdx = static_cast<int8_t>(deviceCount++);
    memset(&devices[dIdx], 0, sizeof(RegisteredDevice));
    devices[dIdx].deviceIdx = static_cast<uint32_t>(dIdx + 1);  // 1-based
    strncpy(devices[dIdx].name, deviceName, SENSOR_REGISTRY_NAME_LEN - 1);
    devices[dIdx].name[SENSOR_REGISTRY_NAME_LEN - 1] = '\0';
    registryRev++;
    Log_info("SensorRegistry: new device[%u] '%s' (rev=%u)", devices[dIdx].deviceIdx, deviceName, registryRev);
  }

  RegisteredDevice& dev = devices[dIdx];

  int8_t mIdx = findMetric(dev, metricName);
  if (mIdx < 0) {
    // New metric: check payload size before inserting
    if (dev.metricCount >= SENSOR_REGISTRY_MAX_METRICS) {
      Log_warn("SensorRegistry::registerMetric - metric table full for device='%s'", deviceName);
      xSemaphoreGive(mutex);
      return false;
    }

    size_t estimatedSize = estimateEncodedDeviceSize(dev, metricName, unit, scale);
    if (estimatedSize > LORA_MAX_PAYLOAD_LEN) {
      Log_warn("SensorRegistry::registerMetric - payload limit exceeded (%u > %u), rejected metric='%s' on device='%s'",
               (unsigned)estimatedSize, (unsigned)LORA_MAX_PAYLOAD_LEN, metricName, deviceName);
      xSemaphoreGive(mutex);
      return false;
    }

    mIdx = static_cast<int8_t>(dev.metricCount++);
    RegisteredMetric& m = dev.metrics[mIdx];
    memset(&m, 0, sizeof(RegisteredMetric));
    m.id = static_cast<uint32_t>(mIdx + 1);  // 1-based
    strncpy(m.name, metricName, SENSOR_REGISTRY_NAME_LEN - 1);
    m.name[SENSOR_REGISTRY_NAME_LEN - 1] = '\0';
    strncpy(m.unit, unit, SENSOR_REGISTRY_UNIT_LEN - 1);
    m.unit[SENSOR_REGISTRY_UNIT_LEN - 1] = '\0';
    m.scale = scale;
    registryRev++;
    Log_info("SensorRegistry: new metric[%u] '%s' on device '%s' (rev=%u)",
             m.id, metricName, deviceName, registryRev);
  }

  outDeviceIdx = dev.deviceIdx;
  outMetricId  = dev.metrics[mIdx].id;

  xSemaphoreGive(mutex);
  return true;
}

void SensorRegistryClass::storeValue(uint32_t deviceIdx, uint32_t metricId, int32_t scaledValue) {
  if (mutex == nullptr) return;
  xSemaphoreTake(mutex, portMAX_DELAY);

  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].deviceIdx == deviceIdx) {
      for (uint8_t m = 0; m < devices[i].metricCount; m++) {
        if (devices[i].metrics[m].id == metricId) {
          devices[i].metrics[m].lastValue   = scaledValue;
          devices[i].metrics[m].lastValueMs = millis();
          devices[i].metrics[m].hasValue    = true;
          xSemaphoreGive(mutex);
          return;
        }
      }
      break;
    }
  }

  xSemaphoreGive(mutex);
}

uint32_t SensorRegistryClass::getRegistryRev() const {
  return registryRev;
}

uint8_t SensorRegistryClass::getDeviceCount() const {
  return deviceCount;
}

bool SensorRegistryClass::getDevice(uint32_t deviceIdx, RegisteredDevice& out) const {
  if (mutex == nullptr) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);

  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].deviceIdx == deviceIdx) {
      out = devices[i];
      xSemaphoreGive(mutex);
      return true;
    }
  }

  xSemaphoreGive(mutex);
  return false;
}

bool SensorRegistryClass::toProtoDevice(uint32_t deviceIdx, loraiot_Device& out) const {
  if (mutex == nullptr) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);

  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].deviceIdx == deviceIdx) {
      const RegisteredDevice& src = devices[i];
      memset(&out, 0, sizeof(out));
      out.device = src.deviceIdx;
      strncpy(out.name, src.name, sizeof(out.name) - 1);
      out.metrics_count = src.metricCount;
      for (uint8_t m = 0; m < src.metricCount; m++) {
        out.metrics[m].id    = src.metrics[m].id;
        out.metrics[m].scale = src.metrics[m].scale;
        strncpy(out.metrics[m].name, src.metrics[m].name, sizeof(out.metrics[m].name) - 1);
        strncpy(out.metrics[m].unit, src.metrics[m].unit, sizeof(out.metrics[m].unit) - 1);
      }
      xSemaphoreGive(mutex);
      return true;
    }
  }

  xSemaphoreGive(mutex);
  return false;
}

bool SensorRegistryClass::toProtoData(uint32_t deviceIdx, loraiot_Data& out) const {
  if (mutex == nullptr) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);

  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].deviceIdx == deviceIdx) {
      const RegisteredDevice& src = devices[i];
      memset(&out, 0, sizeof(out));
      out.device = src.deviceIdx;
      uint8_t valCount = 0;
      for (uint8_t m = 0; m < src.metricCount && valCount < 16; m++) {
        if (src.metrics[m].hasValue) {
          out.values[valCount].metric_id = src.metrics[m].id;
          out.values[valCount].value     = src.metrics[m].lastValue;
          valCount++;
        }
      }
      out.values_count = valCount;
      // age_ms: time since the oldest value was stored - use the first one as proxy
      if (valCount > 0 && src.metrics[0].hasValue) {
        uint32_t now = millis();
        uint32_t stored = src.metrics[0].lastValueMs;
        out.age_ms = (now >= stored) ? (now - stored) : 0;
      }
      xSemaphoreGive(mutex);
      return true;
    }
  }

  xSemaphoreGive(mutex);
  return false;
}

// ----- private helpers -------------------------------------------------------

int8_t SensorRegistryClass::findDevice(const char* deviceName) const {
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (strncmp(devices[i].name, deviceName, SENSOR_REGISTRY_NAME_LEN) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

int8_t SensorRegistryClass::findMetric(const RegisteredDevice& dev, const char* metricName) {
  for (uint8_t i = 0; i < dev.metricCount; i++) {
    if (strncmp(dev.metrics[i].name, metricName, SENSOR_REGISTRY_NAME_LEN) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

size_t SensorRegistryClass::estimateEncodedDeviceSize(const RegisteredDevice& dev,
                                                        const char* extraMetricName,
                                                        const char* extraUnit,
                                                        uint32_t extraScale) {
  // Conservative worst-case estimate of nanopb-encoded loraiot_Device size.
  // Adds one hypothetical extra metric on top of the existing ones.
  // No heap allocation - pure arithmetic on field sizes.
  (void)extraScale;  // scale is a small uint32, fits in PB_METRIC_ENCODED_MAX_SIZE budget

  size_t nameLen  = strnlen(dev.name, SENSOR_REGISTRY_NAME_LEN);
  // field 1 (device varint): tag(1) + varint(max 5)
  // field 2 (name string):   tag(1) + len(1) + nameLen
  size_t total = 6 + 2 + nameLen;

  // Existing metrics
  total += static_cast<size_t>(dev.metricCount) * PB_METRIC_ENCODED_MAX_SIZE;

  // Hypothetical new metric
  size_t extraNameLen = strnlen(extraMetricName, SENSOR_REGISTRY_NAME_LEN);
  size_t extraUnitLen = strnlen(extraUnit, SENSOR_REGISTRY_UNIT_LEN);
  // tag(1)+len(1) wrapper + id(6) + name(2+extraNameLen) + unit(2+extraUnitLen) + scale(6)
  size_t extraMetricSize = 2 + 6 + (2 + extraNameLen) + (2 + extraUnitLen) + 6;
  total += extraMetricSize;

  return total;
}
