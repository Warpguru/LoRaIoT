#include "WebSocketService.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../../diagnostics/src/TaskRegistry.h"
#include "../../util/src/LogBuffer.h"
#include "../../util/src/TaskUtils.h"
#include "../../LoRaIoT/src/Tier.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../registry/src/PeerRegistry.h"
#include "../../registry/src/DeviceTable.h"
#include "RestService.h"

/**
 * WebSocketService.cpp - WebSocket broadcast task (server push only, no inbound commands).
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
 *   "nodes": [                           // same shape as GET /api/lora/nodes/sensor (LOCAL)
 *     {                                  // or GET /api/lora/nodes/control (REMOTE); [] if no peers
 *       "addr":                   <uint8>,
 *       "online":                 <bool>,
 *       "peer_last_seen_ms_ago":  <uint32>,
 *       "devices": [
 *         {
 *           "device":   <uint8>,
 *           "name":     "<text>",
 *           "has_data": <bool>,
 *           "age_ms":   <uint32>,
 *           "alert":    <bool>,
 *           "metrics": [
 *             { "id": <uint8>, "name": "<text>", "unit": "<text>", "scale": <num>, "value": <num> | null }
 *           ]
 *         }
 *       ]
 *     }
 *   ],
 *   "echo": {                          // only when valid=true
 *     "sent":          "<text>",
 *     "received":      "<text>",
 *     "seq_no":        <uint32>,
 *     "round_trip_ms": <uint32>,
 *     "success":       <bool>
 *   } | null
 * }
 */

// Bytes of stack for broadcastTask. The DeviceSlot snapshot alone is
// LORA_MAX_PEERS * DEVICE_TABLE_MAX_DEVICES * sizeof(DeviceSlot) (about 4 KB) on top of
// the ~4 KB the task needed before nodes[] was added.
static constexpr uint32_t BROADCAST_TASK_STACK_SIZE = 10240;
static constexpr UBaseType_t BROADCAST_TASK_PRIORITY = 1;    // Below the LoRa transport task (priority 2)
static constexpr uint32_t BROADCAST_INTERVAL_MS = 1000;      // Time between state snapshots

// Writes one DeviceSlot as a JSON object into dobj (same shape as RestService.cpp writeDeviceSlotJson).
//
// IMPORTANT - string lifetime: ArduinoJson stores const char* by pointer, not by copy.
// `slot` must outlive the JsonDocument until serializeJson() is called.
static void appendDeviceJson(JsonObject dobj, const DeviceSlot& slot) {
  dobj["device"] = slot.index;
  dobj["name"]   = slot.name;   // pointer into slot.name[] - slot must stay alive
  bool hasAnyValue = false;
  uint32_t now = millis();
  uint32_t oldestMs = now;
  bool found = false;
  for (uint8_t m = 0; m < slot.metricCount; m++) {
    if (slot.metrics[m].hasValue) {
      hasAnyValue = true;
      if (!found || slot.metrics[m].valueMs < oldestMs) {
        oldestMs = slot.metrics[m].valueMs;
        found = true;
      }
    }
  }
  dobj["has_data"] = hasAnyValue;
  dobj["age_ms"]   = found ? (now - oldestMs) : 0;
  dobj["alert"]    = false;  // alert is a DATA-level flag not stored per-metric yet
  JsonArray mArr = dobj["metrics"].to<JsonArray>();
  for (uint8_t m = 0; m < slot.metricCount; m++) {
    JsonObject mo = mArr.add<JsonObject>();
    mo["id"]    = slot.metrics[m].id;
    mo["name"]  = slot.metrics[m].name;   // pointer - slot must stay alive
    mo["unit"]  = slot.metrics[m].unit;   // pointer - slot must stay alive
    mo["scale"] = slot.metrics[m].scale;
    if (slot.metrics[m].hasValue) {
      if (slot.metrics[m].isText) {
        mo["value"] = slot.metrics[m].text;
      } else {
        mo["value"] = slot.metrics[m].value;
      }
    } else {
      mo["value"] = nullptr;
    }
  }
}

// Appends one node object (addr, online, peer_last_seen_ms_ago, devices[]) to nodesArr.
// Returns without adding anything if the registry no longer knows the peer.
static void appendNodeJson(JsonArray nodesArr, const PeerRegistry& registry, uint8_t addr,
                           const DeviceSlot* slots, uint8_t slotCount) {
  uint32_t bootId, regRev, lastSeenMs;
  bool online, crawlPending;
  if (!registry.getNodeInfo(addr, bootId, regRev, lastSeenMs, online, crawlPending)) {
    return;
  }
  JsonObject nobj = nodesArr.add<JsonObject>();
  nobj["addr"]                  = addr;
  nobj["online"]                = online;
  nobj["peer_last_seen_ms_ago"] = millis() - lastSeenMs;
  JsonArray devArr = nobj["devices"].to<JsonArray>();
  for (uint8_t s = 0; s < slotCount; s++) {
    appendDeviceJson(devArr.add<JsonObject>(), slots[s]);
  }
}

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
    if (self->ws->count() == 0) {
      continue;
    }

    const LoRaStats stats = LoRaTransport.getStats();
    const EchoResult& echo = restServiceGetLastEcho();

    // Pre-load all device slots before building the document. ArduinoJson stores
    // const char* by pointer, so these arrays must stay alive until serializeJson().
    // LOCAL shows the SensorRegistry cache; REMOTE shows the ControlRegistry cache.
    const PeerRegistry* registry = nullptr;
    if (Gateway.isLocal()) {
      registry = &SensorRegistry;
    } else if (Gateway.isRemote()) {
      registry = &ControlRegistry;
    }

    DeviceSlot slots[LORA_MAX_PEERS][DEVICE_TABLE_MAX_DEVICES];
    uint8_t    slotCount[LORA_MAX_PEERS] = {};
    uint8_t    addrBuf[LORA_MAX_PEERS];
    uint8_t    peerCount = 0;
    if (registry != nullptr) {
      peerCount = registry->listPeers(addrBuf, LORA_MAX_PEERS);
      for (uint8_t n = 0; n < peerCount; n++) {
        for (uint8_t di = 1; di <= DEVICE_TABLE_MAX_DEVICES; di++) {
          if (registry->getDeviceSlot(addrBuf[n], di, slots[n][slotCount[n]])) {
            slotCount[n]++;
          }
        }
      }
    }

    JsonDocument doc;
    doc["uptime_s"] = millis() / 1000UL;
    doc["tier"]     = Gateway.getTierName();

    JsonObject lora = doc["lora"].to<JsonObject>();
    lora["tx_frames"]  = stats.txFrames;
    lora["rx_frames"]  = stats.rxFrames;
    lora["crc_errors"] = stats.crcErrors;
    lora["ack_loss"]   = stats.ackLoss;

    JsonArray nodesArr = doc["nodes"].to<JsonArray>();
    for (uint8_t n = 0; n < peerCount; n++) {
      appendNodeJson(nodesArr, *registry, addrBuf[n], slots[n], slotCount[n]);
    }

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
