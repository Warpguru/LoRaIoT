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

// Capacity of the push notification queue (items are 3 bytes, so this is cheap).
// If full, crawl-complete and node-back items are dropped safely because the next crawl or
// beacon will trigger a sync. A dropped setpoint item is NOT resynchronised until the peer
// returns or re-crawls, so the queue is sized for a short burst of MQTT setpoints.
static constexpr UBaseType_t PUSH_QUEUE_LENGTH = 8;

// Delay before retrying when the LoRaBroker is busy with another transaction (ms).
static constexpr uint32_t PUSH_BUSY_RETRY_MS = 500;

// Maximum consecutive BUSY retries per device before abandoning that device push.
static constexpr uint8_t PUSH_MAX_BUSY_RETRIES = 3;

// Push reason codes for logging and diagnostics.
static constexpr uint8_t PUSH_REASON_CRAWL_COMPLETE = 0;
static constexpr uint8_t PUSH_REASON_NODE_BACK      = 1;
static constexpr uint8_t PUSH_REASON_SETPOINT       = 2;  // single device, requested by MQTT /set

// ---- Queue item structure ---------------------------------------------------

struct PushItem {
  uint8_t peerAddr;   // Target peer LoRa address (e.g. 0x01 or 0x02)
  uint8_t reason;     // PUSH_REASON_CRAWL_COMPLETE, PUSH_REASON_NODE_BACK or PUSH_REASON_SETPOINT
  uint8_t deviceIdx;  // Device to push; only used by PUSH_REASON_SETPOINT (all devices otherwise)
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

/**
 * Enqueue a push of ONE device's control values to a peer (MQTT setpoint, LOCAL only).
 * Non-blocking (queue send with timeout 0): safe to call from mqttTask, which must never
 * wait on the LoRa link. The transmission itself runs on the push task via
 * loraSendDataReliable().
 *
 * @param peerAddr  The LoRa address of the REMOTE the setpoint is for.
 * @param deviceIdx The device index (1..DEVICE_TABLE_MAX_DEVICES) in ControlRegistry.
 */
void pushTaskNotifySetpoint(uint8_t peerAddr, uint8_t deviceIdx);
