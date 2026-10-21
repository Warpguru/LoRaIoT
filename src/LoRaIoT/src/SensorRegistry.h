#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../lora/src/nanopb/loraiot.pb.h"
#include "../../lora/src/LoRaTransport.h"   // LORA_MAX_PAYLOAD_LEN

/**
 * SensorRegistry.h - REMOTE node sensor ingestion registry.
 *
 * IoT sensor nodes connect to REMOTE's WiFi SoftAP and POST JSON payloads to
 * POST /api/sensor/data. This registry:
 *   1. Parses the incoming key-value pairs.
 *   2. Auto-assigns a stable device index (1..N) per device name key.
 *   3. Auto-assigns a stable metric_id (1..M) per metric key within a device.
 *   4. Increments registry_rev on every schema change (new device or new metric).
 *   5. Stores the most-recent measurement value per metric.
 *   6. Enforces the LoRa payload limit at registration time:
 *      registerMetric() returns false if adding the new metric would produce an
 *      encoded loraiot_Device proto > LORA_MAX_PAYLOAD_LEN bytes.
 *
 * Only exists on REMOTE. LOCAL uses DeviceRegistry instead.
 *
 * Thread safety: all public methods are protected by an internal FreeRTOS mutex.
 *
 * Capacity limits (matching the protobuf schema):
 *   - SENSOR_REGISTRY_MAX_DEVICES SoftAP devices per REMOTE (device indices 1..N)
 *   - SENSOR_REGISTRY_MAX_METRICS metrics per device (matches loraiot_Device.metrics[8])
 */

// Maximum SoftAP-connected devices registered on REMOTE (device indices 1..4).
// Device index 0 is reserved for the REMOTE node itself and is never in this table.
static constexpr uint8_t SENSOR_REGISTRY_MAX_DEVICES = 4;

// Maximum metrics per SoftAP device (matches loraiot_Device.metrics[8])
static constexpr uint8_t SENSOR_REGISTRY_MAX_METRICS = 8;

// Maximum device/metric name length (matches loraiot proto: max 12 chars + NUL)
static constexpr uint8_t SENSOR_REGISTRY_NAME_LEN = 13;

// Maximum unit string length (matches loraiot proto: max 5 chars + NUL)
static constexpr uint8_t SENSOR_REGISTRY_UNIT_LEN = 6;

/**
 * RegisteredMetric - one metric slot on a registered SoftAP device.
 */
struct RegisteredMetric {
  uint32_t id;                              // Stable metric_id (1-based, unique within device)
  char     name[SENSOR_REGISTRY_NAME_LEN]; // Human-readable key, e.g. "Voltage"
  char     unit[SENSOR_REGISTRY_UNIT_LEN]; // Unit string, e.g. "V". May be empty
  uint32_t scale;                           // Divisor: real = value / scale (0 treated as 1)
  int32_t  lastValue;                       // Most recent scaled integer value
  uint32_t lastValueMs;                     // millis() when lastValue was stored
  bool     hasValue;                        // true once at least one value has been stored
};

/**
 * RegisteredDevice - one SoftAP-connected device registered on REMOTE.
 */
struct RegisteredDevice {
  uint32_t         deviceIdx;                               // Stable device index (1-based)
  char             name[SENSOR_REGISTRY_NAME_LEN];         // Device name, e.g. "Sensor A"
  uint8_t          metricCount;
  RegisteredMetric metrics[SENSOR_REGISTRY_MAX_METRICS];
};

/**
 * SensorRegistryClass - singleton that owns all RegisteredDevice records on REMOTE.
 *
 * The global singleton is `SensorRegistry` (defined in SensorRegistry.cpp).
 */
class SensorRegistryClass {
 public:
  /**
   * Initialize the registry, create the internal FreeRTOS mutex, and reset
   * registry_rev to 0. Call once from applicationSetup() on REMOTE nodes.
   */
  void begin();

  /**
   * Register or update a device+metric schema entry from an incoming SoftAP payload.
   *
   * Parameters:
   *   deviceName  - Null-terminated device name (max 12 chars).
   *   metricName  - Null-terminated metric name (max 12 chars).
   *   unit        - Null-terminated unit string (max 5 chars). May be "".
   *   scale       - Divisor for real-value reconstruction. 0 treated as 1.
   *   outDeviceIdx - Set to the assigned device index on success.
   *   outMetricId  - Set to the assigned metric_id on success.
   *
   * Returns:
   *   true  - Registration accepted. Call storeValue() to persist the measurement.
   *   false - Rejected: table full, name too long, or adding this metric would
   *           produce an encoded loraiot_Device proto > LORA_MAX_PAYLOAD_LEN bytes.
   *
   * Increments registry_rev on any schema change (new device or new metric added).
   */
  bool registerMetric(const char* deviceName, const char* metricName,
                      const char* unit, uint32_t scale,
                      uint32_t& outDeviceIdx, uint32_t& outMetricId);

  /**
   * Store a measured value for an already-registered metric.
   * No-op if deviceIdx / metricId are unknown.
   */
  void storeValue(uint32_t deviceIdx, uint32_t metricId, int32_t scaledValue);

  /** Current registry revision. Incremented on every schema change. */
  uint32_t getRegistryRev() const;

  /** Number of registered SoftAP devices. */
  uint8_t getDeviceCount() const;

  /**
   * Return a copy of the RegisteredDevice for the given device index (1-based).
   * Returns false if not found.
   */
  bool getDevice(uint32_t deviceIdx, RegisteredDevice& out) const;

  /**
   * Fill a loraiot_Device proto struct from the registered definition.
   * Used by the frame encoder to build Describe responses.
   * Returns false if deviceIdx is not found.
   */
  bool toProtoDevice(uint32_t deviceIdx, loraiot_Device& out) const;

  /**
   * Fill a loraiot_Data proto struct with the most recent values for a device.
   * Returns false if deviceIdx is not found.
   */
  bool toProtoData(uint32_t deviceIdx, loraiot_Data& out) const;

 private:
  RegisteredDevice  devices[SENSOR_REGISTRY_MAX_DEVICES];
  uint8_t           deviceCount = 0;
  uint32_t          registryRev = 0;
  SemaphoreHandle_t mutex = nullptr;

  /** Find the slot index for deviceName, or -1 if absent. */
  int8_t findDevice(const char* deviceName) const;

  /** Find the metric slot index within a device for metricName, or -1 if absent. */
  static int8_t findMetric(const RegisteredDevice& dev, const char* metricName);

  /**
   * Estimate the nanopb-encoded byte size of a loraiot_Device, including a
   * hypothetical extra metric (extraMetricName, extraUnit, extraScale).
   * Used by registerMetric() to enforce LORA_MAX_PAYLOAD_LEN before accepting
   * a new metric entry.
   */
  static size_t estimateEncodedDeviceSize(const RegisteredDevice& dev,
                                           const char* extraMetricName,
                                           const char* extraUnit,
                                           uint32_t extraScale);
};

/** Global singleton. Include SensorRegistry.h and use SensorRegistry.* anywhere on REMOTE. */
extern SensorRegistryClass SensorRegistry;
