#include "WiFiConfig.h"

#include <Arduino.h>
#include <Preferences.h>

#include "../../util/src/LogBuffer.h"
#include "../../LoRaIoT/src/Tier.h"
#include "../../LoRaIoT/src/LoRaConfig.h"

/**
 * WiFiConfig.cpp - Implementation of WiFi mode and SoftAP configuration service.
 *
 * Rationale & Architectural Design:
 * - NVS Persistence: Uses the shared 'sc_prefs' NVS namespace. Keys are strictly <= 15 chars
 *   to comply with ESP-IDF NVS limits (e.g. 'remoteWifiMode', 'remoteStaEnable').
 * - Mode Encoding: Persists raw mode ordinal (0 = STA, 1 = AP, 2 = AP_STA).
 * - Default SSID Derivation: Automatically generates 'LoRaIoT-Node-{n}' based on the LoRa node address
 *   (e.g. address 0x02 -> Node 1, address 0x03 -> Node 2).
 * - WPA2 Constraints: Validates passwords to be either empty (Open WiFi) or 8-64 characters (WPA2-PSK).
 *   Passwords of 1-7 chars are rejected to prevent silent softAP startup failures.
 * - Subnet Derivation: Automatically pairs any custom AP IP with a standard /24 subnet mask (255.255.255.0)
 *   so callers only need to provide the host IP.
 */

WiFiConfigClass WiFiConfig;

void WiFiConfigClass::buildDefaultSsid() {
  // Derive node number from LoRa address (LOCAL = 0x01, REMOTE 1 = 0x02, REMOTE 2 = 0x03, etc.)
  uint8_t addr = LoRaConfig.getAddress();
  uint8_t nodeNum = (addr >= LORA_DEFAULT_ADDR_REMOTE) ? (addr - LORA_DEFAULT_ADDR_REMOTE + 1) : 1;
  snprintf(defaultSsid, sizeof(defaultSsid), "LoRaIoT-Node-%u", nodeNum);
}

void WiFiConfigClass::begin() {
  buildDefaultSsid();

  Preferences prefs;
  if (prefs.begin(WIFI_NVS_NAMESPACE, /*readOnly=*/true)) {
    uint8_t rawMode = prefs.getUChar(WIFI_MODE_KEY, 0);

    // Load persisted AP SSID or fall back to derived default
    String storedSsid = prefs.getString(WIFI_AP_SSID_KEY, "");
    if (storedSsid.length() > 0 && storedSsid.length() <= WIFI_MAX_SSID_LEN) {
      strncpy(apSsid, storedSsid.c_str(), sizeof(apSsid) - 1);
      apSsid[sizeof(apSsid) - 1] = '\0';
    } else {
      strncpy(apSsid, defaultSsid, sizeof(apSsid) - 1);
      apSsid[sizeof(apSsid) - 1] = '\0';
    }

    // Load persisted AP password or fall back to default
    String storedPass = prefs.getString(WIFI_AP_PASS_KEY, WIFI_DEFAULT_AP_PASS);
    if (isValidPassword(storedPass.c_str())) {
      strncpy(apPass, storedPass.c_str(), sizeof(apPass) - 1);
      apPass[sizeof(apPass) - 1] = '\0';
    } else {
      Log_warn("WiFi config: invalid AP password in NVS, falling back to default '%s'", WIFI_DEFAULT_AP_PASS);
      strncpy(apPass, WIFI_DEFAULT_AP_PASS, sizeof(apPass) - 1);
      apPass[sizeof(apPass) - 1] = '\0';
    }

    // Load persisted AP IP or fall back to library default 192.168.4.1
    String storedIp = prefs.getString(WIFI_AP_IP_KEY, WIFI_DEFAULT_AP_IP_STR);
    IPAddress parsedIp;
    if (parsedIp.fromString(storedIp) && parsedIp != IPAddress(0, 0, 0, 0)) {
      apIp = parsedIp;
    } else {
      Log_warn("WiFi config: invalid AP IP in NVS ('%s'), falling back to default '%s'",
               storedIp.c_str(), WIFI_DEFAULT_AP_IP_STR);
      apIp.fromString(WIFI_DEFAULT_AP_IP_STR);
    }
    apSubnet = IPAddress(255, 255, 255, 0);

    // Load persisted AP channel (1-13) or fall back to channel 1
    uint8_t storedChan = prefs.getUChar(WIFI_AP_CHAN_KEY, WIFI_DEFAULT_AP_CHANNEL);
    if (isValidChannel(storedChan)) {
      apChannel = storedChan;
    } else {
      Log_warn("WiFi config: invalid AP channel in NVS (%u), falling back to channel %u",
               storedChan, WIFI_DEFAULT_AP_CHANNEL);
      apChannel = WIFI_DEFAULT_AP_CHANNEL;
    }

    // Validate operational mode:
    // rawMode 1 = AP, rawMode 2 = AP_STA, rawMode 0 = STA.
    // AP/AP_STA modes require valid AP parameters; invalid values fallback to STA mode.
    if (rawMode == static_cast<uint8_t>(WifiOpMode::AP) || rawMode == static_cast<uint8_t>(WifiOpMode::AP_STA)) {
      if (apSsid[0] != '\0' && isValidPassword(apPass) && isValidChannel(apChannel)) {
        opMode = static_cast<WifiOpMode>(rawMode);
      } else {
        Log_warn("WiFi config: AP parameters validation failed, defaulting to STA mode");
        opMode = WifiOpMode::STA;
      }
    } else {
      opMode = WifiOpMode::STA;
    }

    prefs.end();
    Log_info("WiFi config loaded: mode=%s apSsid='%s' apIp=%s apChan=%u",
             getOpModeName(), apSsid, apIp.toString().c_str(), apChannel);
  } else {
    // Fresh node: defaults to STA mode
    opMode = WifiOpMode::STA;
    strncpy(apSsid, defaultSsid, sizeof(apSsid) - 1);
    apSsid[sizeof(apSsid) - 1] = '\0';
    strncpy(apPass, WIFI_DEFAULT_AP_PASS, sizeof(apPass) - 1);
    apPass[sizeof(apPass) - 1] = '\0';
    apIp.fromString(WIFI_DEFAULT_AP_IP_STR);
    apSubnet = IPAddress(255, 255, 255, 0);
    apChannel = WIFI_DEFAULT_AP_CHANNEL;

    Log_info("WiFi config using defaults: mode=%s apSsid='%s' apIp=%s apChan=%u",
             getOpModeName(), apSsid, apIp.toString().c_str(), apChannel);
  }
}

