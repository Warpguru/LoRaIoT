#include "Server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
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

// ---- Captive portal form sizes ----------------------------------------------

static constexpr size_t PORTAL_MAX_SELECT_ENTRIES   = 8;    // Max tiers / log levels listed in a portal dropdown
static constexpr size_t PORTAL_OPTION_SIZE          = 80;   // One <option> element of the tier / level dropdown
static constexpr size_t PORTAL_CHANNEL_OPTION_SIZE  = 80;   // One <option> element of the channel dropdown
static constexpr size_t PORTAL_TIER_OPTIONS_SIZE    = 512;  // All tier <option> elements
static constexpr size_t PORTAL_LEVEL_OPTIONS_SIZE   = 384;  // All log level <option> elements
static constexpr size_t PORTAL_TIER_HTML_SIZE       = 1024; // Custom HTML of the tier parameter
static constexpr size_t PORTAL_LEVEL_HTML_SIZE      = 512;  // Custom HTML of the log level parameter
static constexpr size_t PORTAL_NAME_VALUE_LEN       = 32;   // Max value length of the tier / log level parameter
static constexpr size_t PORTAL_CHANNEL_VALUE_LEN    = 4;    // Max value length of the channel parameter (incl. terminator)
static constexpr size_t PORTAL_AIR_RATE_VALUE_LEN   = 8;    // Max value length of the air rate parameter ("62.5k" + terminator)
static constexpr size_t PORTAL_KEY_VALUE_LEN        = 6;    // Max value length of the loraKey parameter (0-65535 + terminator)
static constexpr size_t PORTAL_KEY_HTML_SIZE        = 300;  // Custom HTML of the loraKey parameter

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

// Erase every key in one application NVS namespace (tier, log level, LoRa
// channel/address/CPU frequency). Each module names its own namespace constant,
// so all of them are cleared even if they stop sharing one namespace later.
static void clearNvsNamespace(const char* nvsNamespace) {
  Preferences prefs;
  if (prefs.begin(nvsNamespace, /*readOnly=*/false)) {
    prefs.clear();
    prefs.end();
    Log_info("NVS namespace '%s' cleared", nvsNamespace);
  } else {
    Log_error("NVS namespace '%s' could not be opened for clearing", nvsNamespace);
  }
}

static void handleReboot(AsyncWebServerRequest* request) {
  Log_warn("GET /reboot - rebooting");
  request->send(HTTP_STATUS_OK, "text/plain", "ESP32 rebooting...");
  delay(RESET_REBOOT_DELAY_MS);
  ESP.restart();
}

