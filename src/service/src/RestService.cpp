#include "RestService.h"

#include <ArduinoJson.h>

#include "../../util/src/LogBuffer.h"
#include "../../LoRaIoT/src/Tier.h"
#include "../../LoRaIoT/src/LoRaConfig.h"
#include "../../LoRaIoT/src/Server.h"
#include "../../diagnostics/src/Diagnostics.h"

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
        if (chan < 0 || chan > 31) {
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"Channel must be between 0 and 31\"}");
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
        if (addr < 1 || addr > 254) {
          request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"Address must be between 1 and 254\"}");
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
          char errMsg[128];
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

  Log_info("RestService: routes registered.");
}