void WiFiConfigClass::save() const {
  Preferences prefs;
  if (prefs.begin(WIFI_NVS_NAMESPACE, /*readOnly=*/false)) {
    prefs.putUChar(WIFI_MODE_KEY, static_cast<uint8_t>(opMode));
    prefs.putString(WIFI_AP_SSID_KEY, apSsid);
    prefs.putString(WIFI_AP_PASS_KEY, apPass);
    prefs.putString(WIFI_AP_IP_KEY, apIp.toString());
    prefs.putUChar(WIFI_AP_CHAN_KEY, apChannel);
    prefs.end();

    Log_info("WiFi config saved to NVS: mode=%s apSsid='%s' apIp=%s apChan=%u",
             getOpModeName(), apSsid, apIp.toString().c_str(), apChannel);
  } else {
    Log_error("WiFi config save failed - could not open NVS namespace '%s'", WIFI_NVS_NAMESPACE);
  }
}

WifiOpMode WiFiConfigClass::getOpMode() const {
  return opMode;
}

bool WiFiConfigClass::isApEnabled() const {
  return (opMode == WifiOpMode::AP || opMode == WifiOpMode::AP_STA);
}

bool WiFiConfigClass::isStaEnabled() const {
  return (opMode == WifiOpMode::STA || opMode == WifiOpMode::AP_STA);
}

const char* WiFiConfigClass::getOpModeName() const {
  switch (opMode) {
    case WifiOpMode::AP:
      return "AP";
    case WifiOpMode::AP_STA:
      return "AP_STA";
    case WifiOpMode::STA:
    default:
      return "STA";
  }
}

bool WiFiConfigClass::parseOpMode(const char* name, WifiOpMode& outMode) {
  if (name == nullptr) {
    return false;
  }
  if (strcasecmp(name, "STA") == 0) {
    outMode = WifiOpMode::STA;
    return true;
  }
  if (strcasecmp(name, "AP") == 0) {
    outMode = WifiOpMode::AP;
    return true;
  }
  if (strcasecmp(name, "AP_STA") == 0 || strcasecmp(name, "AP+STA") == 0) {
    outMode = WifiOpMode::AP_STA;
    return true;
  }
  return false;
}

void WiFiConfigClass::setOpMode(WifiOpMode mode) {
  opMode = mode;
}

const char* WiFiConfigClass::getApSsid() const {
  if (apSsid[0] != '\0') {
    return apSsid;
  }
  return defaultSsid;
}

void WiFiConfigClass::setApSsid(const char* newSsid) {
  if (newSsid != nullptr && newSsid[0] != '\0') {
    strncpy(apSsid, newSsid, sizeof(apSsid) - 1);
    apSsid[sizeof(apSsid) - 1] = '\0';
  } else {
    strncpy(apSsid, defaultSsid, sizeof(apSsid) - 1);
    apSsid[sizeof(apSsid) - 1] = '\0';
  }
}

const char* WiFiConfigClass::getApPassword() const {
  return apPass;
}

bool WiFiConfigClass::setApPassword(const char* newPass) {
  if (newPass == nullptr || newPass[0] == '\0') {
    apPass[0] = '\0';
    return true;
  }
  if (!isValidPassword(newPass)) {
    return false;
  }
  strncpy(apPass, newPass, sizeof(apPass) - 1);
  apPass[sizeof(apPass) - 1] = '\0';
  return true;
}

IPAddress WiFiConfigClass::getApIp() const {
  return apIp;
}

IPAddress WiFiConfigClass::getApSubnet() const {
  return apSubnet;
}

void WiFiConfigClass::setApIp(const IPAddress& ip) {
  apIp = ip;
  apSubnet = IPAddress(255, 255, 255, 0);
}

bool WiFiConfigClass::setApIpFromString(const char* ipStr) {
  if (ipStr == nullptr) {
    return false;
  }
  IPAddress parsed;
  if (!parsed.fromString(ipStr)) {
    return false;
  }
  apIp = parsed;
  apSubnet = IPAddress(255, 255, 255, 0);
  return true;
}

uint8_t WiFiConfigClass::getApChannel() const {
  return apChannel;
}

bool WiFiConfigClass::setApChannel(uint8_t channel) {
  if (!isValidChannel(channel)) {
    return false;
  }
  apChannel = channel;
  return true;
}

bool WiFiConfigClass::isValidPassword(const char* pass) {
  if (pass == nullptr || pass[0] == '\0') {
    return true;  // open network
  }
  size_t len = strlen(pass);
  return (len >= WIFI_MIN_PASS_LEN && len <= WIFI_MAX_PASS_LEN);
}

bool WiFiConfigClass::isValidChannel(uint8_t channel) {
  return (channel >= WIFI_MIN_AP_CHANNEL && channel <= WIFI_MAX_AP_CHANNEL);
}
