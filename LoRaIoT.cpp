/**
 * LoRaIoT.cpp - PlatformIO entry point (stub).
 *
 * All application logic lives in src/LoRaIoT/src/Application.cpp.
 * This file is compiled only under PlatformIO; Arduino IDE defines the
 * ARDUINO macro so the #ifndef guard makes this file an empty translation
 * unit in that environment, preventing duplicate setup()/loop() symbols.
 * It delegates setup() and loop() to the Application package so that
 * both PlatformIO and Arduino IDE share the same implementation.
 *
 * Define LORAIOT_DUAL_CORE here (and in LoRaIoT.ino) to pin FreeRTOS
 * tasks to Core 1 on dual-core boards (ESP32, S3).
 * Leave it undefined for single-core boards (C3, C6, S2, H2, etc.).
 * PlatformIO users can also set this via build_flags in platformio.ini.
 */

#define LORAIOT_DUAL_CORE   // uncomment for dual-core boards (ESP32, S3)

#ifndef ARDUINO

#include <Arduino.h>

#include "src/LoRaIoT/src/Application.h"

void setup() {
  applicationSetup();
}

void loop() {
  applicationLoop();
}

#endif  // ARDUINO
