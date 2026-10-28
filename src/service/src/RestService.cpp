#include "RestService.h"

#include <ArduinoJson.h>

#include "../../util/src/LogBuffer.h"
#include "../../LoRaIoT/src/Tier.h"
#include "../../LoRaIoT/src/LoRaConfig.h"
#include "../../LoRaIoT/src/Server.h"
#include "../../diagnostics/src/Diagnostics.h"
#include "../../registry/src/PeerRegistry.h"
#include "../../LoRaIoT/src/DescribeCrawler.h"
#include "../../LoRaIoT/src/LoRaBroker.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../lora/src/nanopb/pb_encode.h"
#include "../../LoRaIoT/src/Application.h"
#include "../../LoRaIoT/src/PingTask.h"

/**
 * RestService.cpp - LoRaIoT REST API handlers.
 *
 * API documentation: doc/RestApi.md (OpenAPI specification to follow).
 * Every endpoint registered here must be documented there in the same change.
 *
 * Add endpoints here following the SerialController pattern:
 *   - Read state under mutex, serialise with ArduinoJson, send response.
 *   - Write operations: validate input, dispatch to relevant service/task.
 *
 * HTTP status code constants are defined in Server.h.
 */

static constexpr size_t ERROR_BODY_SIZE = 128;   // Buffer for a formatted JSON error response
static constexpr size_t AIR_RATE_BODY_SIZE = 32; // Buffer for {"airRate":"62.5k"} (longest label "19.2k"/"38.4k"/"62.5k")
static constexpr size_t ECHO_TEXT_MAX   = sizeof(loraiot_Echo::text) - 1;  // Max user text (175 chars, null excluded)
static constexpr size_t DATA_VALUES_MAX = sizeof(loraiot_Data::values) / sizeof(loraiot_Data::values[0]); // Max measurements in one Data frame (16)

// ---- Last echo result (set by POST /api/lora/nodes/echo, read by GET /api/lora/echo/result) ----
// EchoResult struct is declared in RestService.h; defined here as the single owner.
static EchoResult lastEchoResult = {{}, {}, 0, 0, false, false};

const EchoResult& restServiceGetLastEcho() {
  return lastEchoResult;
}

// Writes one DeviceSlot as a JSON object into dobj.
// Shared by the single-node and all-nodes snapshot branches.
//
// IMPORTANT - string lifetime: ArduinoJson stores const char* by pointer, not
// by copy. The caller must ensure that `slot` outlives the JsonDocument that
// dobj belongs to (i.e. until serializeJson() is called). Callers achieve this
// by pre-loading all needed DeviceSlot values into a local array before building
// the document - see the snapshot handler below.
static void writeDeviceSlotJson(JsonObject dobj, const DeviceSlot& slot) {
  dobj["device"] = slot.index;
  dobj["name"]   = slot.name;   // pointer into slot.name[] - slot must stay alive
  bool hasAnyValue = false;
  for (uint8_t m = 0; m < slot.metricCount; m++) {
    if (slot.metrics[m].hasValue) {
      hasAnyValue = true;
      break;
    }
  }
  dobj["has_data"] = hasAnyValue;
  // age_ms: millis() - oldest valueMs across metrics that have a value
  uint32_t now = millis();
  uint32_t oldestMs = now;
  bool found = false;
  for (uint8_t m = 0; m < slot.metricCount; m++) {
    if (slot.metrics[m].hasValue) {
      if (!found || slot.metrics[m].valueMs < oldestMs) {
        oldestMs = slot.metrics[m].valueMs;
        found = true;
      }
    }
  }
  dobj["age_ms"] = found ? (now - oldestMs) : 0;
  dobj["alert"]  = false;  // alert is a DATA-level flag not stored per-metric yet
  JsonArray mArr = dobj["metrics"].to<JsonArray>();
  for (uint8_t m = 0; m < slot.metricCount; m++) {
    JsonObject mo = mArr.add<JsonObject>();
    mo["id"]    = slot.metrics[m].id;
    mo["name"]  = slot.metrics[m].name;   // pointer - slot must stay alive
    mo["unit"]  = slot.metrics[m].unit;   // pointer - slot must stay alive
    mo["scale"] = slot.metrics[m].scale;
    if (slot.metrics[m].hasValue) {
      mo["value"] = slot.metrics[m].value;
    } else {
      mo["value"] = nullptr;
    }
  }
}

// Owner views have no liveness of their own for the consumer peer: it is tracked by the
// other registry (the cache role on this node). Writes online and peer_last_seen_ms_ago
// from peerRegistry.getLiveness(); an unknown peer reports online=false, peer_last_seen_ms_ago=0.
static void writeOwnerPeerLivenessJson(JsonObject nobj, const PeerRegistry& peerRegistry, uint8_t peerAddr) {
  uint32_t lastSeenMs = 0;
  bool online         = false;
  if (peerRegistry.getLiveness(peerAddr, lastSeenMs, online)) {
    nobj["online"]                = online;
    nobj["peer_last_seen_ms_ago"] = millis() - lastSeenMs;
  } else {
    nobj["online"]                = false;
    nobj["peer_last_seen_ms_ago"] = 0;
  }
}

RestService::RestService(AsyncWebServer* webServer) : server(webServer) {}

