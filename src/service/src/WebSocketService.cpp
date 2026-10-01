#include "WebSocketService.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../../LoRaIoT/src/LogBuffer.h"

/**
 * WebSocketService.cpp - WebSocket broadcast task + command queue.
 *
 * broadcastTask runs on Core 1, pushes a JSON state snapshot to all
 * connected WebSocket clients every 1 second.
 *
 * Mirrors SerialController WebSocketService - adapt the JSON payload
 * to the LoRaIoT shared state once that struct is defined.
 */

WebSocketService::WebSocketService(AsyncWebSocket* ws) : _ws(ws) {}

void WebSocketService::begin() {
  xTaskCreatePinnedToCore(broadcastTask, "WSBroadcast", 4096, this, 1, nullptr, 1);
  Log_info("WebSocketService: broadcast task started on Core 1.");
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
    //   self->_ws->textAll(json);
  }
}
