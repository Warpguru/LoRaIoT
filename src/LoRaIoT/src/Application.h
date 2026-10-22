#pragma once

/**
 * Application.h - LoRaIoT application entry points.
 *
 * Both LoRaIoT.ino (Arduino IDE) and LoRaIoT.cpp (PlatformIO)
 * are thin stubs that delegate setup() and loop() to these two functions.
 */

void applicationSetup();
void applicationLoop();

/**
 * Returns true once the boot diagnostics have completed and the LoRa radio
 * is fully initialised. Radio tasks (beaconTask, future tasks) must poll
 * this before transmitting anything. Set once by applicationLoop() after
 * the first diagnosticsLoop() run; never reset.
 */
bool loraIsReady();