static void handleReset(AsyncWebServerRequest* request) {
  Log_warn("GET /reset - CLEARING SETTINGS");
  request->send(HTTP_STATUS_OK, "text/plain",
                "WiFi and application settings cleared. ESP32 rebooting to Configuration Mode...");
  delay(RESET_REBOOT_DELAY_MS);
  wm.resetSettings();
  clearNvsNamespace(TIER_NVS_NAMESPACE);
  clearNvsNamespace(LOG_NVS_NAMESPACE);
  clearNvsNamespace(LORA_NVS_NAMESPACE);
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
 * Expands OPENAPI_UI_TEMPLATE (defined in openapi_json.h) into a stack
 * buffer on every request. No heap allocations; no String + operator.
 * The browser fetches Swagger UI JS/CSS from unpkg.com - the ESP32 itself
 * does not need internet access.
 */
static void handleGetOpenApiUi(AsyncWebServerRequest* request) {
  Log_info("GET /openapi/ui");
  String ssid = WiFi.SSID();   // extracted to named local so .c_str() is safe
  char buf[1300];  // the size actually needed is logged below after the snprintf
  const int written = snprintf(buf, sizeof(buf), OPENAPI_UI_TEMPLATE,
           ssid.c_str(),           // %s SSID in banner
           Gateway.getTierName(),  // %s tier in banner
           (unsigned)LoRaConfig.getKey(),               // %u key (decimal) in banner
           LoRaConfig.getKey() ? "on" : "off",          // %s key state in banner
           LoRaTransportClass::airRateName(LoRaConfig.getAirRate()),   // %s air data rate in banner
           __DATE__ " " __TIME__,  // %s build timestamp in banner
           Gateway.isLocal() ? "LOCAL" : "REMOTE"); // %s TIER JS variable for x-tier filter
  // snprintf returns the length it wanted to write, so written >= buffer size means truncation.
  Log_info("GET /openapi/ui: banner needs %d of %u buffer bytes%s",
           written, (unsigned)sizeof(buf), (written >= (int)sizeof(buf)) ? " (TRUNCATED)" : "");
  request->send(HTTP_STATUS_OK, "text/html", buf);
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

  // -- Tier selector (LOCAL / REMOTE nodes) --
  size_t tierCount;
  const Tier* const* tiers = Tier::values(tierCount);
  const char* tierNames[PORTAL_MAX_SELECT_ENTRIES];
  for (size_t i = 0; i < tierCount && i < PORTAL_MAX_SELECT_ENTRIES; i++) {
    tierNames[i] = tiers[i]->name();
  }
  // Build the <select> options; the wrapping <select name='tier'> is supplied
  // by WiFiManager when it renders the customHTML around the registered id.
  // We use a full <select> override via customHTML so the dropdown appears
  // instead of the default text input.
  char tierOptions[PORTAL_TIER_OPTIONS_SIZE];
  tierOptions[0] = '\0';
  for (size_t i = 0; i < tierCount && i < PORTAL_MAX_SELECT_ENTRIES; i++) {
    const char* sel = (strcmp(Gateway.getTierName(), tierNames[i]) == 0) ? " selected" : "";
    char opt[PORTAL_OPTION_SIZE];
    snprintf(opt, sizeof(opt), "<option value='%s'%s>%s</option>",
             tierNames[i], sel, tierNames[i]);
    strncat(tierOptions, opt, sizeof(tierOptions) - strlen(tierOptions) - 1);
  }
  char tierCustomHtml[PORTAL_TIER_HTML_SIZE];
  snprintf(tierCustomHtml, sizeof(tierCustomHtml),
           " type='hidden'><br/>"
           "<label for='tier'>Gateway Tier</label>"
           "<select id='tierSelect' onchange=\"document.getElementById('tier').value=this.value\">%s</select"
           , tierOptions);
  WiFiManagerParameter tierParam("tier", "", Gateway.getTierName(), PORTAL_NAME_VALUE_LEN, tierCustomHtml);
  wm.addParameter(&tierParam);

  // -- Log-level selector --
  size_t levelCount;
  const LogLevel* const* levels = LogLevel::values(levelCount);
  const char* levelNames[PORTAL_MAX_SELECT_ENTRIES];
  for (size_t i = 0; i < levelCount && i < PORTAL_MAX_SELECT_ENTRIES; i++) levelNames[i] = levels[i]->name();
  char levelOptions[PORTAL_LEVEL_OPTIONS_SIZE];
  levelOptions[0] = '\0';
  for (size_t i = 0; i < levelCount && i < PORTAL_MAX_SELECT_ENTRIES; i++) {
    const char* sel = (strcmp(LogBuffer.getLevelName(), levelNames[i]) == 0) ? " selected" : "";
    char opt[PORTAL_OPTION_SIZE];
    snprintf(opt, sizeof(opt), "<option value='%s'%s>%s</option>",
             levelNames[i], sel, levelNames[i]);
    strncat(levelOptions, opt, sizeof(levelOptions) - strlen(levelOptions) - 1);
  }
  char levelCustomHtml[PORTAL_LEVEL_HTML_SIZE];
  snprintf(levelCustomHtml, sizeof(levelCustomHtml),
           " type='hidden'><br/>"
           "<label for='logLevel'>Log Level</label>"
           "<select id='logLevelSelect' onchange=\"document.getElementById('logLevel').value=this.value\">%s</select"
           , levelOptions);
  WiFiManagerParameter levelParam("logLevel", "", LogBuffer.getLevelName(), PORTAL_NAME_VALUE_LEN, levelCustomHtml);
  wm.addParameter(&levelParam);

  // -- LoRa channel selector --
  String channelOptions = "";
  for (uint8_t i = LORA_MIN_CHANNEL; i <= LORA_MAX_CHANNEL; i++) {
    const char* sel = (LoRaConfig.getChannel() == i) ? " selected" : "";
    char opt[PORTAL_CHANNEL_OPTION_SIZE];
    snprintf(opt, sizeof(opt), "<option value='%d'%s>Ch %d - %.3f MHz</option>",
             i, sel, i, LORA_BASE_FREQUENCY + i);
    channelOptions += opt;
  }
  String channelCustomHtml = " type='hidden'><br/>"
                             "<label for='loraChannel'>LoRa Channel</label>"
                             "<select id='loraChannelSelect' onchange=\"document.getElementById('loraChannel').value=this.value\">";
  channelCustomHtml += channelOptions;
  channelCustomHtml += "</select";

  char channelStr[PORTAL_CHANNEL_VALUE_LEN];
  snprintf(channelStr, sizeof(channelStr), "%d", LoRaConfig.getChannel());
  WiFiManagerParameter channelParam("loraChannel", "", channelStr, PORTAL_CHANNEL_VALUE_LEN, channelCustomHtml.c_str());
  wm.addParameter(&channelParam);

  // -- LoRa air data rate selector (value = label from LoRaTransportClass::airRateName, e.g. "62.5k") --
  String airRateOptions = "";
  for (uint8_t i = 0; i < LoRaTransportClass::AIR_RATE_COUNT; i++) {
    const char* rateName = LoRaTransportClass::airRateName(static_cast<LoRaTransportClass::AirRate>(i));
    const char* sel = (LoRaConfig.getAirRate() == i) ? " selected" : "";
    char opt[PORTAL_CHANNEL_OPTION_SIZE];
    snprintf(opt, sizeof(opt), "<option value='%s'%s>%s bps%s</option>", rateName, sel, rateName,
             (i == LoRaTransportClass::AIR_RATE_DEFAULT) ? " (default)" : "");
    airRateOptions += opt;
  }
  String airRateCustomHtml = " type='hidden'><br/>"
                             "<label for='loraAirRate'>LoRa Air Data Rate (both nodes must match; lower = longer range)</label>"
                             "<select id='loraAirRateSelect' onchange=\"document.getElementById('loraAirRate').value=this.value\">";
  airRateCustomHtml += airRateOptions;
  airRateCustomHtml += "</select";
  WiFiManagerParameter airRateParam("loraAirRate", "", LoRaTransportClass::airRateName(LoRaConfig.getAirRate()),
                                    PORTAL_AIR_RATE_VALUE_LEN, airRateCustomHtml.c_str());
  wm.addParameter(&airRateParam);

   // -- LoRa encryption key (ddmmy format; 0 = disabled) --
  // Pre-fill with the persisted key so re-provisioning shows the current value.
  // The field is type='number' - the browser enforces 0-65535 numerically.
  char keyStr[PORTAL_KEY_VALUE_LEN];
  snprintf(keyStr, sizeof(keyStr), "%u", (unsigned)LoRaConfig.getKey());
  char keyCustomHtml[PORTAL_KEY_HTML_SIZE];
  snprintf(keyCustomHtml, sizeof(keyCustomHtml),
           " type='hidden'><br/>"
           "<label for='loraKey'>LoRa Encryption Key (ddmmy, e.g. 25066 = 25 Jun 2026; 0 = disabled)</label>"
           "<input id='loraKeyInput' type='number' min='0' max='65535' value='%s'"
           " onchange=\"document.getElementById('loraKey').value=this.value\">",
           keyStr);
  // Strip trailing '>' - WiFiManager appends its own closing '>'
  keyCustomHtml[strlen(keyCustomHtml) - 1] = '\0';
  // Append closing without '>'
  strncat(keyCustomHtml, "</input", sizeof(keyCustomHtml) - strlen(keyCustomHtml) - 1);
  WiFiManagerParameter keyParam("loraKey", "", keyStr, PORTAL_KEY_VALUE_LEN, keyCustomHtml);
  wm.addParameter(&keyParam);

  auto saveCallback = [&tierParam, &levelParam, &channelParam, &airRateParam, &keyParam]() {
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
      if (chan >= LORA_MIN_CHANNEL && chan <= LORA_MAX_CHANNEL) {
        LoRaConfig.setChannel((uint8_t)chan);
        LoRaConfig.save();
        Log_info("LoRa channel set from portal to %d", LoRaConfig.getChannel());
      } else {
        Log_warn("Portal returned invalid LoRa channel '%s' - ignored", chosenChannel);
      }
    }

    // LoRa air data rate - the transport is not started yet while the portal runs,
    // so LoRaTransport.begin() picks the saved rate up later.
    const char* chosenAirRate = airRateParam.getValue();
    if (chosenAirRate != nullptr && chosenAirRate[0] != '\0') {
      LoRaTransportClass::AirRate rate;
      if (LoRaTransportClass::airRateFromName(chosenAirRate, rate)) {
        LoRaConfig.setAirRate(rate);
        LoRaConfig.save();
        Log_info("LoRa air rate set from portal to %s", LoRaTransportClass::airRateName(LoRaConfig.getAirRate()));
      } else {
        Log_warn("Portal returned unknown LoRa air rate '%s' - ignored", chosenAirRate);
      }
    }

    // LoRa encryption key - portal value overrides persisted key.
    // Key=0 is valid and means "disabled"; blank field is treated as 0.
    const char* chosenKey = keyParam.getValue();
    if (chosenKey != nullptr) {
      int k = (chosenKey[0] != '\0') ? atoi(chosenKey) : 0;
      if (k >= 0 && k <= (int)LORA_MAX_KEY) {
        LoRaConfig.setKey((uint16_t)k);
        Log_info("LoRa key set from portal (%s); takes effect on next reboot",
                 k ? "encryption enabled" : "encryption disabled");
      } else {
        Log_warn("Portal returned invalid LoRa key '%s' - ignored", chosenKey);
      }
    }

    // LoRa address is derived automatically from the selected tier (LOCAL=0x01, REMOTE 1=0x02, etc.).
    if (Gateway.isConfigured()) {
      LoRaConfig.setAddress(Gateway.defaultAddress());
      LoRaConfig.save();
      Log_info("LoRa address set to 0x%02X (%s)", Gateway.defaultAddress(), Gateway.getTierName());
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
  server.on("/status",  AsyncWebRequestMethod::HTTP_GET, handleGetStatus);
  server.on("/reboot",  AsyncWebRequestMethod::HTTP_GET, handleReboot);
  server.on("/reset",   AsyncWebRequestMethod::HTTP_GET, handleReset);

  server.begin();
  Log_info("AsyncWebServer started on port %d (HTTP + WS on %s).", SERVER_PORT, WS_PATH);
}

void handleServerRequests() {
  // intentionally empty - ESPAsyncWebServer is fully non-blocking
}
