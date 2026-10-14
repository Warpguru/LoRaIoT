#include "Server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "../../service/src/RestService.h"
#include "../../service/src/WebSocketService.h"
#include "../../diagnostics/src/ESPInfo.h"
#include "../../util/src/LogBuffer.h"
#include "../../util/src/StatusLed.h"
#include "Tier.h"
#include "index_html.h"
#include "openapi_json.h"

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

// ---- OpenAPI ---------------------------------------------------------------

static void handleGetOpenApiJson(AsyncWebServerRequest* request) {
  Log_info("GET /openapi");
  request->send_P(HTTP_CODE_OK, "application/json", OPENAPI_JSON);
}

/**
 * GET /openapi/ui - Swagger UI loaded from unpkg CDN (swagger-ui-dist@5.33.0).
 *
 * The HTML stub is ~600 bytes served from DRAM (String). The browser fetches
 * Swagger UI JS/CSS from unpkg.com and then fetches /openapi from this
 * device. Requires the developer's browser to have internet access; the ESP32
 * itself does not.
 */
static void handleGetOpenApiUi(AsyncWebServerRequest* request) {
  Log_info("GET /openapi/ui");
  String html =
      "<!DOCTYPE html>"
      "<html lang=\"en\"><head>"
      "<meta charset=\"utf-8\"/>"
      "<title>LoRaIoT API</title>"
      "<link rel=\"stylesheet\" href=\"https://unpkg.com/swagger-ui-dist@5.33.0/swagger-ui.css\"/>"
      "<style>"
      ".banner{background:#f8f9fa;border-bottom:1px solid #e9ecef;padding:8px 16px;"
      "font-family:sans-serif;font-size:13px;color:#495057}"
      ".banner strong{color:#212529}"
      "</style>"
      "</head><body>"
      "<div class=\"banner\">"
      "IP: <strong>" + WiFi.localIP().toString() + "</strong>"
      " | SSID: <strong>" + WiFi.SSID() + "</strong>"
      " | Tier: <strong>" + Gateway.getTierName() + "</strong>"
      " | Built: <strong>" __DATE__ " " __TIME__ "</strong>"
      "</div>"
      "<div id=\"swagger-ui\"></div>"
      "<script src=\"https://unpkg.com/swagger-ui-dist@5.33.0/swagger-ui-bundle.js\"></script>"
      "<script>"
      "window.onload=()=>{"
      "window.ui=SwaggerUIBundle({"
      "url:'/openapi',"
      "dom_id:'#swagger-ui',"
      "presets:[SwaggerUIBundle.presets.apis,SwaggerUIBundle.SwaggerUIStandalonePreset],"
      "layout:'BaseLayout'"
      "});"
      "};"
      "</script>"
      "</body></html>";
  request->send(HTTP_CODE_OK, "text/html", html);
}

// ---- setupServer -----------------------------------------------------------

