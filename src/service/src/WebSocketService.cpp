#include "WebSocketService.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../../LoRaIoT/src/LogBuffer.h"
#include "../../LoRaIoT/src/TaskUtils.h"

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

WebSocketService::WebSocketService(AsyncWebSocket* webSocket) : ws(webSocket) {}

void WebSocketService::begin() {
  TASK_CREATE(broadcastTask, "WSBroadcast", 4096, this, 1, nullptr);
  Log_info("WebSocketService: broadcast task started.");
}

void WebSocketService::broadcastTask(void* param) {
  WebSocketService* self = static_cast<WebSocketService*>(param);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));

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
