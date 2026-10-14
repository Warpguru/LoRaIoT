#include "Application.h"

#include <Arduino.h>

#include "LogBuffer.h"
#include "Server.h"
#include "StatusLed.h"
#include "Tier.h"
#include "../../diagnostics/src/Diagnostics.h"

/**
 * Application.cpp - Main application setup and loop.
 *
 * Constructs the LoRa transport and global singletons.
 * HTTP handler logic lives in service/RestService.
 * WebSocket broadcast + receive live in service/WebSocketService.
 */

// ---- applicationSetup ------------------------------------------------------

void applicationSetup() {
  // Turn the status LED on immediately (BOOTING state) before anything else.
  // The led task becomes the sole owner of STATUS_LED_PIN from this point forward.
  statusLed.begin();

  Serial.begin(115200);
  delay(1000);

  // Initialise the levelled logger. Must be called before any Log_* call.
  Log.begin();

  // Load the persisted gateway tier (LOCAL/REMOTE) from NVS. Must be called
  // before setupServer() so the captive portal can pre-select the saved tier.
  Gateway.begin();

  Log_info("Starting LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ") tier=%s logLevel=%s",
           Gateway.getTierName(), Log.getLevelName());
  Serial.printf("\n--- LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ") tier=%s logLevel=%s ---\n",
                Gateway.getTierName(), Log.getLevelName());

  // TODO: initialise E220 LoRa module (configure UART2 at 9600 for config
  // mode, write registers, switch to 115200 normal mode).
  // See doc/Feasibility.md §3.3 for the full configuration sequence.

  // Initialize WiFi, construct service objects, register all routes, start server.
  setupServer();

  // TODO: set READY once LoRa module is confirmed online; use NO_DEVICE until then.
  // The E220 check runs from applicationLoop() (after this point) and switches the
  // LED to NO_DEVICE itself if the module does not answer.
  statusLed.setState(LedState::READY);
}

// ---- applicationLoop -------------------------------------------------------

void applicationLoop() {
  // E220 wiring check: runs on the first pass (boot) and on GET /api/diag/e220.
  // Remove when Milestone 3 (LoRaTransport) takes over UART2.
  diagnosticsLoop();

  // ESPAsyncWebServer is fully non-blocking; this is a no-op kept for symmetry.
  handleServerRequests();

  // TODO: drain WebSocket command queue and dispatch LoRa/device actions.
  // Mirror the pattern in SerialController/Application.cpp applicationLoop().
}
