#pragma once

#include <Arduino.h>

#include "../../lora/src/nanopb/loraiot.pb.h"
#include "../../lora/src/LoRaFrame.h"   // LORA_MAX_PAYLOAD_LEN

/**
 * DeviceTable.h - Pure data structure for device/metric storage with no mutex.
 *
 * One DeviceTable holds up to DEVICE_TABLE_MAX_DEVICES device slots, each with
 * up to DEVICE_TABLE_MAX_METRICS metric slots. Each metric slot stores one value
 * and its timestamp independently (allowing partial updates).
 *
 * Roles:
 *   Owner - calls registerMetric() to create entries from incoming REST/SoftAP
 *           payloads. Enforces name-length and payload-size limits.
 *   Cache - calls applyDevice() to absorb a loraiot_Device from a Describe
 *           response, and applyData() to absorb a loraiot_Data from a DATA frame
 *           (partial: only the metrics named in the frame are updated).
 *
 * Thread safety: NONE. The caller (PeerRegistry, Step 6) holds a mutex around
 * all table accesses.
 *
 * Capacity limits (matching the protobuf schema):
 *   DEVICE_TABLE_MAX_DEVICES  4  (device indices 1..4)
 *   DEVICE_TABLE_MAX_METRICS  8  (matches loraiot_Device.metrics[8])
 *   DEVICE_TABLE_NAME_LEN    13  (max 12 chars + NUL, matches loraiot proto)
 *   DEVICE_TABLE_UNIT_LEN     6  (max 5 chars + NUL, matches loraiot proto)
 */

// Maximum devices per table (device indices 1..4; index 0 is reserved for the node itself).
static constexpr uint8_t DEVICE_TABLE_MAX_DEVICES = 4;

// Maximum metrics per device (matches loraiot_Device.metrics[8]).
static constexpr uint8_t DEVICE_TABLE_MAX_METRICS = 8;

// Maximum device/metric name length (matches loraiot proto: max 12 chars + NUL).
static constexpr uint8_t DEVICE_TABLE_NAME_LEN = 13;

// Maximum unit string length (matches loraiot proto: max 5 chars + NUL).
static constexpr uint8_t DEVICE_TABLE_UNIT_LEN = 6;

/**
 * Result codes returned by cache functions.
 */
enum class DeviceTableResult : uint8_t {
  OK             = 0,  // Operation succeeded
  UNKNOWN_DEVICE = 1,  // Device index not present in the table
  UNKNOWN_METRIC = 2,  // Metric id not present in the device slot
};

/**
 * MetricSlot - one metric definition plus its most recent stored value.
 */
struct MetricSlot {
  uint32_t id;                            // Stable metric_id (1-based, unique within device)
  char     name[DEVICE_TABLE_NAME_LEN];  // Human-readable key, e.g. "Voltage"
  char     unit[DEVICE_TABLE_UNIT_LEN];  // Unit string, e.g. "V". May be empty
  uint32_t scale;                         // Divisor: real = value / scale (0 treated as 1)
  int32_t  value;                         // Most recent scaled integer value
  uint32_t valueMs;                       // millis() when value was stored
  bool     hasValue;                      // true once at least one value has been stored
};

/**
 * DeviceSlot - one device definition and all its metric slots.
 */
struct DeviceSlot {
  uint32_t   index;                               // Stable device index (1-based)
  char       name[DEVICE_TABLE_NAME_LEN];         // Device name, e.g. "Sensor A"
  uint8_t    metricCount;
  MetricSlot metrics[DEVICE_TABLE_MAX_METRICS];
};

/**
 * DeviceTable - the shared building block used by PeerRegistry
 * and (after Step 6) PeerRegistry.
 *
 * No heap allocation. All storage is inline (about 1.4 KB per table instance).
 */
class DeviceTable {
 public:
  DeviceTable();

  // ---- Owner functions (caller creates entries via REST/SoftAP) --------------

  /**
   * Register or update a device+metric entry from an incoming REST payload.
   *
   * Parameters:
   *   deviceName   - Null-terminated device name (max 12 chars).
   *   metricName   - Null-terminated metric name (max 12 chars).
   *   unit         - Null-terminated unit string (max 5 chars). May be "".
   *   scale        - Divisor for real-value reconstruction. 0 treated as 1.
   *   outDeviceIdx - Set to the assigned device index on success.
   *   outMetricId  - Set to the assigned metric_id on success.
   *
   * Returns:
   *   true  - Registration accepted. Call storeValue() to persist the measurement.
   *   false - Rejected: table full, name too long, or adding this metric would
   *           produce an encoded loraiot_Device proto > LORA_MAX_PAYLOAD_LEN bytes.
   *
   * Increments rev on any schema change (new device or new metric added).
   */
  bool registerMetric(const char* deviceName, const char* metricName,
                      const char* unit, uint32_t scale,
                      uint32_t& outDeviceIdx, uint32_t& outMetricId);

