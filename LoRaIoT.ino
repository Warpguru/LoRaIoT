/**
 * LoRaIoT.ino - Arduino IDE entry point (stub).
 *
 * All application logic lives in src/LoRaIoT/src/Application.cpp.
 * This file exists solely to satisfy the Arduino IDE requirement that
 * a .ino file matching the sketch folder name must be present.
 * It delegates setup() and loop() to the Application package so that
 * both Arduino IDE and PlatformIO share the same implementation.
 *
 * Define LORAIOT_DUAL_CORE here (and in LoRaIoT.cpp) to pin FreeRTOS
 * tasks to Core 1 on dual-core boards (ESP32, S3).
 * Leave it undefined for single-core boards (C3, C6, S2, H2, etc.).
 */

#define LORAIOT_DUAL_CORE   // uncomment for dual-core boards (ESP32, S3)

#include "src/LoRaIoT/src/Application.h"

void setup() {
  applicationSetup();
}

void loop() {
  applicationLoop();
}
