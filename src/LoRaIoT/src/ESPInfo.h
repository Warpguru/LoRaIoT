#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_chip_info.h>
#include <esp_partition.h>
#include <esp_system.h>

/**
 * ESPInfo.h - ESP32 hardware and diagnostics information.
 *
 * Provides helper functions and ArduinoJson serialisation for ESP32 diagnostics
 * metrics exposed via GET /status.
 *
 * No Java equivalent - this is ESP32-specific platform diagnostics.
 * The Java application has no /status endpoint; it uses the OS and JVM for
 * diagnostics (e.g. JVM heap, OS serial port enumeration).
 */

/**
 * Helper: Get Reset Reason as String.
 */
inline const char* getResetReasonString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:
      return "Unknown";
    case ESP_RST_POWERON:
      return "Power On";
    case ESP_RST_EXT:
      return "External Pin";
    case ESP_RST_SW:
      return "Software";
    case ESP_RST_PANIC:
      return "Exception/Panic";
    case ESP_RST_INT_WDT:
      return "Interrupt Watchdog";
    case ESP_RST_TASK_WDT:
      return "Task Watchdog";
    case ESP_RST_WDT:
      return "Other Watchdog";
    case ESP_RST_DEEPSLEEP:
      return "Deep Sleep Wakeup";
    case ESP_RST_BROWNOUT:
      return "Brownout";
    case ESP_RST_SDIO:
      return "SDIO";
    default:
      return "Other";
  }
}

/**
 * Helper: Get Flash Mode as String.
 */
inline const char* getFlashModeString(uint32_t mode) {
  switch (mode) {
    case 0:
      return "QIO";
    case 1:
      return "QOUT";
    case 2:
      return "DIO";
    case 3:
      return "DOUT";
    default:
      return "Unknown";
  }
}

/**
 * Populates hardware and diagnostic metrics into the provided ArduinoJson JsonObject.
 * Called from Server.cpp's GET /status handler.
 */
inline void fillESPInfo(JsonObject obj) {
  esp_chip_info_t chipInfo;
  esp_chip_info(&chipInfo);

  // Features string construction
  String features = "";
  if (chipInfo.features & CHIP_FEATURE_WIFI_BGN) {
    features += "WiFi ";
  }
  if (chipInfo.features & CHIP_FEATURE_BT) {
    features += "BT ";
  }
  if (chipInfo.features & CHIP_FEATURE_BLE) {
    features += "BLE ";
  }
  if (chipInfo.features & CHIP_FEATURE_IEEE802154) {
    features += "802.15.4 ";
  }
  if (chipInfo.features & CHIP_FEATURE_EMB_FLASH) {
    features += "Embedded-Flash";
  } else {
    features += "External-Flash";
  }
  features.trim();

  // Efuse MAC formatting (byte-swapped to MSB-first colon notation)
  uint64_t chipid = ESP.getEfuseMac();
  char efuseMacBuf[18];
  snprintf(efuseMacBuf, sizeof(efuseMacBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           (uint8_t)(chipid), (uint8_t)(chipid >> 8), (uint8_t)(chipid >> 16),
           (uint8_t)(chipid >> 24), (uint8_t)(chipid >> 32), (uint8_t)(chipid >> 40));

  // Chip Info
  JsonObject chip = obj["chip"].to<JsonObject>();
  chip["model"] = ESP.getChipModel();
  chip["revision"] = ESP.getChipRevision();
  chip["cores"] = chipInfo.cores;
  chip["cpuFreqMHz"] = ESP.getCpuFreqMHz();
  chip["resetReason"] = getResetReasonString(esp_reset_reason());
  chip["features"] = features;

  // Flash Info
  uint32_t flash_size = ESP.getFlashChipSize();
  JsonObject flash = obj["flash"].to<JsonObject>();
  flash["sizeMB"] = flash_size / (1024 * 1024);
  flash["speedMHz"] = ESP.getFlashChipSpeed() / 1000000;
  flash["mode"] = getFlashModeString(ESP.getFlashChipMode());

  // Memory Info
  JsonObject memory = obj["memory"].to<JsonObject>();
  memory["heapSize"] = ESP.getHeapSize();
  memory["freeHeap"] = ESP.getFreeHeap();
  memory["minFreeHeap"] = ESP.getMinFreeHeap();
  memory["maxAllocHeap"] = ESP.getMaxAllocHeap();
  if (psramFound()) {
    memory["psramSize"] = ESP.getPsramSize();
    memory["freePsram"] = ESP.getFreePsram();
    memory["psramStatus"] = "Available";
  } else {
    memory["psramSize"] = 0;
    memory["freePsram"] = 0;
    memory["psramStatus"] = "Not Found / Disabled";
  }

  // Sketch Info
  JsonObject sketch = obj["sketch"].to<JsonObject>();
  sketch["size"] = ESP.getSketchSize();
  sketch["freeSpace"] = ESP.getFreeSketchSpace();

  // Partition Table Map
  JsonArray partitions = obj["partitions"].to<JsonArray>();
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL) {
    const esp_partition_t* p = esp_partition_get(it);
    JsonObject part = partitions.add<JsonObject>();
    part["label"] = p->label;
    part["type"] = (p->type == 0) ? "App" : "Data";
    char addrBuf[12];
    snprintf(addrBuf, sizeof(addrBuf), "0x%06X", (unsigned int)p->address);
    part["address"] = addrBuf;
    part["sizeKB"] = (int)p->size / 1024;
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);

  // System & Sensors
  JsonObject system = obj["system"].to<JsonObject>();
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
  system["internalTemperature"] = temperatureRead();
#else
  system["internalTemperature"] = nullptr;
#endif
  system["cycleCount"] = ESP.getCycleCount();
  system["sdkVersion"] = ESP.getSdkVersion();

  // Network / MACs
  JsonObject network = obj["network"].to<JsonObject>();
  network["efuseMac"] = efuseMacBuf;
  network["staMac"] = WiFi.macAddress();
  network["apMac"] = WiFi.softAPmacAddress();
  if (WiFi.status() == WL_CONNECTED) {
    network["connected"] = true;
    network["ip"] = WiFi.localIP().toString();
    network["subnetMask"] = WiFi.subnetMask().toString();
    network["gateway"] = WiFi.gatewayIP().toString();
    network["rssi"] = WiFi.RSSI();
    network["ssid"] = WiFi.SSID();
  } else {
    network["connected"] = false;
    network["ip"] = "0.0.0.0";
    network["subnetMask"] = "0.0.0.0";
    network["gateway"] = "0.0.0.0";
    network["rssi"] = 0;
    network["ssid"] = "";
  }
}
