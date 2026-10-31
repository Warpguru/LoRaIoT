#include "ControlSetpoint.h"

#include <string.h>

#include "../../registry/src/DeviceTable.h"
#include "../../registry/src/PeerRegistry.h"

bool controlApplyMetric(uint8_t addr, const char* deviceName, const char* metricName,
                        JsonVariant metric,
                        uint32_t& outDeviceIdx, uint32_t& outMetricId) {
  JsonVariant valVar = metric["value"];
  const char* unit = metric["unit"].is<const char*>() ? metric["unit"].as<const char*>() : "";
  uint32_t scale   = metric["scale"].is<uint32_t>()   ? metric["scale"].as<uint32_t>()   : 1;
  if (scale == 0) {
    scale = 1;
  }

  bool isText = false;
  const char* textValue = nullptr;
  int32_t numValue = 0;
  bool valid = false;

  if (valVar.is<bool>()) {
    isText = true;
    textValue = valVar.as<bool>() ? "true" : "false";
    valid = true;
  } else if (valVar.is<const char*>()) {
    isText = true;
    textValue = valVar.as<const char*>();
    valid = (textValue != nullptr && strlen(textValue) <= DEVICE_TABLE_TEXT_MAX_LEN);
  } else if (valVar.is<int32_t>() || valVar.is<float>() || valVar.is<double>()) {
    isText = false;
    numValue = valVar.as<int32_t>();
    valid = true;
  }

  if (!valid) {
    return false;
  }

  if (!ControlRegistry.registerMetric(addr, deviceName, metricName, unit, scale,
                                      outDeviceIdx, outMetricId)) {
    return false;
  }

  if (isText) {
    ControlRegistry.storeTextValue(addr, outDeviceIdx, outMetricId, textValue);
  } else {
    ControlRegistry.storeValue(addr, outDeviceIdx, outMetricId, numValue);
  }
  return true;
}
