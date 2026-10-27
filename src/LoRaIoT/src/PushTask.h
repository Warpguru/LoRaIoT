#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

/**
 * PushTask.h - Autonomous proactive push task (FreeRTOS).
 *
 * Architecture and rationale (push-control-plan.md Step 12):
 * -----------------------------------------------------------
 * When a node completes a Describe crawl of its peer's registry definitions, it has
 * the metric schemas (names, units, scale factors) but not the current values.
 * Furthermore, when an offline node comes back online or reboots, its state must be
 * brought up to date without waiting for manual REST client action.
 *
 * PushTask solves this by autonomously pushing owned registry data (control values
 * from LOCAL, sensor values from REMOTE) to peer nodes reliably using DATA_ACK frames.
 *
 * Trigger events:
 * ---------------
 * 1. Crawl completion (PUSH_REASON_CRAWL_COMPLETE):
 *    Triggered on the RESPONDER side of Describe when sendDescribeResponse() in
 *    Application.cpp encounters a request for an unknown device index and sends
 *    BAD_REQUEST. This indicates the requester has finished crawling all device
 *    definitions. The responder (owner of the data) immediately pushes its current
 *    metric values:
 *      - LOCAL answering REMOTE's crawl -> pushes all control values to REMOTE (R1)
 *      - REMOTE answering LOCAL's crawl -> pushes all sensor values to LOCAL (fixes
 *        lost first value on boot/registration)
 *
 * 2. Node back online (PUSH_REASON_NODE_BACK, LOCAL only):
 *    Triggered in onLoRaReceive() when cachedRegistry().markSeen(src) returns true.
 *    LOCAL pushes all current control values to the returning REMOTE to ensure
 *    the remote actuator state matches LOCAL without user intervention.
 *
 * Concurrency and threading:
 * --------------------------
 * - Notifications are posted to a FreeRTOS queue with timeout 0 (non-blocking).
 *   This ensures loraTransportTask (which runs the receive callback) is never blocked.
 * - The task loop runs at priority 1 (below loraTransportTask priority 2) on Core 1
 *   (or sole core on single-core builds).
 * - All radio transmissions go through loraSendDataReliable(), which acquires the
 *   LoRaBroker slot and handles DATA_ACK frame delivery with acknowledgements.
 * - If the broker is BUSY (e.g. PingTask or REST in flight), PushTask pauses for
 *   PUSH_BUSY_RETRY_MS and retries up to PUSH_MAX_BUSY_RETRIES times per device.
 */

// ---- Timing and sizing constants --------------------------------------------

// FreeRTOS task stack size for the push task (bytes).
// Sized to accommodate nanopb serialization, LoRaBroker calls, and FreeRTOS semaphores.
static constexpr uint32_t PUSH_TASK_STACK_SIZE = 4096;

// FreeRTOS task priority for the push task.
// Below loraTransportTask (priority 2) to avoid starving radio packet reception.
static constexpr UBaseType_t PUSH_TASK_PRIORITY = 1;

// Capacity of the push notification queue.
// If full, items are dropped safely because next crawl or beacon will trigger sync.
static constexpr UBaseType_t PUSH_QUEUE_LENGTH = 4;

// Delay before retrying when the LoRaBroker is busy with another transaction (ms).
static constexpr uint32_t PUSH_BUSY_RETRY_MS = 500;

// Maximum consecutive BUSY retries per device before abandoning that device push.
static constexpr uint8_t PUSH_MAX_BUSY_RETRIES = 3;

// Push reason codes for logging and diagnostics.
static constexpr uint8_t PUSH_REASON_CRAWL_COMPLETE = 0;
static constexpr uint8_t PUSH_REASON_NODE_BACK      = 1;

// ---- Queue item structure ---------------------------------------------------

struct PushItem {
  uint8_t peerAddr;  // Target peer LoRa address (e.g. 0x01 or 0x02)
  uint8_t reason;    // PUSH_REASON_CRAWL_COMPLETE or PUSH_REASON_NODE_BACK
};

// ---- Public interface -------------------------------------------------------

/**
 * Start the push FreeRTOS task. Call once from applicationSetup() on both tiers.
 */
void pushTaskBegin();

/**
 * Enqueue a push request triggered by Describe crawl completion on the responder side.
 * Non-blocking: safe to call from loraTransportTask receive callback.
 *
 * @param peerAddr The LoRa address of the requesting peer whose crawl just finished.
 */
void pushTaskNotifyCrawlComplete(uint8_t peerAddr);

/**
 * Enqueue a push request triggered by an offline peer returning online.
 * Non-blocking: safe to call from loraTransportTask receive callback.
 *
 * @param peerAddr The LoRa address of the peer that was revived.
 */
void pushTaskNotifyNodeBack(uint8_t peerAddr);
