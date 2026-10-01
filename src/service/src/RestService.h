#pragma once

#include <ESPAsyncWebServer.h>

/**
 * RestService.h - All /api/* HTTP handler registration.
 *
 * Mirrors SerialController RestService pattern:
 *   restService.registerRoutes() is called once from setupServer().
 *
 * Add LoRaIoT-specific REST endpoints here (e.g. GET /api/state,
 * GET /api/lora/status, PUT /api/lora/config, etc.).
 */
class RestService {
 public:
  explicit RestService(AsyncWebServer* server);
  void registerRoutes();

 private:
  AsyncWebServer* _server;
};
