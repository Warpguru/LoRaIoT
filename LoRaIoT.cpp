/**
 * LoRaIoT.cpp - PlatformIO entry point (stub).
 *
 * All application logic lives in src/LoRaIoT/src/Application.cpp.
 * This file is compiled only under PlatformIO; Arduino IDE defines the
 * ARDUINO macro so the #ifndef guard makes this file an empty translation
 * unit in that environment, preventing duplicate setup()/loop() symbols.
 * It delegates setup() and loop() to the Application package so that
 * both PlatformIO and Arduino IDE share the same implementation.
 */
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