void RestService::registerRoutes() {
  // GET /api/diag/e220 - request a re-run of the E220 wiring and register check.
  // GET (not POST) so it can be triggered from a browser address bar or link.
  // The check itself runs in applicationLoop() (never in this callback, which must
  // not do Serial2 I/O or block). Read the outcome from GET /api/log.
  // Documented in doc/RestApi.md (section "GET /api/diag/e220").
  server->on("/api/diag/e220", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/diag/e220: E220 check requested");
    diagnosticsRequest();
    request->send(HTTP_STATUS_ACCEPTED, "application/json", "{\"status\":\"requested\",\"result\":\"see GET /api/log\"}");
  });

  // GET /api/tier - return the active gateway tier.
  // Documented in doc/RestApi.md (section "GET /api/tier").
  server->on("/api/tier", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/tier");
    JsonDocument doc;
    doc["tier"]         = Gateway.getTierName();
    doc["configured"]   = Gateway.isConfigured();
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // GET /api/lora/channel - get active LoRa channel (available on all nodes).
  server->on("/api/lora/channel", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/lora/channel");
    JsonDocument doc;
    doc["channel"] = LoRaConfig.getChannel();
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // PUT /api/lora/channel - set active LoRa channel (available on all nodes).
  server->on(
      "/api/lora/channel", HTTP_PUT,
      [](AsyncWebServerRequest* request) {},
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err || !doc["channel"].is<int>()) {
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"Expected {\\\"channel\\\":23}\"}");
          return;
        }

        int chan = doc["channel"].as<int>();
        if (chan < LORA_MIN_CHANNEL || chan > LORA_MAX_CHANNEL) {
          char errMsg[ERROR_BODY_SIZE];
          snprintf(errMsg, sizeof(errMsg), "{\"error\":\"Channel must be between %d and %d\"}", LORA_MIN_CHANNEL, LORA_MAX_CHANNEL);
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", errMsg);
          return;
        }

        LoRaConfig.setChannel((uint8_t)chan);
        LoRaConfig.save();

        Log_info("LoRa channel changed via API to %d", LoRaConfig.getChannel());

        String json = "{\"channel\":";
        json += String(LoRaConfig.getChannel());
        json += "}";
        request->send(HTTP_STATUS_OK, "application/json", json);
      });

  // GET /api/lora/airrate - get active LoRa air data rate (available on all nodes).
  // Plain snprintf into a stack buffer: no JsonDocument or String heap use for a one-field reply.
  server->on("/api/lora/airrate", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/lora/airrate");
    char body[AIR_RATE_BODY_SIZE];
    snprintf(body, sizeof(body), "{\"airRate\":\"%s\"}", LoRaTransportClass::airRateName(LoRaConfig.getAirRate()));
    request->send(HTTP_STATUS_OK, "application/json", body);
  });

  // PUT /api/lora/airrate - set the LoRa air data rate (available on all nodes).
  // Persists the rate in NVS and asks loraTransportTask (sole owner of UART2) to rewrite the
  // E220 registers right away, so the new rate is active without a reboot.
  server->on(
      "/api/lora/airrate", HTTP_PUT,
      [](AsyncWebServerRequest* request) {},
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        LoRaTransportClass::AirRate rate;
        if (err || !doc["airRate"].is<const char*>() ||
            !LoRaTransportClass::airRateFromName(doc["airRate"].as<const char*>(), rate)) {
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json",
                        "{\"error\":\"Expected {\\\"airRate\\\":\\\"2.4k|4.8k|9.6k|19.2k|38.4k|62.5k\\\"}\"}");
          return;
        }

        LoRaConfig.setAirRate(rate);
        LoRaConfig.save();
        LoRaTransport.requestReconfigure();

        Log_info("LoRa air rate changed via API to %s", LoRaTransportClass::airRateName(LoRaConfig.getAirRate()));

        char body[AIR_RATE_BODY_SIZE];
        snprintf(body, sizeof(body), "{\"airRate\":\"%s\"}", LoRaTransportClass::airRateName(LoRaConfig.getAirRate()));
        request->send(HTTP_STATUS_OK, "application/json", body);
      });

  // GET /api/lora/address - get active LoRa address (available on all nodes).
  server->on("/api/lora/address", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/lora/address");
    JsonDocument doc;
    doc["address"] = LoRaConfig.getAddress();
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // PUT /api/lora/address - set active LoRa address (only on REMOTE).
  server->on(
      "/api/lora/address", HTTP_PUT,
      [](AsyncWebServerRequest* request) {
        if (!Gateway.isRemote()) {
          Log_warn("PUT /api/lora/address failed - not a REMOTE instance");
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"endpoint not supported\"}");
        }
      },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (!Gateway.isRemote()) {
          return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err || !doc["address"].is<int>()) {
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"Expected {\\\"address\\\":1}\"}");
          return;
        }

        int addr = doc["address"].as<int>();
        if (addr < LORA_DEFAULT_ADDR_REMOTE || addr > LORA_MAX_ADDR_REMOTE) {
          char errMsg[ERROR_BODY_SIZE];
          snprintf(errMsg, sizeof(errMsg), "{\"error\":\"Address must be between %d and %d\"}", LORA_DEFAULT_ADDR_REMOTE, LORA_MAX_ADDR_REMOTE);
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", errMsg);
          return;
        }

        LoRaConfig.setAddress((uint8_t)addr);
        LoRaConfig.save();

        Log_info("LoRa address changed via API to %d", LoRaConfig.getAddress());

        String json = "{\"address\":";
        json += String(LoRaConfig.getAddress());
        json += "}";
        request->send(HTTP_STATUS_OK, "application/json", json);
      });

  // Note: loraKey has no REST endpoint - it is set only via the captive portal during
  // provisioning and takes effect on next reboot. The boot log shows whether encryption
  // is enabled. Use GET /reset + re-provision to change the key.

  // GET /api/system/cpu - get active CPU frequency (available on all nodes).
  server->on("/api/system/cpu", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/system/cpu");
    JsonDocument doc;
    doc["frequency"] = getCpuFrequencyMhz();
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // PUT /api/system/cpu - set active CPU frequency (available on all nodes).
  server->on(
      "/api/system/cpu", HTTP_PUT,
      [](AsyncWebServerRequest* request) {},
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err || !doc["frequency"].is<int>()) {
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"Expected {\\\"frequency\\\":240}\"}");
          return;
        }

        int freq = doc["frequency"].as<int>();
        if (freq != LORA_CPU_FREQ_80MHZ &&
            freq != LORA_CPU_FREQ_160MHZ &&
            freq != LORA_CPU_FREQ_240MHZ) {
          char errMsg[ERROR_BODY_SIZE];
          snprintf(errMsg, sizeof(errMsg), "{\"error\":\"Frequency must be %d, %d, or %d MHz\"}",
                   LORA_CPU_FREQ_80MHZ, LORA_CPU_FREQ_160MHZ, LORA_CPU_FREQ_240MHZ);
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", errMsg);
          return;
        }

        // Apply dynamically and save to NVS
        setCpuFrequencyMhz((uint32_t)freq);
        LoRaConfig.setCpuFreq((uint32_t)freq);
        LoRaConfig.save();

        Log_info("CPU frequency changed via API to %d MHz", getCpuFrequencyMhz());

        String json = "{\"frequency\":";
        json += String(getCpuFrequencyMhz());
        json += "}";
        request->send(HTTP_STATUS_OK, "application/json", json);
      });

  // -------------------------------------------------------------------------
  // LOCAL-only: LoRa node registry and telemetry endpoints (read-only, 2c)
  // All require tier=LOCAL. Parameters are query strings (see doc/TechDebt.md §2).
  // -------------------------------------------------------------------------

  // GET /api/lora/nodes/sensor - sensor telemetry view (available on all nodes).
  // LOCAL: cache view of SensorRegistry (supports ?addr=, ?device=, ?live=true, ?timeout=, ?refresh=true).
  // REMOTE: owner view of SensorRegistry (?addr= and ?live=/?refresh= return 400).
  server->on("/api/lora/nodes/sensor", HTTP_GET, [](AsyncWebServerRequest* request) {
    const bool isRemote = Gateway.isRemote();

    if (isRemote) {
      if (request->hasArg("addr")) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr not supported\"}");
        return;
      }
      if (request->hasArg("live") || request->hasArg("refresh")) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"live and refresh not supported\"}");
        return;
      }

      bool hasDevice    = request->hasArg("device");
      uint8_t deviceIdx = hasDevice ? (uint8_t)request->arg("device").toInt() : 0;

      Log_info("GET /api/lora/nodes/sensor (REMOTE owner view)");

      DeviceSlot slots[DEVICE_TABLE_MAX_DEVICES];
      uint8_t slotCount = 0;
      for (uint8_t di = 1; di <= DEVICE_TABLE_MAX_DEVICES; di++) {
        if (SensorRegistry.getDeviceSlot(LORA_DEFAULT_ADDR_LOCAL, di, slots[slotCount])) {
          slotCount++;
        }
      }

      JsonDocument doc;
      JsonArray nodesArr = doc["nodes"].to<JsonArray>();
      JsonObject nobj = nodesArr.add<JsonObject>();
      nobj["addr"]         = LoRaConfig.getAddress();
      nobj["boot_id"]      = loraBrokerBootId();
      nobj["registry_rev"] = SensorRegistry.getRegistryRev(LORA_DEFAULT_ADDR_LOCAL);
      writeOwnerPeerLivenessJson(nobj, ControlRegistry, LORA_DEFAULT_ADDR_LOCAL);
      JsonArray devArr = nobj["devices"].to<JsonArray>();
      for (uint8_t s = 0; s < slotCount; s++) {
        if (hasDevice && slots[s].index != deviceIdx) {
          continue;
        }
        writeDeviceSlotJson(devArr.add<JsonObject>(), slots[s]);
      }

      String json;
      serializeJson(doc, json);
      request->send(HTTP_STATUS_OK, "application/json", json);
      return;
    }

    // LOCAL cache view
    bool hasAddr   = request->hasArg("addr");
    bool hasDevice = request->hasArg("device");
    bool liveReq   = request->hasArg("live") && request->arg("live") == "true";
    bool refreshReq = request->hasArg("refresh") && request->arg("refresh") == "true";

    // Validate parameter combinations
    if (hasDevice && !hasAddr) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"device requires addr\"}");
      return;
    }
    if (refreshReq && !hasAddr) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"refresh=true requires addr\"}");
      return;
    }
    if (liveReq && (!hasAddr || !hasDevice)) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"live=true requires both addr and device\"}");
      return;
    }

    uint8_t addr      = 0;
    uint8_t deviceIdx = 0;
    if (hasAddr) {
      addr = (uint8_t)request->arg("addr").toInt();
      if (addr < LORA_DEFAULT_ADDR_LOCAL || addr > LORA_MAX_ADDR_REMOTE) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (1-254)\"}");
        return;
      }
    }
    if (hasDevice) {
      deviceIdx = (uint8_t)request->arg("device").toInt();
    }

    // Refresh: flag a Describe crawl of the node and return at once (never blocks).
    if (refreshReq) {
      if (!SensorRegistry.requestCrawl(addr)) {
        request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"node not found\"}");
        return;
      }
      describeCrawlerSchedule(addr);
      char body[ERROR_BODY_SIZE];
      snprintf(body, sizeof(body), "{\"status\":\"crawl_scheduled\",\"addr\":%u}", (unsigned)addr);
      request->send(HTTP_STATUS_ACCEPTED, "application/json", body);
      return;
    }

    // Live read: send REQUEST(Read) and wait for a fresh DATA response
    if (liveReq) {
      uint32_t timeoutMs = BROKER_DEFAULT_TIMEOUT_MS;
      if (request->hasArg("timeout")) {
        uint32_t t = (uint32_t)request->arg("timeout").toInt();
        if (t >= BROKER_MIN_TIMEOUT_MS && t <= BROKER_MAX_TIMEOUT_MS) {
          timeoutMs = t;
        }
      }
      loraiot_Read readReq = loraiot_Read_init_zero;
      readReq.device = deviceIdx;
      uint32_t seq = 0;
      BrokerResult res = loraBrokerSend(BrokerRequestType::READ, addr, &readReq, seq);
      if (res == BrokerResult::BUSY) {
        request->send(HTTP_STATUS_SERVICE_UNAVAILABLE, "application/json", "{\"error\":\"LoRa broker busy\"}");
        return;
      }
      if (loraBrokerWait(timeoutMs) != BrokerResult::OK) {
        request->send(HTTP_STATUS_GATEWAY_TIMEOUT, "application/json", "{\"error\":\"no response from node\"}");
        return;
      }
      const loraiot_Response& resp = loraBrokerGetResponse();
      if (resp.status != loraiot_Status_STATUS_OK || resp.which_result != loraiot_Response_data_tag) {
        request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"node returned error\"}");
        return;
      }
      SensorRegistry.storeData(addr, resp.result.data);
    }

    Log_info("GET /api/lora/nodes/sensor addr=%u device=%u live=%d",
             (unsigned)addr, (unsigned)deviceIdx, (int)liveReq);

    // Pre-load all device slots before building the document.
    // ArduinoJson stores const char* by pointer (not by copy), so every DeviceSlot
    // whose string fields are referenced in the document must stay alive until
    // serializeJson() is called. Declaring the arrays here (same scope as doc)
    // guarantees that lifetime without any heap allocation.
    DeviceSlot slots[LORA_MAX_PEERS][DEVICE_TABLE_MAX_DEVICES];
    uint8_t    slotCount[LORA_MAX_PEERS] = {};  // number of valid slots per peer

    // Build the response
    JsonDocument doc;
    JsonArray nodesArr = doc["nodes"].to<JsonArray>();

    if (hasAddr) {
      // Single node - load its slots into slots[0]
      uint32_t bootId, regRev, lastSeenMs;
      bool online, crawlPending;
      if (!SensorRegistry.getNodeInfo(addr, bootId, regRev, lastSeenMs, online, crawlPending)) {
        request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"node not found\"}");
        return;
      }
      for (uint8_t di = 1; di <= DEVICE_TABLE_MAX_DEVICES; di++) {
        if (SensorRegistry.getDeviceSlot(addr, di, slots[0][slotCount[0]])) {
          slotCount[0]++;
        }
      }
      JsonObject nobj = nodesArr.add<JsonObject>();
      nobj["addr"]         = addr;
      nobj["boot_id"]      = bootId;
      nobj["registry_rev"] = regRev;
      nobj["online"]       = online;
      nobj["peer_last_seen_ms_ago"] = millis() - lastSeenMs;
      JsonArray devArr = nobj["devices"].to<JsonArray>();
      for (uint8_t s = 0; s < slotCount[0]; s++) {
        if (hasDevice && slots[0][s].index != deviceIdx) {
          continue;
        }
        writeDeviceSlotJson(devArr.add<JsonObject>(), slots[0][s]);
      }
    } else {
      // All nodes - load each peer's slots into slots[n]
      uint8_t addrBuf[LORA_MAX_PEERS];
      uint8_t count = SensorRegistry.listPeers(addrBuf, LORA_MAX_PEERS);
      for (uint8_t n = 0; n < count; n++) {
        for (uint8_t di = 1; di <= DEVICE_TABLE_MAX_DEVICES; di++) {
          if (SensorRegistry.getDeviceSlot(addrBuf[n], di, slots[n][slotCount[n]])) {
            slotCount[n]++;
          }
        }
      }
      for (uint8_t n = 0; n < count; n++) {
        uint32_t bootId, regRev, lastSeenMs;
        bool online, crawlPending;
        if (!SensorRegistry.getNodeInfo(addrBuf[n], bootId, regRev, lastSeenMs, online, crawlPending)) {
          continue;
        }
        JsonObject nobj = nodesArr.add<JsonObject>();
        nobj["addr"]         = addrBuf[n];
        nobj["boot_id"]      = bootId;
        nobj["registry_rev"] = regRev;
        nobj["online"]       = online;
        nobj["peer_last_seen_ms_ago"] = millis() - lastSeenMs;
        JsonArray devArr = nobj["devices"].to<JsonArray>();
        for (uint8_t s = 0; s < slotCount[n]; s++) {
          writeDeviceSlotJson(devArr.add<JsonObject>(), slots[n][s]);
        }
      }
    }

    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // GET /api/lora/nodes/control - control telemetry view (available on all nodes).
  // LOCAL: owner view of ControlRegistry (supports ?addr=, ?device=; ?live=/?refresh= return 400).
  // REMOTE: cache view of ControlRegistry (?addr= returns 400; supports ?device=, ?live=true
  //         (requires device), ?timeout=, ?refresh=true). The only peer is LOCAL.
  server->on("/api/lora/nodes/control", HTTP_GET, [](AsyncWebServerRequest* request) {
    const bool isRemote = Gateway.isRemote();

    if (isRemote) {
      if (request->hasArg("addr")) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr not supported\"}");
        return;
      }

      bool hasDevice    = request->hasArg("device");
      bool liveReq      = request->hasArg("live") && request->arg("live") == "true";
      bool refreshReq   = request->hasArg("refresh") && request->arg("refresh") == "true";
      uint8_t deviceIdx = hasDevice ? (uint8_t)request->arg("device").toInt() : 0;

      if (liveReq && !hasDevice) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"live=true requires device\"}");
        return;
      }

      // Refresh: flag a Describe crawl of LOCAL and return at once (never blocks).
      if (refreshReq) {
        if (!ControlRegistry.requestCrawl(LORA_DEFAULT_ADDR_LOCAL)) {
          request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"node not found\"}");
          return;
        }
        describeCrawlerSchedule(LORA_DEFAULT_ADDR_LOCAL);
        request->send(HTTP_STATUS_ACCEPTED, "application/json", "{\"status\":\"crawl_scheduled\"}");
        return;
      }

      // Live read: send REQUEST(Read) to LOCAL and wait for a fresh DATA response
      if (liveReq) {
        uint32_t timeoutMs = BROKER_DEFAULT_TIMEOUT_MS;
        if (request->hasArg("timeout")) {
          uint32_t t = (uint32_t)request->arg("timeout").toInt();
          if (t >= BROKER_MIN_TIMEOUT_MS && t <= BROKER_MAX_TIMEOUT_MS) {
            timeoutMs = t;
          }
        }
        loraiot_Read readReq = loraiot_Read_init_zero;
        readReq.device = deviceIdx;
        uint32_t seq = 0;
        BrokerResult res = loraBrokerSend(BrokerRequestType::READ, LORA_DEFAULT_ADDR_LOCAL, &readReq, seq);
        if (res == BrokerResult::BUSY) {
          request->send(HTTP_STATUS_SERVICE_UNAVAILABLE, "application/json", "{\"error\":\"LoRa broker busy\"}");
          return;
        }
        if (loraBrokerWait(timeoutMs) != BrokerResult::OK) {
          request->send(HTTP_STATUS_GATEWAY_TIMEOUT, "application/json", "{\"error\":\"no response from node\"}");
          return;
        }
        const loraiot_Response& resp = loraBrokerGetResponse();
        if (resp.status != loraiot_Status_STATUS_OK || resp.which_result != loraiot_Response_data_tag) {
          request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"node returned error\"}");
          return;
        }
        ControlRegistry.storeData(LORA_DEFAULT_ADDR_LOCAL, resp.result.data);
      }

      Log_info("GET /api/lora/nodes/control (REMOTE cache view) device=%u live=%d",
               (unsigned)deviceIdx, (int)liveReq);

      DeviceSlot slots[LORA_MAX_PEERS][DEVICE_TABLE_MAX_DEVICES];
      uint8_t    slotCount[LORA_MAX_PEERS] = {};

      uint8_t addrBuf[LORA_MAX_PEERS];
      uint8_t count = ControlRegistry.listPeers(addrBuf, LORA_MAX_PEERS);
      for (uint8_t n = 0; n < count; n++) {
        for (uint8_t di = 1; di <= DEVICE_TABLE_MAX_DEVICES; di++) {
          if (ControlRegistry.getDeviceSlot(addrBuf[n], di, slots[n][slotCount[n]])) {
            slotCount[n]++;
          }
        }
      }

      JsonDocument doc;
      JsonArray nodesArr = doc["nodes"].to<JsonArray>();
      for (uint8_t n = 0; n < count; n++) {
        uint32_t bootId, regRev, lastSeenMs;
        bool online, crawlPending;
        if (!ControlRegistry.getNodeInfo(addrBuf[n], bootId, regRev, lastSeenMs, online, crawlPending)) {
          continue;
        }
        JsonObject nobj = nodesArr.add<JsonObject>();
        nobj["addr"]         = addrBuf[n];
        nobj["boot_id"]      = bootId;
        nobj["registry_rev"] = regRev;
        nobj["online"]       = online;
        nobj["peer_last_seen_ms_ago"] = millis() - lastSeenMs;
        JsonArray devArr = nobj["devices"].to<JsonArray>();
        for (uint8_t s = 0; s < slotCount[n]; s++) {
          if (hasDevice && slots[n][s].index != deviceIdx) {
            continue;
          }
          writeDeviceSlotJson(devArr.add<JsonObject>(), slots[n][s]);
        }
      }

      String json;
      serializeJson(doc, json);
      request->send(HTTP_STATUS_OK, "application/json", json);
      return;
    }

    // LOCAL owner view
    if (request->hasArg("live") || request->hasArg("refresh")) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"live and refresh not supported\"}");
      return;
    }

    bool hasAddr   = request->hasArg("addr");
    bool hasDevice = request->hasArg("device");

    if (hasDevice && !hasAddr) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"device requires addr\"}");
      return;
    }

    uint8_t addr      = 0;
    uint8_t deviceIdx = 0;
    if (hasAddr) {
      addr = (uint8_t)request->arg("addr").toInt();
      if (addr < LORA_DEFAULT_ADDR_LOCAL || addr > LORA_MAX_ADDR_REMOTE) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (1-254)\"}");
        return;
      }
    }
    if (hasDevice) {
      deviceIdx = (uint8_t)request->arg("device").toInt();
    }

    Log_info("GET /api/lora/nodes/control addr=%u device=%u (LOCAL owner view)",
             (unsigned)addr, (unsigned)deviceIdx);

    DeviceSlot slots[LORA_MAX_PEERS][DEVICE_TABLE_MAX_DEVICES];
    uint8_t    slotCount[LORA_MAX_PEERS] = {};

    JsonDocument doc;
    JsonArray nodesArr = doc["nodes"].to<JsonArray>();

    if (hasAddr) {
      uint32_t bootId, regRev, lastSeenMs;
      bool online, crawlPending;
      if (!ControlRegistry.getNodeInfo(addr, bootId, regRev, lastSeenMs, online, crawlPending)) {
        request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"node not found\"}");
        return;
      }
      for (uint8_t di = 1; di <= DEVICE_TABLE_MAX_DEVICES; di++) {
        if (ControlRegistry.getDeviceSlot(addr, di, slots[0][slotCount[0]])) {
          slotCount[0]++;
        }
      }
      JsonObject nobj = nodesArr.add<JsonObject>();
      nobj["addr"]         = addr;
      nobj["boot_id"]      = loraBrokerBootId();
      nobj["registry_rev"] = ControlRegistry.getRegistryRev(addr);
      writeOwnerPeerLivenessJson(nobj, SensorRegistry, addr);
      JsonArray devArr = nobj["devices"].to<JsonArray>();
      for (uint8_t s = 0; s < slotCount[0]; s++) {
        if (hasDevice && slots[0][s].index != deviceIdx) {
          continue;
        }
        writeDeviceSlotJson(devArr.add<JsonObject>(), slots[0][s]);
      }
    } else {
      uint8_t addrBuf[LORA_MAX_PEERS];
      uint8_t count = ControlRegistry.listPeers(addrBuf, LORA_MAX_PEERS);
      for (uint8_t n = 0; n < count; n++) {
        for (uint8_t di = 1; di <= DEVICE_TABLE_MAX_DEVICES; di++) {
          if (ControlRegistry.getDeviceSlot(addrBuf[n], di, slots[n][slotCount[n]])) {
            slotCount[n]++;
          }
        }
      }
      for (uint8_t n = 0; n < count; n++) {
        JsonObject nobj = nodesArr.add<JsonObject>();
        nobj["addr"]         = addrBuf[n];
        nobj["boot_id"]      = loraBrokerBootId();
        nobj["registry_rev"] = ControlRegistry.getRegistryRev(addrBuf[n]);
        writeOwnerPeerLivenessJson(nobj, SensorRegistry, addrBuf[n]);
        JsonArray devArr = nobj["devices"].to<JsonArray>();
        for (uint8_t s = 0; s < slotCount[n]; s++) {
          writeDeviceSlotJson(devArr.add<JsonObject>(), slots[n][s]);
        }
      }
    }

    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // POST /api/lora/nodes/control?addr=2  (LOCAL only) - Control device registration, value storage, and push
  // Body: {"device":"Charger","metrics":{"VoltageSet":{"value":333,"unit":"V","scale":100}}}
  // Returns 200 (ok), 202 (after_describe - schema changed or peer BAD_REQUEST), 503 (busy), 504 (timeout).
  server->on("/api/lora/nodes/control", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      if (!Gateway.isLocal()) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"endpoint not supported\"}");
        return;
      }
      if (!request->hasArg("addr")) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"missing param: addr\"}");
        return;
      }
      uint8_t addr = (uint8_t)request->arg("addr").toInt();
      if (addr < LORA_DEFAULT_ADDR_REMOTE || addr > LORA_MAX_ADDR_REMOTE) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (2-254)\"}");
        return;
      }
      uint32_t bId, rRev, lsMs;
      bool onl, crw;
      if (!SensorRegistry.getNodeInfo(addr, bId, rRev, lsMs, onl, crw)) {
        request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"node not found\"}");
        return;
      }
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
      }
      if (!doc["device"].is<const char*>() || !doc["metrics"].is<JsonObject>()) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json",
                      "{\"error\":\"expected {\\\"device\\\":\\\"name\\\",\\\"metrics\\\":{\\\"MetricName\\\":{\\\"value\\\":0}}}\"}");
        return;
      }
      const char* deviceName = doc["device"].as<const char*>();
      JsonObject metrics     = doc["metrics"].as<JsonObject>();

      // Snapshot rev before registration to detect schema changes.
      uint32_t revBefore = ControlRegistry.getRegistryRev(addr);

      JsonDocument respDoc;
      JsonArray accepted = respDoc["accepted"].to<JsonArray>();
      JsonArray rejected = respDoc["rejected"].to<JsonArray>();

      // Collect unique device indices touched by accepted metrics (at most one per POST
      // since the body has a single "device" key, but guard defensively).
      uint32_t touchedDevIdx[DEVICE_TABLE_MAX_DEVICES];
      uint8_t touchedCount = 0;

      for (JsonPair kv : metrics) {
        const char* metricName = kv.key().c_str();
        int32_t value = kv.value()["value"].as<int32_t>();
        const char* unit  = kv.value()["unit"].is<const char*>()  ? kv.value()["unit"].as<const char*>()  : "";
        uint32_t scale    = kv.value()["scale"].is<uint32_t>()    ? kv.value()["scale"].as<uint32_t>()    : 1;
        if (scale == 0) {
          scale = 1;
        }
        uint32_t devIdx = 0, metricId = 0;
        if (ControlRegistry.registerMetric(addr, deviceName, metricName, unit, scale, devIdx, metricId)) {
          ControlRegistry.storeValue(addr, devIdx, metricId, value);
          JsonObject a = accepted.add<JsonObject>();
          a["metric"]    = metricName;
          a["device_idx"]= devIdx;
          a["metric_id"] = metricId;
          // Add devIdx to touchedDevIdx if not already present.
          bool found = false;
          for (uint8_t t = 0; t < touchedCount; t++) {
            if (touchedDevIdx[t] == devIdx) {
              found = true;
              break;
            }
          }
          if (!found && touchedCount < DEVICE_TABLE_MAX_DEVICES) {
            touchedDevIdx[touchedCount++] = devIdx;
          }
        } else {
          rejected.add(metricName);
        }
      }

      uint32_t revAfter = ControlRegistry.getRegistryRev(addr);
      respDoc["registry_rev"] = revAfter;
      bool schemaChanged = (revAfter != revBefore);

      if (accepted.size() == 0) {
        // Nothing accepted - return 400 with no delivery attempt.
        String json;
        serializeJson(respDoc, json);
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", json);
        return;
      }

      // Schema changed: send PING so REMOTE crawls the new revision.
      pingTaskNotifyControlChange(addr);

      // Push the touched device(s) to the remote node, one at a time (fail-fast).
      bool gotBusy        = false;
      bool gotTimeout     = false;
      bool gotBadRequest  = false;

      for (uint8_t t = 0; t < touchedCount; t++) {
        loraiot_Data protoData = loraiot_Data_init_default;
        if (!ControlRegistry.toProtoData(addr, touchedDevIdx[t], protoData)) {
          Log_warn("control POST: toProtoData failed for devIdx=%u addr=%u", touchedDevIdx[t], addr);
          continue;
        }
        loraiot_Status ackStatus = loraiot_Status_STATUS_OK;
        BrokerResult result = loraSendDataReliable(addr, protoData, ackStatus);
        if (result == BrokerResult::BUSY) {
          gotBusy = true;
          break;
        }
        if (result == BrokerResult::TIMEOUT) {
          gotTimeout = true;
          break;
        }
        // result == OK
        if (ackStatus == loraiot_Status_STATUS_BAD_REQUEST) {
          gotBadRequest = true;
        }
      }

      // Determine HTTP status and delivery field.
      int httpStatus;
      const char* delivery;
      if (gotBusy) {
        httpStatus = HTTP_STATUS_SERVICE_UNAVAILABLE;
        delivery   = "busy";
      } else if (gotTimeout) {
        httpStatus = HTTP_STATUS_GATEWAY_TIMEOUT;
        delivery   = "timeout";
      } else if (gotBadRequest || schemaChanged) {
        httpStatus = HTTP_STATUS_ACCEPTED;
        delivery   = "after_describe";
      } else {
        httpStatus = HTTP_STATUS_OK;
        delivery   = "ok";
      }

      respDoc["delivery"] = delivery;
      String json;
      serializeJson(respDoc, json);
      request->send(httpStatus, "application/json", json);
    });

  // GET /api/lora/stats - transport-layer packet counters.
  server->on("/api/lora/stats", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/lora/stats");
    LoRaStats s = LoRaTransport.getStats();
    JsonDocument doc;
    doc["tx_frames"]  = s.txFrames;
    doc["rx_frames"]  = s.rxFrames;
    doc["crc_errors"] = s.crcErrors;
    doc["ack_loss"]   = s.ackLoss;
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // -------------------------------------------------------------------------
  // Blocking endpoints - require LoRaBroker (LOCAL only)
  // -------------------------------------------------------------------------

  // POST /api/lora/nodes/echo?addr=2  body: {"text":"hello"}
  server->on("/api/lora/nodes/echo", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      if (!request->hasArg("addr")) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"missing param: addr\"}");
        return;
      }
      uint8_t addr = (uint8_t)request->arg("addr").toInt();
      if (addr < LORA_DEFAULT_ADDR_LOCAL || addr > LORA_MAX_ADDR_REMOTE) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (1-254)\"}");
        return;
      }
      JsonDocument bodyDoc;
      if (deserializeJson(bodyDoc, data, len) || !bodyDoc["text"].is<const char*>()) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"expected {\\\"text\\\":\\\"...\\\"}\"}" );
        return;
      }
      const char* text = bodyDoc["text"].as<const char*>();
      if (strlen(text) > ECHO_TEXT_MAX) {
        char errMsg[ERROR_BODY_SIZE];
        snprintf(errMsg, sizeof(errMsg), "{\"error\":\"text exceeds %u characters\"}", (unsigned)ECHO_TEXT_MAX);
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", errMsg);
        return;
      }
      loraiot_Echo echo = loraiot_Echo_init_zero;
      strncpy(echo.text, text, sizeof(echo.text) - 1);
      const uint32_t sendMs = millis();
      uint32_t seq = 0;
      BrokerResult res = loraBrokerSend(BrokerRequestType::ECHO, addr, &echo, seq);
      if (res == BrokerResult::BUSY) {
        request->send(HTTP_STATUS_SERVICE_UNAVAILABLE, "application/json", "{\"error\":\"LoRa broker busy\"}");
        return;
      }
      if (res == BrokerResult::ENCODE_ERR) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"encode failed\"}");
        return;
      }
      const bool ok = (loraBrokerWait() == BrokerResult::OK);
      const uint32_t rttMs = millis() - sendMs;
      const loraiot_Response& resp = loraBrokerGetResponse();
      const bool hasEcho = ok && (resp.which_result == loraiot_Response_echo_tag);

      // Store result so GET /api/lora/echo/result can serve it without blocking.
      strncpy(lastEchoResult.sent,     echo.text,                          sizeof(lastEchoResult.sent) - 1);
      strncpy(lastEchoResult.received, hasEcho ? resp.result.echo.text : "", sizeof(lastEchoResult.received) - 1);
      lastEchoResult.seqNo       = seq;
      lastEchoResult.roundTripMs = rttMs;
      lastEchoResult.success     = ok;
      lastEchoResult.valid       = true;

      if (!ok) {
        request->send(HTTP_STATUS_GATEWAY_TIMEOUT, "application/json", "{\"error\":\"no response from node\"}");
        return;
      }
      JsonDocument doc;
      doc["addr"]          = addr;
      doc["seq"]           = seq;
      doc["status"]        = (resp.status == loraiot_Status_STATUS_OK) ? "ok" : "error";
      doc["echo"]          = hasEcho ? resp.result.echo.text : "";
      doc["round_trip_ms"] = rttMs;
      String json;
      serializeJson(doc, json);
      request->send(HTTP_STATUS_OK, "application/json", json);
    });

  // GET /api/lora/echo/result - return the result of the last POST /api/lora/nodes/echo.
  server->on("/api/lora/echo/result", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/lora/echo/result");
    if (!lastEchoResult.valid) {
      request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"no echo result yet\"}");
      return;
    }
    JsonDocument doc;
    doc["sent"]           = lastEchoResult.sent;
    doc["received"]       = lastEchoResult.received;
    doc["seqNo"]          = lastEchoResult.seqNo;
    doc["roundTripMs"]    = lastEchoResult.roundTripMs;
    doc["success"]        = lastEchoResult.success;
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // GET /api/lora/nodes/ping?addr=2  (also accepts POST for symmetry)
  auto pingHandler = [](AsyncWebServerRequest* request) {
    if (!request->hasArg("addr")) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"missing param: addr\"}");
      return;
    }
    uint8_t addr = (uint8_t)request->arg("addr").toInt();
    if (addr < LORA_DEFAULT_ADDR_REMOTE || addr > LORA_MAX_ADDR_REMOTE) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (2-254)\"}");
      return;
    }
    uint32_t seq = 0;
    BrokerResult res = loraBrokerSend(BrokerRequestType::PING, addr, nullptr, seq);
    if (res == BrokerResult::BUSY) {
      request->send(HTTP_STATUS_SERVICE_UNAVAILABLE, "application/json", "{\"error\":\"LoRa broker busy\"}");
      return;
    }
    if (loraBrokerWait() != BrokerResult::OK) {
      request->send(HTTP_STATUS_GATEWAY_TIMEOUT, "application/json", "{\"error\":\"no response from node\"}");
      return;
    }
    const PingResult& pr = loraBrokerGetPingResult();
    JsonDocument doc;
    doc["addr"]   = pr.addr;
    doc["rtt_ms"] = pr.rttMs;
    // The Ack carries no boot_id, so report the one learned from the node's last BEACON.
    // It is omitted (not 0) when the node never beaconed, to avoid a misleading value.
    uint32_t bootId = 0;
    if (SensorRegistry.getBootId(pr.addr, bootId)) {
      doc["boot_id"] = bootId;
    }
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  };
  server->on("/api/lora/nodes/ping", HTTP_GET,  pingHandler);
  server->on("/api/lora/nodes/ping", HTTP_POST, pingHandler);

  // GET /api/lora/ping  - broadcast PING (no addr param, sends to 0xFF)
  server->on("/api/lora/ping", HTTP_GET, [](AsyncWebServerRequest* request) {
    uint32_t seq = 0;
    BrokerResult res = loraBrokerSend(BrokerRequestType::PING, LORA_ADDR_BROADCAST, nullptr, seq);
    if (res == BrokerResult::BUSY) {
      request->send(HTTP_STATUS_SERVICE_UNAVAILABLE, "application/json", "{\"error\":\"LoRa broker busy\"}");
      return;
    }
    if (loraBrokerWait() != BrokerResult::OK) {
      request->send(HTTP_STATUS_GATEWAY_TIMEOUT, "application/json", "{\"error\":\"no response\"}");
      return;
    }
    // For a broadcast PING pr.addr is the node that answered first (not 0xFF).
    const PingResult& pr = loraBrokerGetPingResult();
    JsonDocument doc;
    doc["addr"]   = pr.addr;
    doc["rtt_ms"] = pr.rttMs;
    uint32_t bootId = 0;
    if (SensorRegistry.getBootId(pr.addr, bootId)) {
      doc["boot_id"] = bootId;
    }
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // POST /api/lora/sensor/data  (REMOTE only) - SoftAP sensor ingestion
  // Body: {"device":"SensorA","metrics":{"Voltage":{"value":335,"unit":"V","scale":100}}}
  server->on("/api/lora/sensor/data", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      if (!Gateway.isRemote()) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"endpoint not supported\"}");
        return;
      }
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
      }
      if (!doc["device"].is<const char*>() || !doc["metrics"].is<JsonObject>()) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json",
                      "{\"error\":\"expected {\\\"device\\\":\\\"name\\\",\\\"metrics\\\":{\\\"MetricName\\\":{\\\"value\\\":0}}}\"}");
        return;
      }
      const char* deviceName = doc["device"].as<const char*>();
      JsonObject metrics     = doc["metrics"].as<JsonObject>();
      JsonDocument respDoc;
      JsonArray accepted = respDoc["accepted"].to<JsonArray>();
      JsonArray rejected = respDoc["rejected"].to<JsonArray>();
      // Track unique device indices that had at least one accepted metric so we
      // can push each one to LOCAL over LoRa after the loop.
      uint32_t pushedDevIdx[DEVICE_TABLE_MAX_DEVICES];
      uint8_t  pushedCount = 0;
      for (JsonPair kv : metrics) {
        const char* metricName = kv.key().c_str();
        int32_t value = kv.value()["value"].as<int32_t>();
        const char* unit  = kv.value()["unit"].is<const char*>()  ? kv.value()["unit"].as<const char*>()  : "";
        uint32_t scale    = kv.value()["scale"].is<uint32_t>()    ? kv.value()["scale"].as<uint32_t>()    : 1;
        uint32_t devIdx = 0, metricId = 0;
        if (SensorRegistry.registerMetric(LORA_DEFAULT_ADDR_LOCAL, deviceName, metricName, unit, scale, devIdx, metricId)) {
          SensorRegistry.storeValue(LORA_DEFAULT_ADDR_LOCAL, devIdx, metricId, value);
          JsonObject a = accepted.add<JsonObject>();
          a["metric"]    = metricName;
          a["device_idx"]= devIdx;
          a["metric_id"] = metricId;
          // Record devIdx if not already in the list.
          bool seen = false;
          for (uint8_t i = 0; i < pushedCount; i++) {
            if (pushedDevIdx[i] == devIdx) {
              seen = true;
              break;
            }
          }
          if (!seen && pushedCount < DEVICE_TABLE_MAX_DEVICES) {
            pushedDevIdx[pushedCount++] = devIdx;
          }
        } else {
          rejected.add(metricName);
        }
      }
      // Fire-and-forget LoRa push for each unique accepted device.
      for (uint8_t i = 0; i < pushedCount; i++) {
        loraiot_Data loraData = loraiot_Data_init_zero;
        if (SensorRegistry.toProtoData(LORA_DEFAULT_ADDR_LOCAL, pushedDevIdx[i], loraData)) {
          loraPushData(LORA_DEFAULT_ADDR_LOCAL, loraData);
        }
      }
      respDoc["registry_rev"] = SensorRegistry.getRegistryRev(LORA_DEFAULT_ADDR_LOCAL);
      String json;
      serializeJson(respDoc, json);
      int status = rejected.size() > 0 ? HTTP_STATUS_BAD_REQUEST : HTTP_STATUS_OK;
      request->send(status, "application/json", json);
    });

  // GET /api/lora/nodes - list all known remote node addresses.
  // Registered LAST among all /api/lora/nodes/* routes because ESPAsyncWebServer
  // uses first-match-wins and this bare path would otherwise swallow all sub-paths.
  server->on("/api/lora/nodes", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/lora/nodes");
    uint8_t addrs[LORA_MAX_PEERS];
    uint8_t count = SensorRegistry.listPeers(addrs, LORA_MAX_PEERS);
    JsonDocument doc;
    JsonArray arr = doc["nodes"].to<JsonArray>();
    for (uint8_t i = 0; i < count; i++) {
      uint32_t bootId, regRev, lastSeenMs;
      bool online, crawlPending;
      if (SensorRegistry.getNodeInfo(addrs[i], bootId, regRev, lastSeenMs, online, crawlPending)) {
        JsonObject obj = arr.add<JsonObject>();
        obj["addr"]          = addrs[i];
        obj["boot_id"]       = bootId;
        obj["registry_rev"]  = regRev;
        obj["last_seen_ms"]  = lastSeenMs;
        obj["online"]        = online;
        obj["devices"]       = SensorRegistry.getDeviceCount(addrs[i]);
        obj["crawl_pending"] = crawlPending;
      }
    }
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  Log_info("RestService: routes registered.");
}
