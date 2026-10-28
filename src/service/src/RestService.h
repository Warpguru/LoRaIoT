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
  char     sent[sizeof(loraiot_Echo::text)];      ///< Text payload that was sent in the echo request.
  char     received[sizeof(loraiot_Echo::text)];  ///< Text payload echoed back by the remote node.
  uint32_t seqNo;                                 ///< Frame sequence number assigned to the echo request.
  uint32_t roundTripMs;                           ///< Round-trip time from send to ACK receipt (ms).
  bool     success;                               ///< True if the remote node echoed the exact sent text.
  bool     valid;                                 ///< False until at least one echo exchange has completed.
};

/** Returns a const reference to the last echo result (valid field is false if none yet). */
const EchoResult& restServiceGetLastEcho();

class RestService {
 public:
  /**
   * Construct the service with a pointer to the shared ESPAsyncWebServer instance.
   * The pointer must remain valid for the lifetime of the object.
   */
  explicit RestService(AsyncWebServer* webServer);

  /**
   * Register all /api/* routes on the server. Call once from setupServer() before
   * the server is started. Routes added here are documented in doc/RestApi.md and
   * served at GET /openapi.
   */
  void registerRoutes();

 private:
  AsyncWebServer* server;
};
