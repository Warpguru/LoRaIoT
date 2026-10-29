#pragma once

#include <ESPAsyncWebServer.h>

/**
 * WebSocketService.h - WebSocket broadcast task (server push only).
 *
 * wsService.begin() starts the broadcast task (1 s push to all clients).
 *
 * The WebSocket endpoint is /ws/data (defined as WS_PATH in Server.h).
 * Clients receive a JSON snapshot every 1 s (schema: see WebSocketService.cpp).
 * Inbound messages from clients are not processed (read-only dashboard).
 */
class WebSocketService {
 public:
  explicit WebSocketService(AsyncWebSocket* webSocket);

  /** Starts the FreeRTOS broadcast task. On dual-core builds pins to Core 1. Call once from setupServer(). */
  void begin();

 private:
  AsyncWebSocket* ws;
  static void broadcastTask(void* param);
};
