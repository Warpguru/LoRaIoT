#include "RestService.h"

#include <ArduinoJson.h>

#include "../../LoRaIoT/src/LogBuffer.h"

/**
 * RestService.cpp - LoRaIoT REST API handlers.
 *
 * Add endpoints here following the SerialController pattern:
 *   - Read state under mutex, serialise with ArduinoJson, send response.
 *   - Write operations: validate input, dispatch to relevant service/task.
 *
 * HTTP status code constants are defined in Server.h.
 */

RestService::RestService(AsyncWebServer* server) : _server(server) {}

void RestService::registerRoutes() {
  // TODO: register LoRaIoT-specific /api/* endpoints.
  // Example skeleton:
  //
  // _server->on("/api/state", HTTP_GET, [](AsyncWebServerRequest* request) {
  //   Log_info("GET /api/state");
  //   JsonDocument doc;
  //   // fill doc from shared state...
  //   String json;
  //   serializeJson(doc, json);
  //   request->send(200, "application/json", json);
  // });

  Log_info("RestService: routes registered (none yet - add in RestService.cpp).");
}
