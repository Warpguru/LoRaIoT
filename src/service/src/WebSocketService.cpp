#include "WebSocketService.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../../util/src/LogBuffer.h"
#include "../../util/src/TaskUtils.h"

/**
 * WebSocketService.cpp - WebSocket broadcast task + command queue.
 *
 * broadcastTask pushes a JSON state snapshot to all connected WebSocket
 * clients every 1 second. On dual-core builds it runs on Core 1; on
 * single-core builds it runs on the only available core.
 *
 * Mirrors SerialController WebSocketService - adapt the JSON payload
 * to the LoRaIoT shared state once that struct is defined.
 */

static constexpr uint32_t BROADCAST_TASK_STACK_SIZE = 4096;  // Bytes of stack for broadcastTask
static constexpr UBaseType_t BROADCAST_TASK_PRIORITY = 1;    // Below the LoRa transport task (priority 2)
static constexpr uint32_t BROADCAST_INTERVAL_MS = 1000;      // Time between state snapshots

WebSocketService::WebSocketService(AsyncWebSocket* webSocket) : ws(webSocket) {}

void WebSocketService::begin() {
  TASK_CREATE(broadcastTask, "WSBroadcast", BROADCAST_TASK_STACK_SIZE, this, BROADCAST_TASK_PRIORITY, nullptr);
  Log_info("WebSocketService: broadcast task started.");
}

void WebSocketService::broadcastTask(void* param) {
  WebSocketService* self = static_cast<WebSocketService*>(param);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(BROADCAST_INTERVAL_MS));

    // TODO: build JSON snapshot from shared state and broadcast.
    // Example:
    //   JsonDocument doc;
    //   doc["uptime"] = millis() / 1000;
    //   // ... add LoRa state fields ...
    //   String json;
    //   serializeJson(doc, json);
    //   self->ws->textAll(json);
  }
}
