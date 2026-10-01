#include "Application.h"

#include <Arduino.h>

#include "LogBuffer.h"
#include "Server.h"
#include "StatusLed.h"

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

  Log_info("Starting LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ")");
  Serial.println("\n--- LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ") ---");

  // TODO: initialise E220 LoRa module (configure UART2 at 9600 for config
  // mode, write registers, switch to 115200 normal mode).
  // See doc/Feasibility.md §3.3 for the full configuration sequence.

  // Initialize WiFi, construct service objects, register all routes, start server.
  setupServer();

  // TODO: set READY once LoRa module is confirmed online; use NO_DEVICE until then.
  statusLed.setState(LedState::READY);
}

// ---- applicationLoop -------------------------------------------------------

extern WebSocketService wsService;

void applicationLoop() {
  // ESPAsyncWebServer is fully non-blocking; this is a no-op kept for symmetry.
  handleServerRequests();

  // TODO: drain WebSocket command queue and dispatch LoRa/device actions.
  // Mirror the pattern in SerialController/Application.cpp applicationLoop().
}
