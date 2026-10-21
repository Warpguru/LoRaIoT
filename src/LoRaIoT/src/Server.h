#pragma once

/**
 * Server.h - Public API for WiFi management and the RESTful HTTP server.
 *
 * Initialise WiFi via WiFiManager and start the ESPAsyncWebServer by calling
 * setupServer() once from applicationSetup(). handleServerRequests() is a
 * no-op kept for API symmetry - ESPAsyncWebServer is fully non-blocking.
 */

#include "../../service/src/WebSocketService.h"

// ---- Server configuration constants ----------------------------------------

/** TCP port the HTTP server listens on. */
static constexpr int SERVER_PORT = 80;

/** WebSocket endpoint path. */
static constexpr const char* WS_PATH = "/ws/data";

/**
 * Delay in milliseconds between sending the /reset acknowledgement response
 * and calling wm.resetSettings() + ESP.restart().
 */
static constexpr int RESET_REBOOT_DELAY_MS = 200;

// ---- HTTP status codes ------------------------------------------------------

static constexpr int HTTP_STATUS_OK                  = 200;
static constexpr int HTTP_STATUS_ACCEPTED            = 202;
static constexpr int HTTP_STATUS_BAD_REQUEST         = 400;
static constexpr int HTTP_STATUS_NOT_FOUND           = 404;
static constexpr int HTTP_STATUS_GATEWAY_TIMEOUT     = 504;
static constexpr int HTTP_STATUS_SERVICE_UNAVAILABLE = 503;

// ---- Global objects (defined in Server.cpp) ---------------------------------

/** WebSocket broadcast service. Constructed in Server.cpp, started by setupServer(). */
extern WebSocketService wsService;

// ---- Public functions -------------------------------------------------------

/**
 * Connects to WiFi using WiFiManager and starts the async HTTP + WebSocket server.
 * Must be called once from applicationSetup().
 */
void setupServer();

/**
 * No-op: ESPAsyncWebServer handles all requests on its own internal FreeRTOS task.
 * Kept for API compatibility with the call site in applicationLoop().
 */
void handleServerRequests();
