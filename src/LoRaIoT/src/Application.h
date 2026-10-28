#pragma once

#include "../../lora/src/nanopb/loraiot.pb.h"
#include "../../registry/src/PeerRegistry.h"

/**
 * Application.h - LoRaIoT application entry points.
 *
 * Both LoRaIoT.ino (Arduino IDE) and LoRaIoT.cpp (PlatformIO)
 * are thin stubs that delegate setup() and loop() to these two functions.
 */

/** Initialise all hardware, networking, and FreeRTOS tasks. Called once from setup(). */
void applicationSetup();

/**
 * Main loop body. Called repeatedly from loop().
 * Drains the WebSocket command queue and drives the DescribeCrawler state machine.
 */
void applicationLoop();

/**
 * Returns true once the boot diagnostics have completed and the LoRa radio
 * is fully initialised. Radio tasks (beaconTask, future tasks) must poll
 * this before transmitting anything. Set once by applicationLoop() after
 * the first diagnosticsLoop() run; never reset.
 */
bool loraIsReady();

/**
 * Encode a loraiot_Data payload as a DATA frame and enqueue it for
 * transmission to dstAddr. Fire-and-forget: posts to the LoRa TX queue
 * and returns immediately. Safe to call from any task or core.
 * No-op if LoRa transport is not yet initialised.
 */
void loraPushData(uint8_t dstAddr, const loraiot_Data& data);

/**
 * Registry this node OWNS (answers Describe/Read, pushes values).
 *   LOCAL: ControlRegistry   REMOTE: SensorRegistry
 */
PeerRegistry& ownedRegistry();

/**
 * Registry this node CACHES (crawled by Describe, filled by pushes).
 *   LOCAL: SensorRegistry    REMOTE: ControlRegistry
 */
PeerRegistry& cachedRegistry();