  // ---- Cache functions (caller absorbs data received over LoRa) --------------

  /**
   * Absorb a loraiot_Device definition received from a Describe response.
   *
   * - A device whose index is already present is updated in place.
   * - Metrics whose id, unit and scale are unchanged keep their stored values.
   * - Metrics that are new or changed have their values cleared.
   * - Increments rev.
   *
   * Returns OK, or UNKNOWN_DEVICE if the device index in dev is 0 or > MAX_DEVICES.
   */
  DeviceTableResult applyDevice(const loraiot_Device& dev);

  /**
   * Absorb a loraiot_Data frame received from a DATA push.
   *
   * Only the metrics named in data are updated (partial update). Other metrics
   * in the same device slot keep their previous value unchanged.
   *
   * Parameters:
   *   data   - The decoded frame payload.
   *   nowMs  - millis() at the moment of receipt; the stored valueMs is
   *            nowMs - data.age_ms (clamped to 0 if underflow).
   *
   * Returns:
   *   OK             - All metrics in data were applied.
   *   UNKNOWN_DEVICE - data.device index not present in the table.
   *   UNKNOWN_METRIC - At least one metric_id in data.values was not found.
   *                    All known metrics from this frame were still applied.
   */
  DeviceTableResult applyData(const loraiot_Data& data, uint32_t nowMs);

  // ---- Common functions ------------------------------------------------------

  /**
   * Store a measured value for an already-registered metric.
   * No-op if deviceIdx / metricId are unknown.
   */
  void storeValue(uint32_t deviceIdx, uint32_t metricId, int32_t scaledValue, uint32_t nowMs);

  /**
   * Fill a loraiot_Device proto struct from the stored definition.
   * Returns false if deviceIdx is not found.
   */
  bool toProtoDevice(uint32_t deviceIdx, loraiot_Device& out) const;

  /**
   * Fill a loraiot_Data proto struct with all present values for a device.
   * age_ms is derived from the oldest valueMs present in the device.
   * Returns false if deviceIdx is not found.
   */
  bool toProtoData(uint32_t deviceIdx, loraiot_Data& out, uint32_t nowMs) const;

  /**
   * Copy the DeviceSlot for deviceIdx into out.
   * Returns false if deviceIdx is not found.
   */
  bool copyDevice(uint32_t deviceIdx, DeviceSlot& out) const;

  /** Reset all device slots and revision to zero. */
  void clear();

  /** Current revision counter. Incremented by registerMetric() and applyDevice(). */
  uint32_t getRev() const;

  /** Overwrite the revision counter (used by the cache role to copy the owner's rev). */
  void setRev(uint32_t newRev);

  /** Number of occupied device slots. */
  uint8_t getDeviceCount() const;

 private:
  DeviceSlot devices[DEVICE_TABLE_MAX_DEVICES];
  uint8_t    deviceCount = 0;
  uint32_t   rev         = 0;

  /** Find the slot index for deviceIdx, or -1 if absent. */
  int8_t findByIndex(uint32_t deviceIdx) const;

  /** Find the slot index for deviceName, or -1 if absent. */
  int8_t findByName(const char* deviceName) const;

  /** Find the metric slot index within a device for metricId, or -1 if absent. */
  static int8_t findMetricById(const DeviceSlot& dev, uint32_t metricId);

  /** Find the metric slot index within a device for metricName, or -1 if absent. */
  static int8_t findMetricByName(const DeviceSlot& dev, const char* metricName);

  /**
   * Estimate the nanopb-encoded byte size of a loraiot_Device, including a
   * hypothetical extra metric (extraMetricName, extraUnit, extraScale).
   * Used by registerMetric() to enforce LORA_MAX_PAYLOAD_LEN before accepting
   * a new metric entry.
   */
  static size_t estimateEncodedDeviceSize(const DeviceSlot& dev,
                                          const char* extraMetricName,
                                          const char* extraUnit,
                                          uint32_t extraScale);
};
