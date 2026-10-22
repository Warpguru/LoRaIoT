#pragma once

#include <ESPAsyncWebServer.h>
#include "../../lora/src/nanopb/loraiot.pb.h"

/**
 * RestService.h - All /api/* HTTP handler registration.
 *
 * Mirrors SerialController RestService pattern:
 *   restService.registerRoutes() is called once from setupServer().
 *
 * Add LoRaIoT-specific REST endpoints here (e.g. GET /api/state,
 * GET /api/lora/status, PUT /api/lora/config, etc.).
 *
 * API documentation: doc/RestApi.md (OpenAPI specification to follow).
 */

/**
 * Result of the last POST /api/lora/nodes/echo exchange.
 * Read by WebSocketService to include echo health in the 1s broadcast.
 * Buffer sizes are derived from the generated loraiot_Echo struct.
 */
struct EchoResult {
  char     sent[sizeof(loraiot_Echo::text)];
  char     received[sizeof(loraiot_Echo::text)];
  uint32_t seqNo;
  uint32_t roundTripMs;
  bool     success;
  bool     valid;  // false until the first echo completes
};

/** Returns a const reference to the last echo result (valid field is false if none yet). */
const EchoResult& restServiceGetLastEcho();

class RestService {
 public:
  explicit RestService(AsyncWebServer* webServer);
  void registerRoutes();

 private:
  AsyncWebServer* server;
};
