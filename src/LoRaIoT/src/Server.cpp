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
#include "LoRaConfig.h"
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
  request->send(HTTP_STATUS_OK, "application/json", json);
}

static void handleReset(AsyncWebServerRequest* request) {
  Log_warn("GET /reset - CLEARING SETTINGS");
  request->send(HTTP_STATUS_OK, "text/plain",
                "WiFi settings cleared. ESP32 rebooting to Configuration Mode...");
  delay(RESET_REBOOT_DELAY_MS);
  wm.resetSettings();
  ESP.restart();
}

// ---- Log endpoints ---------------------------------------------------------

static void handleGetLog(AsyncWebServerRequest* request) {
  Log_info("GET /api/log");
  if (request->hasArg("clear") && request->arg("clear") == "1") {
    LogBuffer.clear();
  }
  String json;
  LogBuffer.getJson(json);
  request->send(HTTP_STATUS_OK, "application/json", json);
}

static void handlePutLogLevel(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err || !doc["level"].is<const char*>()) {
    request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"Expected {\\\"level\\\":\\\"INFO\\\"}\"}");
    return;
  }
  const char* levelName = doc["level"].as<const char*>();
  if (!LogBuffer.setLevelFromString(levelName)) {
    request->send(HTTP_STATUS_BAD_REQUEST, "application/json", "{\"error\":\"Unknown level\"}");
    return;
  }
  LogBuffer.saveLevel();
  Log_info("Log level changed to %s", LogBuffer.getLevelName());
  String json = "{\"level\":\"";
  json += LogBuffer.getLevelName();
  json += "\"}";
  request->send(HTTP_STATUS_OK, "application/json", json);
}

// ---- Browser UI ------------------------------------------------------------

static void handleRoot(AsyncWebServerRequest* request) {
  Log_info("GET /");
  request->send_P(HTTP_STATUS_OK, "text/html", INDEX_HTML);
}

// ---- OpenAPI ---------------------------------------------------------------