void setupServer() {
  Log_info("Initializing WiFiManager...");

  // Each WiFiManagerParameter wraps one <select> element. WiFiManager posts
  // the submitted value back to getValue() on the corresponding object.
  // Two separate objects are required - a single object with two <select>
  // elements inside only returns the first field's value via getValue().
  // Tier selector comes first (more important choice on a fresh device).

  // WiFiManagerParameter(id, label, default, length, customHTML):
  //   - id      : name of the POST field; getValue() retrieves this field's value
  //   - label   : rendered as a <label> by WiFiManager (we suppress it with "")
  //   - default : pre-filled value (we set it to the current persisted value)
  //   - length  : max length of the returned value
  //   - customHTML : replaces WiFiManager's default <input> with our <select>
  //
  // Using the raw-HTML-only constructor WiFiManagerParameter(html) leaves id=""
  // so getValue() always returns an empty string - that was the original bug.

  // -- Tier selector (LOCAL / REMOTE) --
  size_t tierCount;
  const Tier* const* tiers = Tier::values(tierCount);
  const char* tierNames[8];
  for (size_t i = 0; i < tierCount && i < 8; i++) tierNames[i] = tiers[i]->name();
  // Build the <select> options; the wrapping <select name='tier'> is supplied
  // by WiFiManager when it renders the customHTML around the registered id.
  // We use a full <select> override via customHTML so the dropdown appears
  // instead of the default text input.
  char tierOptions[200];
  tierOptions[0] = '\0';
  for (size_t i = 0; i < tierCount && i < 8; i++) {
    const char* sel = (strcmp(Gateway.getTierName(), tierNames[i]) == 0) ? " selected" : "";
    char opt[60];
    snprintf(opt, sizeof(opt), "<option value='%s'%s>%s</option>",
             tierNames[i], sel, tierNames[i]);
    strncat(tierOptions, opt, sizeof(tierOptions) - strlen(tierOptions) - 1);
  }
  char tierCustomHtml[280];
  snprintf(tierCustomHtml, sizeof(tierCustomHtml),
           " type='hidden'><br/>"
           "<label for='tier'>Gateway Tier</label>"
           "<select id='tier' name='tier'>%s</select"
           , tierOptions);
  WiFiManagerParameter tierParam("tier", "", Gateway.getTierName(), 8, tierCustomHtml);
  wm.addParameter(&tierParam);

  // -- Log-level selector --
  size_t levelCount;
  const LogLevel* const* levels = LogLevel::values(levelCount);
  const char* levelNames[8];
  for (size_t i = 0; i < levelCount && i < 8; i++) levelNames[i] = levels[i]->name();
  char levelOptions[300];
  levelOptions[0] = '\0';
  for (size_t i = 0; i < levelCount && i < 8; i++) {
    const char* sel = (strcmp(Log.getLevelName(), levelNames[i]) == 0) ? " selected" : "";
    char opt[60];
    snprintf(opt, sizeof(opt), "<option value='%s'%s>%s</option>",
             levelNames[i], sel, levelNames[i]);
    strncat(levelOptions, opt, sizeof(levelOptions) - strlen(levelOptions) - 1);
  }
  char levelCustomHtml[350];
  snprintf(levelCustomHtml, sizeof(levelCustomHtml),
           " type='hidden'><br/>"
           "<label for='logLevel'>Log Level</label>"
           "<select id='logLevel' name='logLevel'>%s</select"
           , levelOptions);
  WiFiManagerParameter levelParam("logLevel", "", Log.getLevelName(), 8, levelCustomHtml);
  wm.addParameter(&levelParam);

  auto saveCallback = [&tierParam, &levelParam]() {
    const char* chosenTier  = tierParam.getValue();
    const char* chosenLevel = levelParam.getValue();

    if (chosenTier != nullptr && chosenTier[0] != '\0') {
      if (Gateway.setTierFromString(chosenTier)) {
        Gateway.saveTier();
        Log_info("Gateway tier set from portal to %s", Gateway.getTierName());
      } else {
        Log_warn("Portal returned unknown tier '%s' - ignored", chosenTier);
      }
    }
    if (chosenLevel != nullptr && chosenLevel[0] != '\0') {
      if (Log.setLevelFromString(chosenLevel)) {
        Log.saveLevel();
        Log_info("Log level set from portal to %s", Log.getLevelName());
      } else {
        Log_warn("Portal returned unknown log level '%s' - ignored", chosenLevel);
      }
    }
  };
  wm.setSaveParamsCallback(saveCallback);
  wm.setSaveConfigCallback(saveCallback);

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

  // Browser UI and API documentation
  server.on("/",            AsyncWebRequestMethod::HTTP_GET, handleRoot);
  server.on("/openapi",     AsyncWebRequestMethod::HTTP_GET, handleGetOpenApiJson);
  server.on("/openapi/ui",  AsyncWebRequestMethod::HTTP_GET, handleGetOpenApiUi);

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
