#pragma once

#include <ESPAsyncWebServer.h>

/**
 * WebSocketService.h - WebSocket broadcast task + command queue.
 *
 * Mirrors SerialController WebSocketService pattern:
 *   wsService.begin() starts the broadcast task (1 s push to all clients).
 *
 * The WebSocket endpoint is /ws/data (defined as WS_PATH in Server.h).
 * Clients receive a JSON snapshot every 1 s.
 * Clients send JSON commands which are queued and dispatched in applicationLoop().
 */
class WebSocketService {
 public:
  explicit WebSocketService(AsyncWebSocket* ws);

  /** Starts the FreeRTOS broadcast task on Core 1. Call once from setupServer(). */
  void begin();

 private:
  AsyncWebSocket* _ws;
  static void broadcastTask(void* param);
};
