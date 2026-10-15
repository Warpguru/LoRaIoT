#include "Application.h"

#include <Arduino.h>

#include "../../util/src/LogBuffer.h"
#include "Server.h"
#include "../../util/src/StatusLed.h"
#include "Tier.h"
#include "LoRaConfig.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../diagnostics/src/Diagnostics.h"

/**
 * Application.cpp - Main application setup and loop.
 *
 * Constructs the LoRa transport and global singletons.
 * HTTP handler logic lives in service/RestService.
 * WebSocket broadcast + receive live in service/WebSocketService.
 */

// ---- Application Constants -------------------------------------------------
static constexpr uint32_t PING_INTERVAL_MS = 2000; // Ping every 2 seconds on LOCAL
static constexpr size_t   MSG_BUFFER_LEN   = 40;   // Buffer size for outgoing ping/reply strings
static constexpr uint32_t BOOT_DELAY_MS    = 1000; // Safe boot startup delay

// ---- LoRa Receive Callback -------------------------------------------------

static void onLoRaReceive(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) return;

  // Convert received payload to safe null-terminated string for logging and scanning
  String str = String((const char*)data);
  str.trim();

  if (strcmp(Gateway.getTierName(), "REMOTE") == 0) {
    // REMOTE Node: Logs receipt of ping, then immediately replies back with the same index
    Log_info("Received ping: %s", str.c_str());

    int pingIndex = 0;
    if (sscanf(str.c_str(), "Local send: %d", &pingIndex) == 1) {
      char replyMsg[MSG_BUFFER_LEN];
      snprintf(replyMsg, sizeof(replyMsg), "Remote reply: %d\n", pingIndex);
      
      // Asynchronously queue the reply frame
      LoRaTransport.sendRaw(reinterpret_cast<const uint8_t*>(replyMsg), strlen(replyMsg));
      Log_info("Sent reply: %d", pingIndex);
    }
  } else {
    // LOCAL Node: Simply logs the received ping-echo reply
    Log_info("Received reply: %s", str.c_str());
  }
}

// ---- applicationSetup ------------------------------------------------------

void applicationSetup() {
  // Turn the status LED on immediately (BOOTING state) before anything else.
  // The led task becomes the sole owner of STATUS_LED_PIN from this point forward.
  statusLed.begin();

  Serial.begin(LoRaTransportClass::NORMAL_BAUD);
  delay(BOOT_DELAY_MS);

  // Initialise the levelled logger. Must be called before any Log_* call.
  LogBuffer.begin();

  // Load the persisted gateway tier (LOCAL/REMOTE) from NVS. Must be called
  // before setupServer() so the captive portal can pre-select the saved tier.
  Gateway.begin();

  // Initialize and load the LoRa settings (channel and address) from NVS.
  LoRaConfig.begin();

  // Apply the loaded CPU frequency config
  setCpuFrequencyMhz(LoRaConfig.getCpuFreq());

  Log_info("Starting LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ") tier=%s logLevel=%s loraChannel=0x%02X loraAddr=0x%02X cpuFreq=%d MHz",
           Gateway.getTierName(), LogBuffer.getLevelName(), LoRaConfig.getChannel(), LoRaConfig.getAddress(), getCpuFrequencyMhz());
  Serial.printf("\n--- LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ") tier=%s logLevel=%s loraChannel=0x%02X loraAddr=0x%02X cpuFreq=%d MHz ---\n",
                Gateway.getTierName(), LogBuffer.getLevelName(), LoRaConfig.getChannel(), LoRaConfig.getAddress(), getCpuFrequencyMhz());

  // Set the transport layer callback and initialize E220 hardware
  LoRaTransport.setReceiveCallback(onLoRaReceive);
  LoRaTransport.begin();

  // Initialize WiFi, construct service objects, register all routes, start server.
  setupServer();

  // LoRa module is configured and online; transition the status LED to READY
  statusLed.setState(LedState::READY);
}

// ---- applicationLoop -------------------------------------------------------

void applicationLoop() {
  // Service any pending on-demand hardware wiring checks
  diagnosticsLoop();

  static unsigned long lastSendTime = 0;
  static int pingCounter = 0;

  // LOCAL Node: Periodically transmit pings to REMOTE
  if (strcmp(Gateway.getTierName(), "LOCAL") == 0) {
    unsigned long now = millis();
    if (now - lastSendTime >= PING_INTERVAL_MS) {
      lastSendTime = now;

      char pingMsg[MSG_BUFFER_LEN];
      snprintf(pingMsg, sizeof(pingMsg), "Local send: %d\n", pingCounter);
      
      // Asynchronously queue the ping frame
      LoRaTransport.sendRaw(reinterpret_cast<const uint8_t*>(pingMsg), strlen(pingMsg));
      Log_info("Sent: %d", pingCounter);
      pingCounter++;
    }
  }

  // ESPAsyncWebServer is fully non-blocking; this is a no-op kept for symmetry.
  handleServerRequests();

  // TODO: drain WebSocket command queue and dispatch LoRa/device actions.
  // Mirror the pattern in SerialController/Application.cpp applicationLoop().
}
