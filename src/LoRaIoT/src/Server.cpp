#include "Server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "../../service/src/RestService.h"
#include "../../service/src/WebSocketService.h"
#include "ESPInfo.h"
#include "LogBuffer.h"
#include "StatusLed.h"
#include "index_html.h"

/**
 * Server.cpp - WiFi initialisation, HTTP/WebSocket server wiring.
 *
 * - Connects to WiFi via WiFiManager captive portal
 * - Constructs RestService, WebSocketService
 * - Registers all routes and starts the server
 *
 * All /api/* handler logic lives in service/RestService.
 * WebSocket broadcast + command-receive live in service/WebSocketService.
 * ESP32-specific /status and /reset endpoints live here.
 */

// ---- Global server objects -------------------------------------------------

AsyncWebServer server(SERVER_PORT);
AsyncWebSocket ws(WS_PATH);
WebSocketService wsService(&ws);

WiFiManager wm;

// ---- Diagnostics endpoints (ESP32-specific) --------------------------------

static void handleGetStatus(AsyncWebServerRequest* request) {
  Log_info("GET /status");
  JsonDocument doc;
  JsonObject statusObject = doc.to<JsonObject>();
  fillESPInfo(statusObject);
  String json;
  serializeJson(doc, json);
  request->send(HTTP_CODE_OK, "application/json", json);
}

static void handleReset(AsyncWebServerRequest* request) {
  Log_warn("GET /reset - CLEARING SETTINGS");
  request->send(HTTP_CODE_OK, "text/plain",
                "WiFi settings cleared. ESP32 rebooting to Configuration Mode...");
  delay(RESET_REBOOT_DELAY_MS);
  wm.resetSettings();
  ESP.restart();
}

// ---- Log endpoints ---------------------------------------------------------

static void handleGetLog(AsyncWebServerRequest* request) {
  Log_info("GET /api/log");
  if (request->hasArg("clear") && request->arg("clear") == "1") {
    Log.clear();
  }
  String json;
  Log.getJson(json);
  request->send(HTTP_CODE_OK, "application/json", json);
}

static void handlePutLogLevel(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err || !doc["level"].is<const char*>()) {
    request->send(HTTP_CODE_BAD_REQUEST, "application/json", "{\"error\":\"Expected {\\\"level\\\":\\\"INFO\\\"}\"}");
    return;
  }
  const char* levelName = doc["level"].as<const char*>();
  if (!Log.setLevelFromString(levelName)) {
    request->send(HTTP_CODE_BAD_REQUEST, "application/json", "{\"error\":\"Unknown level\"}");
    return;
  }
  Log.saveLevel();
  Log_info("Log level changed to %s", Log.getLevelName());
  String json = "{\"level\":\"";
  json += Log.getLevelName();
  json += "\"}";
  request->send(HTTP_CODE_OK, "application/json", json);
}

// ---- Browser UI ------------------------------------------------------------

static void handleRoot(AsyncWebServerRequest* request) {
  Log_info("GET /");
  request->send_P(HTTP_CODE_OK, "text/html", INDEX_HTML);
}

// ---- setupServer -----------------------------------------------------------

void setupServer() {
  Log_info("Initializing WiFiManager...");

  // Inject log-level selector into the captive-portal form.
  char portalHtml[300];
  size_t remaining = sizeof(portalHtml);
  portalHtml[0] = '\0';
  strncat(portalHtml, "<br/><label for='logLevel'>Log Level</label>"
                      "<select id='logLevel' name='logLevel'>", remaining - 1);
  remaining -= strlen(portalHtml);

  size_t levelCount;
  const LogLevel* const* levels = LogLevel::values(levelCount);
  const char* currentLevel = Log.getLevelName();
  for (size_t i = 0; i < levelCount; i++) {
    const char* levelName = levels[i]->name();
    const char* selected = (strcmp(currentLevel, levelName) == 0) ? " selected" : "";
    char option[60];
    snprintf(option, sizeof(option), "<option value='%s'%s>%s</option>",
             levelName, selected, levelName);
    strncat(portalHtml, option, remaining - 1);
    remaining -= strlen(option);
  }
  strncat(portalHtml, "</select>", remaining - 1);

  WiFiManagerParameter logLevelParam(portalHtml);
  wm.addParameter(&logLevelParam);

  auto saveLevelCallback = [&logLevelParam]() {
    const char* chosen = logLevelParam.getValue();
    if (chosen != nullptr && chosen[0] != '\0' && Log.setLevelFromString(chosen)) {
      Log.saveLevel();
      Log_info("Log level set from portal to %s", Log.getLevelName());
    }
  };
  wm.setSaveParamsCallback(saveLevelCallback);
  wm.setSaveConfigCallback(saveLevelCallback);

  if (!wm.autoConnect("LoRaIoT")) {
    Log_error("WiFi Connection Failed! Halting with SOS signal.");
    statusLed.setState(LedState::FAULT);
    vTaskSuspend(NULL);
  }

  Log_info("WiFi Connected! IP: %s", WiFi.localIP().toString().c_str());

  // Construct and register REST routes.
  static RestService restService(&server);
  restService.registerRoutes();

  // Initialise WebSocket service.
  wsService.begin();
  server.addHandler(&ws);

  // Browser UI
  server.on("/", AsyncWebRequestMethod::HTTP_GET, handleRoot);

  // Diagnostics
  server.on("/api/log", AsyncWebRequestMethod::HTTP_GET, handleGetLog);
  server.on(
      "/api/log/level", AsyncWebRequestMethod::HTTP_PUT,
      [](AsyncWebServerRequest* request) {},
      nullptr,
      handlePutLogLevel);
  server.on("/status", AsyncWebRequestMethod::HTTP_GET, handleGetStatus);
  server.on("/reset",  AsyncWebRequestMethod::HTTP_GET, handleReset);

  server.begin();
  Log_info("AsyncWebServer started on port 80 (HTTP + WS on /ws/data).");
}

void handleServerRequests() {
  // intentionally empty - ESPAsyncWebServer is fully non-blocking
}
