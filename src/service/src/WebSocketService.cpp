#include "WebSocketService.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../../diagnostics/src/TaskRegistry.h"
#include "../../util/src/LogBuffer.h"
#include "../../util/src/TaskUtils.h"
#include "../../LoRaIoT/src/Tier.h"
#include "../../lora/src/LoRaTransport.h"
#include "RestService.h"

/**
 * WebSocketService.cpp - WebSocket broadcast task + command queue.
 *
 * broadcastTask pushes a JSON state snapshot to all connected WebSocket
 * clients every 1 second. On dual-core builds it runs on Core 1; on
 * single-core builds it runs on the only available core.
 *
 * JSON schema (all fields always present):
 * {
 *   "uptime_s":    <seconds since boot>,
 *   "tier":        "LOCAL" | "REMOTE" | "UNKNOWN",
 *   "lora": {
 *     "tx_frames":  <uint32>,
 *     "rx_frames":  <uint32>,
 *     "crc_errors": <uint32>,
 *     "ack_loss":   <uint32>
 *   },
 *   "echo": {                          // only when valid=true
 *     "sent":          "<text>",
 *     "received":      "<text>",
 *     "seq_no":        <uint32>,
 *     "round_trip_ms": <uint32>,
 *     "success":       <bool>
 *   } | null
 * }
 */

static constexpr uint32_t BROADCAST_TASK_STACK_SIZE = 4096;  // Bytes of stack for broadcastTask
static constexpr UBaseType_t BROADCAST_TASK_PRIORITY = 1;    // Below the LoRa transport task (priority 2)
static constexpr uint32_t BROADCAST_INTERVAL_MS = 1000;      // Time between state snapshots

WebSocketService::WebSocketService(AsyncWebSocket* webSocket) : ws(webSocket) {}

void WebSocketService::begin() {
  TaskHandle_t handle = nullptr;
  TASK_CREATE(broadcastTask, "WSBroadcast", BROADCAST_TASK_STACK_SIZE, this, BROADCAST_TASK_PRIORITY, &handle);
  TaskRegistry.add("WSBroadcast", handle, BROADCAST_TASK_STACK_SIZE);
  Log_info("WebSocketService: broadcast task started.");
}

void WebSocketService::broadcastTask(void* param) {
  WebSocketService* self = static_cast<WebSocketService*>(param);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(BROADCAST_INTERVAL_MS));

    // Skip if no clients are connected (avoids serialisation cost).
    if (self->ws->count() == 0) continue;

    const LoRaStats stats = LoRaTransport.getStats();
    const EchoResult& echo = restServiceGetLastEcho();

    JsonDocument doc;
    doc["uptime_s"] = millis() / 1000UL;
    doc["tier"]     = Gateway.getTierName();

    JsonObject lora = doc["lora"].to<JsonObject>();
    lora["tx_frames"]  = stats.txFrames;
    lora["rx_frames"]  = stats.rxFrames;
    lora["crc_errors"] = stats.crcErrors;
    lora["ack_loss"]   = stats.ackLoss;

    if (echo.valid) {
      JsonObject echoObj = doc["echo"].to<JsonObject>();
      echoObj["sent"]          = echo.sent;
      echoObj["received"]      = echo.received;
      echoObj["seq_no"]        = echo.seqNo;
      echoObj["round_trip_ms"] = echo.roundTripMs;
      echoObj["success"]       = echo.success;
    } else {
      doc["echo"] = nullptr;
    }

    String json;
    serializeJson(doc, json);
    self->ws->textAll(json);
  }
}
