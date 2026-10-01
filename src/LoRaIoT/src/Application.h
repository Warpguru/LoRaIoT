#pragma once

/**
 * Application.h - LoRaIoT application entry points.
 *
 * Both LoRaIoT.ino (Arduino IDE) and LoRaIoT.cpp (PlatformIO)
 * are thin stubs that delegate setup() and loop() to these two functions.
 */

void applicationSetup();
void applicationLoop();
