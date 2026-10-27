#include "PushTask.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "../../diagnostics/src/TaskRegistry.h"
#include "../../lora/src/nanopb/loraiot.pb.h"
#include "../../registry/src/DeviceTable.h"
#include "../../registry/src/PeerRegistry.h"
#include "../../util/src/LogBuffer.h"
#include "../../util/src/TaskUtils.h"
#include "Application.h"
#include "LoRaBroker.h"

/**
 * PushTask.cpp - Autonomous proactive push task implementation.
 *
 * Detailed execution flow:
 * ------------------------
 * 1. Initialization (pushTaskBegin):
 *    - Creates pushQueue of capacity PUSH_QUEUE_LENGTH.
 *    - Spawns the FreeRTOS task with TASK_CREATE macro and registers it with TaskRegistry.
 *
 * 2. Gate check (pushTask):
 *    - Polls loraIsReady() until boot diagnostics and radio initialisation complete.
 *
 * 3. Event processing loop:
 *    - Blocks indefinitely on pushQueue waiting for a PushItem (portMAX_DELAY).
 *    - When an item arrives (specifying peerAddr and reason):
 *      a. Queries ownedRegistry().getDeviceCount(peerAddr) to log the push scope.
 *      b. Enumerates all device slots (1 .. DEVICE_TABLE_MAX_DEVICES).
 *      c. For each slot, attempts to build a protobuf Data message via
 *         ownedRegistry().toProtoData(peerAddr, deviceIdx, protoData).
 *         If false (slot is unused), skips to the next device.
 *      d. Sends the Data payload reliably via loraSendDataReliable().
 *         - If result is BrokerResult::BUSY:
 *           Waits PUSH_BUSY_RETRY_MS and retries up to PUSH_MAX_BUSY_RETRIES times.
 *           This gives transient PingTask / REST transactions time to complete.
 *         - If result is BrokerResult::TIMEOUT:
 *           Logs warning and moves on to the next device slot.
 *         - If result is BrokerResult::OK:
 *           Checks ackStatus. If STATUS_BAD_REQUEST is returned, the peer's schema
 *           is not yet aligned (e.g. peer needs a Describe crawl). Otherwise logs OK.
 */

// ---- State ------------------------------------------------------------------

// FreeRTOS queue holding pending push notifications.
static QueueHandle_t pushQueue = nullptr;

// ---- Public notification interface -----------------------------------------

/**
 * Enqueue a push notification after a peer finishes its Describe crawl.
 *
 * Called on the RESPONDER node in sendDescribeResponse() when deviceIdx is unknown,
 * meaning the requester has crawled all available device schemas.
 */
void pushTaskNotifyCrawlComplete(uint8_t peerAddr) {
  if (pushQueue == nullptr) {
    return;
  }
  PushItem item = {peerAddr, PUSH_REASON_CRAWL_COMPLETE};
  if (xQueueSend(pushQueue, &item, 0) != pdTRUE) {
    Log_warn("PushTask: queue full, dropped crawl-complete push for 0x%02X", peerAddr);
  }
}

/**
 * Enqueue a push notification when an offline peer is detected alive again.
 *
 * Called on LOCAL in onLoRaReceive() when cachedRegistry().markSeen() transitions
 * a peer from offline to online.
 */
void pushTaskNotifyNodeBack(uint8_t peerAddr) {
  if (pushQueue == nullptr) {
    return;
  }
  PushItem item = {peerAddr, PUSH_REASON_NODE_BACK};
  if (xQueueSend(pushQueue, &item, 0) != pdTRUE) {
    Log_warn("PushTask: queue full, dropped node-back push for 0x%02X", peerAddr);
  }
}

// ---- FreeRTOS task ----------------------------------------------------------

/**
 * Internal FreeRTOS worker task.
 *
 * Runs at priority 1 (below radio transport task) and handles outbound DATA_ACK
 * pushes sequentially through the LoRaBroker.
 */
static void pushTask(void* /*param*/) {
  // Wait until boot diagnostics finish and radio hardware is ready
  while (!loraIsReady()) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  Log_info("PushTask: LoRa gate open - starting push loop");

  for (;;) {
    PushItem item = {0, 0};
    // Block until a notification arrives from sendDescribeResponse or markSeen
    if (xQueueReceive(pushQueue, &item, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    const uint8_t devCount = ownedRegistry().getDeviceCount(item.peerAddr);
    Log_info("PushTask: pushing %u devices to 0x%02X (reason=%u)", devCount, item.peerAddr, item.reason);

    // Enumerate all possible device slots for this peer
    for (uint8_t deviceIdx = 1; deviceIdx <= DEVICE_TABLE_MAX_DEVICES; deviceIdx++) {
      loraiot_Data protoData = loraiot_Data_init_zero;
      // Extract latest metric values for this device into a protobuf Data structure
      if (!ownedRegistry().toProtoData(item.peerAddr, deviceIdx, protoData)) {
        continue;
      }

      // Transmit with broker busy retry logic
      for (uint8_t attempt = 0; attempt < PUSH_MAX_BUSY_RETRIES; attempt++) {
        loraiot_Status ackStatus = loraiot_Status_STATUS_OK;
        const BrokerResult result = loraSendDataReliable(item.peerAddr, protoData, ackStatus);

        if (result == BrokerResult::BUSY) {
          // Broker currently held by another task (e.g. PingTask or REST call)
          if (attempt + 1 < PUSH_MAX_BUSY_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(PUSH_BUSY_RETRY_MS));
          } else {
            Log_warn("PushTask: broker busy, abandoned push for 0x%02X device=%u", item.peerAddr, deviceIdx);
          }
        } else if (result == BrokerResult::TIMEOUT) {
          // Frame was transmitted but no ACK received after full retransmit cycle
          Log_warn("PushTask: no ACK from 0x%02X device=%u", item.peerAddr, deviceIdx);
          break;
        } else if (result == BrokerResult::OK) {
          if (ackStatus == loraiot_Status_STATUS_BAD_REQUEST) {
            Log_info("PushTask: BAD_REQUEST for device=%u from 0x%02X, peer needs crawl", deviceIdx, item.peerAddr);
          } else {
            Log_info("PushTask: pushed device=%u to 0x%02X OK", deviceIdx, item.peerAddr);
          }
          break;
        } else {
          Log_error("PushTask: encode error for device=%u to 0x%02X", deviceIdx, item.peerAddr);
          break;
        }
      }
    }
  }
}

// ---- Public task begin ------------------------------------------------------

/**
 * Initialize and spawn the PushTask FreeRTOS worker.
 */
void pushTaskBegin() {
  pushQueue = xQueueCreate(PUSH_QUEUE_LENGTH, sizeof(PushItem));
  TaskHandle_t handle = nullptr;
  TASK_CREATE(pushTask, "Push", PUSH_TASK_STACK_SIZE, nullptr, PUSH_TASK_PRIORITY, &handle);
  TaskRegistry.add("Push", handle, PUSH_TASK_STACK_SIZE);
}
