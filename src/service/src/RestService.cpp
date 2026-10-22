#include "RestService.h"

#include <ArduinoJson.h>

#include "../../util/src/LogBuffer.h"
#include "../../LoRaIoT/src/Tier.h"
#include "../../LoRaIoT/src/LoRaConfig.h"
#include "../../LoRaIoT/src/Server.h"
#include "../../diagnostics/src/Diagnostics.h"
#include "../../LoRaIoT/src/DeviceRegistry.h"
#include "../../LoRaIoT/src/DescribeCrawler.h"
#include "../../LoRaIoT/src/SensorRegistry.h"
#include "../../LoRaIoT/src/LoRaBroker.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../lora/src/nanopb/pb_encode.h"

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

static constexpr size_t ERROR_BODY_SIZE = 128;  // Buffer for a formatted JSON error response

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
        if (strcmp(Gateway.getTierName(), "REMOTE") != 0) {
          Log_warn("PUT /api/lora/address failed - not a REMOTE instance");
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"Address API only available on REMOTE instances\"}");
        }
      },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (strcmp(Gateway.getTierName(), "REMOTE") != 0) {
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

  // GET /api/lora/nodes - list all known remote node addresses.
  server->on("/api/lora/nodes", HTTP_GET, [](AsyncWebServerRequest* request) {
    Log_info("GET /api/lora/nodes");
    uint8_t addrs[DEVICE_REGISTRY_MAX_NODES];
    uint8_t count = DeviceRegistry.listNodes(addrs, DEVICE_REGISTRY_MAX_NODES);
    JsonDocument doc;
    JsonArray arr = doc["nodes"].to<JsonArray>();
    for (uint8_t i = 0; i < count; i++) {
      NodeEntry node;
      if (DeviceRegistry.getNode(addrs[i], node)) {
        JsonObject obj = arr.add<JsonObject>();
        obj["addr"]         = node.addr;
        obj["boot_id"]      = node.bootId;
        obj["registry_rev"] = node.registryRev;
        obj["last_seen_ms"] = node.lastSeenMs;
        obj["devices"]      = node.deviceCount;
        obj["crawl_pending"]= node.crawlPending;
      }
    }
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // GET /api/lora/nodes/devices?addr=2[&device=1][&refresh=true]
  // Without 'device': returns all devices on the node.
  // With 'device': returns the single named/indexed device.
  // With 'refresh=true': schedules a fresh Describe crawl before responding.
  server->on("/api/lora/nodes/devices", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!request->hasArg("addr")) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"missing param: addr\"}");
      return;
    }
    uint8_t addr = (uint8_t)request->arg("addr").toInt();
    if (addr < LORA_DEFAULT_ADDR_REMOTE || addr > LORA_MAX_ADDR_REMOTE) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (2-254)\"}");
      return;
    }

    // Optional refresh: schedule a Describe crawl; caller must poll again
    if (request->hasArg("refresh") && request->arg("refresh") == "true") {
      describeCrawlerSchedule(addr);
      Log_info("GET /api/lora/nodes/devices?addr=%u&refresh=true - crawl scheduled", addr);
    }

    NodeEntry node;
    if (!DeviceRegistry.getNode(addr, node)) {
      request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"node not found\"}");
      return;
    }

    // Optional single-device filter
    bool hasDevFilter = request->hasArg("device");
    String devVal     = hasDevFilter ? request->arg("device") : String();

    Log_info("GET /api/lora/nodes/devices?addr=%u", addr);
    JsonDocument doc;
    doc["addr"]         = node.addr;
    doc["registry_rev"] = node.registryRev;
    doc["crawl_pending"]= node.crawlPending;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (uint8_t i = 0; i < node.deviceCount; i++) {
      const CachedDevice& cd = node.devices[i];
      // Filter by device index or name if param given
      if (hasDevFilter) {
        bool matchIdx  = (devVal.toInt() == (int)cd.def.device);
        bool matchName = (devVal == cd.def.name);
        if (!matchIdx && !matchName) continue;
      }
      JsonObject obj = arr.add<JsonObject>();
      obj["device"] = cd.def.device;
      obj["name"]   = cd.def.name;
      JsonArray metrics = obj["metrics"].to<JsonArray>();
      for (uint8_t m = 0; m < cd.def.metrics_count; m++) {
        JsonObject mo = metrics.add<JsonObject>();
        mo["id"]    = cd.def.metrics[m].id;
        mo["name"]  = cd.def.metrics[m].name;
        mo["unit"]  = cd.def.metrics[m].unit;
        mo["scale"] = cd.def.metrics[m].scale;
      }
    }
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // GET /api/lora/nodes/metrics - full handler with live=true support registered below in Phase 2e.

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
  // Phase 2e: Blocking endpoints - require LoRaBroker (LOCAL only)
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
      if (addr < LORA_DEFAULT_ADDR_REMOTE || addr > LORA_MAX_ADDR_REMOTE) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (2-254)\"}");
        return;
      }
      JsonDocument bodyDoc;
      if (deserializeJson(bodyDoc, data, len) || !bodyDoc["text"].is<const char*>()) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"expected {\\\"text\\\":\\\"...\\\"}\"}" );
        return;
      }
      const char* text = bodyDoc["text"].as<const char*>();
      static constexpr size_t ECHO_MAX_LEN = 175;
      if (strlen(text) > ECHO_MAX_LEN) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"text exceeds 175 characters\"}");
        return;
      }
      loraiot_Echo echo = loraiot_Echo_init_zero;
      strncpy(echo.text, text, sizeof(echo.text) - 1);
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
      if (loraBrokerWait() != BrokerResult::OK) {
        request->send(HTTP_STATUS_GATEWAY_TIMEOUT, "application/json", "{\"error\":\"no response from node\"}");
        return;
      }
      const loraiot_Response& resp = loraBrokerGetResponse();
      JsonDocument doc;
      doc["addr"]    = addr;
      doc["seq"]     = seq;
      doc["status"]  = (resp.status == loraiot_Status_STATUS_OK) ? "ok" : "error";
      doc["echo"]    = (resp.which_result == loraiot_Response_echo_tag) ? resp.result.echo.text : "";
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
    doc["addr"]    = addr;
    doc["boot_id"] = pr.bootId;
    doc["rtt_ms"]  = pr.rttMs;
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
    const PingResult& pr = loraBrokerGetPingResult();
    JsonDocument doc;
    doc["addr"]    = LORA_ADDR_BROADCAST;
    doc["boot_id"] = pr.bootId;
    doc["rtt_ms"]  = pr.rttMs;
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // GET /api/lora/nodes/metrics with live=true - now wired to the broker
  // Override the 503 stub registered in 2c by re-registering the handler.
  // ESPAsyncWebServer uses last-registered wins for duplicate paths.
  server->on("/api/lora/nodes/metrics", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!request->hasArg("addr") || !request->hasArg("device")) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"missing params: addr, device\"}");
      return;
    }
    uint8_t addr      = (uint8_t)request->arg("addr").toInt();
    uint8_t deviceIdx = (uint8_t)request->arg("device").toInt();
    if (addr < LORA_DEFAULT_ADDR_REMOTE || addr > LORA_MAX_ADDR_REMOTE) {
      request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (2-254)\"}");
      return;
    }

    bool liveRequested = request->hasArg("live") && request->arg("live") == "true";
    if (liveRequested) {
      uint32_t timeoutMs = BROKER_DEFAULT_TIMEOUT_MS;
      if (request->hasArg("timeout")) {
        uint32_t t = (uint32_t)request->arg("timeout").toInt();
        if (t >= BROKER_MIN_TIMEOUT_MS && t <= BROKER_MAX_TIMEOUT_MS) timeoutMs = t;
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
      // Store live data in the registry for future cached reads
      DeviceRegistry.storeData(addr, resp.result.data);
    }

    // Serve from cache (populated by live read above or previous DATA frame)
    CachedDevice cd;
    if (!DeviceRegistry.getDevice(addr, deviceIdx, cd)) {
      request->send(HTTP_STATUS_NOT_FOUND, "application/json", "{\"error\":\"device not found\"}");
      return;
    }
    JsonDocument doc;
    doc["addr"]     = addr;
    doc["device"]   = cd.def.device;
    doc["name"]     = cd.def.name;
    doc["has_data"] = cd.hasData;
    doc["age_ms"]   = cd.hasData ? cd.lastData.age_ms : 0;
    doc["alert"]    = cd.hasData && cd.lastData.alert;
    doc["live"]     = liveRequested;
    JsonArray vals  = doc["values"].to<JsonArray>();
    if (cd.hasData) {
      for (uint8_t i = 0; i < cd.lastData.values_count; i++) {
        const loraiot_Measurement& m = cd.lastData.values[i];
        JsonObject vo = vals.add<JsonObject>();
        vo["metric_id"] = m.metric_id;
        vo["value"]     = m.value;
        for (uint8_t mi = 0; mi < cd.def.metrics_count; mi++) {
          if (cd.def.metrics[mi].id == m.metric_id) {
            vo["name"]  = cd.def.metrics[mi].name;
            vo["unit"]  = cd.def.metrics[mi].unit;
            vo["scale"] = cd.def.metrics[mi].scale;
            break;
          }
        }
      }
    }
    String json;
    serializeJson(doc, json);
    request->send(HTTP_STATUS_OK, "application/json", json);
  });

  // POST /api/sensor/data  (REMOTE only) - SoftAP sensor ingestion
  // Body: {"device":"SensorA","metrics":{"Voltage":{"value":335,"unit":"V","scale":100}}}
  server->on("/api/sensor/data", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      if (strcmp(Gateway.getTierName(), "REMOTE") != 0) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"endpoint only available on REMOTE\"}");
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
      for (JsonPair kv : metrics) {
        const char* metricName = kv.key().c_str();
        int32_t value = kv.value()["value"].as<int32_t>();
        const char* unit  = kv.value()["unit"].is<const char*>()  ? kv.value()["unit"].as<const char*>()  : "";
        uint32_t scale    = kv.value()["scale"].is<uint32_t>()    ? kv.value()["scale"].as<uint32_t>()    : 1;
        uint32_t devIdx = 0, metricId = 0;
        if (SensorRegistry.registerMetric(deviceName, metricName, unit, scale, devIdx, metricId)) {
          SensorRegistry.storeValue(devIdx, metricId, value);
          JsonObject a = accepted.add<JsonObject>();
          a["metric"]    = metricName;
          a["device_idx"]= devIdx;
          a["metric_id"] = metricId;
        } else {
          rejected.add(metricName);
        }
      }
      respDoc["registry_rev"] = SensorRegistry.getRegistryRev();
      String json;
      serializeJson(respDoc, json);
      int status = rejected.size() > 0 ? HTTP_STATUS_BAD_REQUEST : HTTP_STATUS_OK;
      request->send(status, "application/json", json);
    });

  // -------------------------------------------------------------------------
  // Phase 2f: Fire-and-forget endpoints (no blocking wait)
  // -------------------------------------------------------------------------

  // POST /api/lora/beacon  - trigger an immediate manual BEACON from LOCAL
  server->on("/api/lora/beacon", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      // Build a minimal Beacon and send it broadcast. Phase 3 will wrap this
      // in a proper BEACON-type frame header.
      const char beaconMsg[] = "BEACON\n";
      LoRaTransport.sendRaw(LORA_ADDR_BROADCAST, reinterpret_cast<const uint8_t*>(beaconMsg), sizeof(beaconMsg) - 1);
      Log_info("POST /api/lora/beacon: manual BEACON queued [Phase2 stub]");
      request->send(HTTP_STATUS_ACCEPTED, "application/json", "{\"status\":\"queued\"}");
    });

  // POST /api/lora/nodes/data?addr=2[&ack=true]
  // Body: {"device":1,"values":[{"metric_id":1,"value":335}]}
  server->on("/api/lora/nodes/data", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
      if (!request->hasArg("addr")) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"missing param: addr\"}");
        return;
      }
      uint8_t addr = (uint8_t)request->arg("addr").toInt();
      if (addr < LORA_DEFAULT_ADDR_REMOTE || addr > LORA_MAX_ADDR_REMOTE) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"addr out of range (2-254)\"}");
        return;
      }
      bool ack = request->hasArg("ack") && request->arg("ack") == "true";
      JsonDocument bodyDoc;
      if (deserializeJson(bodyDoc, data, len)) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
      }
      // Encode the Data payload and queue it. Phase 3 will add frame header + CRC.
      loraiot_Data dataMsg = loraiot_Data_init_zero;
      dataMsg.device       = bodyDoc["device"].as<uint32_t>();
      JsonArray vals       = bodyDoc["values"].as<JsonArray>();
      for (JsonObject v : vals) {
        if (dataMsg.values_count >= 16) break;
        dataMsg.values[dataMsg.values_count].metric_id = v["metric_id"].as<uint32_t>();
        dataMsg.values[dataMsg.values_count].value     = v["value"].as<int32_t>();
        dataMsg.values_count++;
      }
      uint8_t buf[LORA_MAX_PAYLOAD_LEN];
      pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
      if (!pb_encode(&stream, loraiot_Data_fields, &dataMsg)) {
        request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"encode failed\"}");
        return;
      }
      LoRaTransport.sendRaw(addr, buf, stream.bytes_written);
      Log_info("POST /api/lora/nodes/data?addr=%u&ack=%s: %u bytes queued [Phase2 stub]",
               addr, ack ? "true" : "false", (unsigned)stream.bytes_written);
      JsonDocument respDoc;
      respDoc["addr"]  = addr;
      respDoc["ack"]   = ack;
      respDoc["bytes"] = stream.bytes_written;
      String json;
      serializeJson(respDoc, json);
      request->send(HTTP_STATUS_ACCEPTED, "application/json", json);
    });

  Log_info("RestService: routes registered.");
}