static void handleGetOpenApiJson(AsyncWebServerRequest* request) {
  Log_info("GET /openapi");
  request->send_P(HTTP_STATUS_OK, "application/json", OPENAPI_JSON);
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
  request->send(HTTP_STATUS_OK, "text/html", html);
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
  //
  // -- Non-Obvious WiFiManager Parameter Injection Mechanics --
  //
  // 1. THE DOUBLE-WIDGET / JAVASCRIPT ONCHANGE BIND:
  //    WiFiManager internally renders each parameter as:
  //      "<input id='[id]' name='[id]' value='[default]' ... [customHTML]>"
  //    To render a custom dropdown, we inject custom HTML. By starting customHTML with
  //    " type='hidden'>", we:
  //      - Convert WiFiManager's default <input> to type='hidden' (invisible to the user).
  //      - Close the original <input> tag.
  //    Next, we append our actual custom `<select id='[id]Select' ...>` dropdown WITHOUT
  //    any "name" attribute. Because it has no "name", the browser never submits this dropdown directly,
  //    completely avoiding duplicate POST parameters or name conflicts.
  //    To link them, we add an `onchange` handler to our `<select>` element:
  //      `onchange="document.getElementById('[id]').value=this.value"`
  //    When the user changes the dropdown option, the hidden <input>'s value is immediately updated
  //    in the DOM. On submit, only the single, updated value of the <input> field is sent to the server.
  //
  // 2. THE DELIBERATELY MISSING TRAILING '>' ON THE </select> TAG:
  //    Because WiFiManager appends its own closing '>' at the very end of the rendered tag,
  //    our customHTML must end with "</select" (WITHOUT the closing '>'). If we included
  //    the closing '>', the output would render as "</select>>", causing a literal '>'
  //    to appear as unformatted text right before the "Save" button in the browser GUI.

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
           "<select id='tierSelect' onchange=\"document.getElementById('tier').value=this.value\">%s</select"
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
    const char* sel = (strcmp(LogBuffer.getLevelName(), levelNames[i]) == 0) ? " selected" : "";
    char opt[60];
    snprintf(opt, sizeof(opt), "<option value='%s'%s>%s</option>",
             levelNames[i], sel, levelNames[i]);
    strncat(levelOptions, opt, sizeof(levelOptions) - strlen(levelOptions) - 1);
  }
  char levelCustomHtml[350];
  snprintf(levelCustomHtml, sizeof(levelCustomHtml),
           " type='hidden'><br/>"
           "<label for='logLevel'>Log Level</label>"
           "<select id='logLevelSelect' onchange=\"document.getElementById('logLevel').value=this.value\">%s</select"
           , levelOptions);
  WiFiManagerParameter levelParam("logLevel", "", LogBuffer.getLevelName(), 8, levelCustomHtml);
  wm.addParameter(&levelParam);

  // -- LoRa channel selector --
  String channelOptions = "";
  for (uint8_t i = 0; i <= 31; i++) {
    const char* sel = (LoRaConfig.getChannel() == i) ? " selected" : "";
    char opt[80];
    snprintf(opt, sizeof(opt), "<option value='%d'%s>Ch %d - %.3f MHz</option>",
             i, sel, i, LORA_BASE_FREQUENCY + i);
    channelOptions += opt;
  }
  String channelCustomHtml = " type='hidden'><br/>"
                             "<label for='loraChannel'>LoRa Channel</label>"
                             "<select id='loraChannelSelect' onchange=\"document.getElementById('loraChannel').value=this.value\">";
  channelCustomHtml += channelOptions;
  channelCustomHtml += "</select";

  char channelStr[4];
  snprintf(channelStr, sizeof(channelStr), "%d", LoRaConfig.getChannel());
  WiFiManagerParameter channelParam("loraChannel", "", channelStr, 4, channelCustomHtml.c_str());
  wm.addParameter(&channelParam);

  auto saveCallback = [&tierParam, &levelParam, &channelParam]() {
    const char* chosenTier    = tierParam.getValue();
    const char* chosenLevel   = levelParam.getValue();
    const char* chosenChannel = channelParam.getValue();

    if (chosenTier != nullptr && chosenTier[0] != '\0') {
      if (Gateway.setTierFromString(chosenTier)) {
        Gateway.saveTier();
        Log_info("Gateway tier set from portal to %s", Gateway.getTierName());
      } else {
        Log_warn("Portal returned unknown tier '%s' - ignored", chosenTier);
      }
    }
    if (chosenLevel != nullptr && chosenLevel[0] != '\0') {
      if (LogBuffer.setLevelFromString(chosenLevel)) {
        LogBuffer.saveLevel();
        Log_info("Log level set from portal to %s", LogBuffer.getLevelName());
      } else {
        Log_warn("Portal returned unknown log level '%s' - ignored", chosenLevel);
      }
    }
    if (chosenChannel != nullptr && chosenChannel[0] != '\0') {
      int chan = atoi(chosenChannel);
      if (chan >= 0 && chan <= 31) {
        LoRaConfig.setChannel((uint8_t)chan);
        LoRaConfig.save();
        Log_info("LoRa channel set from portal to %d", LoRaConfig.getChannel());
      } else {
        Log_warn("Portal returned invalid LoRa channel '%s' - ignored", chosenChannel);
      }
    }

    // Auto-configure the LoRa address based on the selected tier (no GUI visible)
    if (Gateway.isConfigured()) {
      if (strcmp(Gateway.getTierName(), "REMOTE") == 0) {
        LoRaConfig.setAddress(LORA_DEFAULT_ADDR_REMOTE);
      } else {
        LoRaConfig.setAddress(LORA_DEFAULT_ADDR_LOCAL);
      }
      LoRaConfig.save();
      Log_info("LoRa address auto-configured to 0x%02X based on tier %s", LoRaConfig.getAddress(), Gateway.getTierName());
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
  server.on("/openapi/ui",  AsyncWebRequestMethod::HTTP_GET, handleGetOpenApiUi);
  server.on("/openapi",     AsyncWebRequestMethod::HTTP_GET, handleGetOpenApiJson);

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
