#include "RestService.h"

#include <ArduinoJson.h>

#include "../../util/src/LogBuffer.h"
#include "../../LoRaIoT/src/Tier.h"
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
    request->send(202, "application/json", "{\"status\":\"requested\",\"result\":\"see GET /api/log\"}");
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
    request->send(200, "application/json", json);
  });

  // TODO: register LoRaIoT-specific /api/* endpoints.
  Log_info("RestService: routes registered.");
}
