/**
 * LoRaIoT.ino - Arduino IDE entry point (stub).
 *
 * All application logic lives in src/LoRaIoT/src/Application.cpp.
 * This file exists solely to satisfy the Arduino IDE requirement that
 * a .ino file matching the sketch folder name must be present.
 * It delegates setup() and loop() to the Application package so that
 * both Arduino IDE and PlatformIO share the same implementation.
 */
#include "src/LoRaIoT/src/Application.h"

void setup() {
  applicationSetup();
}

void loop() {
  applicationLoop();
}
