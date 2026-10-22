#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

/**
 * BeaconTask.h - REMOTE beacon state machine (FreeRTOS task).
 *
 * The beacon task broadcasts a BEACON frame on a periodic interval that depends
 * on whether LOCAL has been seen recently. The two-state machine is:
 *
 *   UNDISCOVERED  beacon every 5 s + 0-1 s jitter
 *       |  first frame from LOCAL addressed to this REMOTE
 *       v
 *   DISCOVERED    beacon every 60 s
 *       |  no frame from LOCAL for 2 min
 *       v
 *   UNDISCOVERED  (self-healing)
 *
 * Call beaconTaskNotifyDiscovered() from the loraTransportTask receive callback
 * each time a frame from LOCAL arrives. The beacon task reads the timestamp and
 * selects the appropriate interval on its next wakeup.
 */

// ---- Timing and sizing constants --------------------------------------------

// Beacon interval while in UNDISCOVERED state (fast rate, ms).
static constexpr uint32_t BEACON_INTERVAL_UNDISCOVERED_MS = 5000;

// Beacon interval while in DISCOVERED state (slow rate, ms).
static constexpr uint32_t BEACON_INTERVAL_DISCOVERED_MS = 60000;

// Maximum random jitter added to each beacon interval (ms).
// Actual added jitter is in the range [0, BEACON_JITTER_MAX_MS] inclusive.
static constexpr uint32_t BEACON_JITTER_MAX_MS = 1000;

// Silence from LOCAL that causes a DISCOVERED -> UNDISCOVERED revert (ms).
static constexpr uint32_t BEACON_REVERT_TIMEOUT_MS = 120000;

// FreeRTOS task stack size for the beacon task (bytes).
static constexpr uint32_t BEACON_TASK_STACK_SIZE = 4096;

// FreeRTOS task priority for the beacon task.
// Below loraTransportTask (priority 2) to avoid starving the radio.
static constexpr UBaseType_t BEACON_TASK_PRIORITY = 1;

// ---- Public interface -------------------------------------------------------

/**
 * Start the beacon FreeRTOS task. Call once from applicationSetup() on REMOTE
 * only, after loraBrokerBegin() and LoRaTransport.begin().
 */
void beaconTaskBegin();

/**
 * Notify the beacon task that a frame from LOCAL has been received.
 * Resets the "last LOCAL contact" timestamp used for DISCOVERED state tracking.
 * Safe to call from loraTransportTask (the receive callback context).
 */
void beaconTaskNotifyDiscovered();
