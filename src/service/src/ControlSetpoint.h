#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

/**
 * ControlSetpoint.h - shared per-metric control setpoint logic (LOCAL only).
 *
 * One metric object, as used in the body of POST /api/lora/nodes/control and as the payload
 * of an MQTT <prefix>/Local/Remote_Node_<n>/<device>/<metric>/set message:
 *
 *   {"value":336,"unit":"V","scale":100}     numeric, scaled integer
 *   {"value":"online"}                       text (also true/false, stored as "true"/"false")
 *
 * "unit" (default "") and "scale" (default 1, 0 is treated as 1) are optional.
 *
 * Keeping this in one place guarantees that REST and MQTT register and store a setpoint
 * identically. Pushing the result to the REMOTE is NOT done here: REST pushes inline,
 * MQTT hands the push off to PushTask so mqttTask never waits on the LoRa link.
 */

/**
 * Register the metric in ControlRegistry[addr] and store its value.
 *
 * @param addr          REMOTE LoRa address the control data is addressed to.
 * @param deviceName    Device name (max DEVICE_TABLE_NAME_LEN - 1 chars).
 * @param metricName    Metric name (max DEVICE_TABLE_NAME_LEN - 1 chars).
 * @param metric        The metric object ({"value":...,"unit":...,"scale":...}).
 * @param outDeviceIdx  Set to the device index on success.
 * @param outMetricId   Set to the metric id on success.
 * @return true if the metric was registered and the value stored; false if the value is
 *         missing or invalid (wrong type, text longer than DEVICE_TABLE_TEXT_MAX_LEN) or the
 *         registry rejected the metric (table full, name too long, payload too large).
 */
bool controlApplyMetric(uint8_t addr, const char* deviceName, const char* metricName,
                        JsonVariant metric,
                        uint32_t& outDeviceIdx, uint32_t& outMetricId);
