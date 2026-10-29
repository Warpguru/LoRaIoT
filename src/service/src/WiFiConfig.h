#pragma once

#include <Arduino.h>
#include <IPAddress.h>

/**
 * WiFiConfig.h - WiFi operational mode and SoftAP configuration persistence.
 *
 * Persisted in NVS (namespace WIFI_NVS_NAMESPACE = "sc_prefs").
 *
 * Operating modes:
 *   - STA:    Station client only (connects to external router).
 *   - AP:     Access Point only (broadcasts SoftAP, no router connection).
 *   - AP_STA: Concurrent Access Point and Station (broadcasts SoftAP and connects to router).
 *
 * Defaults:
 *   - mode:      WifiOpMode::STA
 *   - enableSta: false (when mode is AP)
 *   - ssid:      "LoRaIoT-Node-{n}" (derived from Gateway tier/address, e.g. "LoRaIoT-Node-1")
 *   - password:  "LoRaIoT"
 *   - ip:        192.168.4.1
 *   - subnet:    255.255.255.0 (/24)
 *   - channel:   1 (recommended non-overlapping 2.4 GHz channel)
 */

// NVS namespace and key constants (all keys <= 15 chars for ESP-IDF NVS limits)
static constexpr const char* WIFI_NVS_NAMESPACE = "sc_prefs";
static constexpr const char* WIFI_MODE_KEY       = "remoteWifiMode";   // 14 chars
static constexpr const char* WIFI_AP_SSID_KEY    = "remoteApSsid";     // 12 chars
static constexpr const char* WIFI_AP_PASS_KEY    = "remoteApPass";     // 12 chars
static constexpr const char* WIFI_AP_IP_KEY      = "remoteApIp";       // 10 chars
static constexpr const char* WIFI_AP_CHAN_KEY    = "remoteApChannel";  // 15 chars

// Configuration constraints and buffer sizes
static constexpr size_t   WIFI_MAX_SSID_LEN        = 32;
static constexpr size_t   WIFI_SSID_BUF_SIZE       = WIFI_MAX_SSID_LEN + 1;
static constexpr size_t   WIFI_MIN_PASS_LEN        = 8;
static constexpr size_t   WIFI_MAX_PASS_LEN        = 64;
static constexpr size_t   WIFI_PASS_BUF_SIZE       = WIFI_MAX_PASS_LEN + 1;
static constexpr uint8_t  WIFI_MIN_AP_CHANNEL      = 1;
static constexpr uint8_t  WIFI_MAX_AP_CHANNEL      = 13;
static constexpr uint8_t  WIFI_DEFAULT_AP_CHANNEL  = 1;
static constexpr const char* WIFI_DEFAULT_AP_PASS  = "LoRaIoT";
static constexpr const char* WIFI_DEFAULT_AP_IP_STR = "192.168.4.1";

/** Operational WiFi mode. */
enum class WifiOpMode : uint8_t {
  STA    = 0,  ///< Station mode (client connected to existing router)
  AP     = 1,  ///< SoftAP mode only (standalone access point)
  AP_STA = 2   ///< Concurrent SoftAP and Station mode
};

class WiFiConfigClass {
 public:
  /** Load persisted values from NVS. Call once from applicationSetup() before setupServer(). */
  void begin();

  /** Write all current values back to NVS. */
  void save() const;

  /// Active operational WiFi mode (STA, AP, or AP_STA).
  WifiOpMode getOpMode() const;

  /// Returns true if AP beacon is active (in either AP or AP_STA mode).
  bool isApEnabled() const;

  /// Returns true if STA client connection is active (in either STA or AP_STA mode).
  bool isStaEnabled() const;

  /// Human-readable mode name ("STA", "AP", "AP_STA").
  const char* getOpModeName() const;

  /// Parse mode name from string ("STA" -> STA, "AP" -> AP, "AP_STA" -> AP_STA).
  static bool parseOpMode(const char* name, WifiOpMode& outMode);

  /// Set the operational mode directly.
  void setOpMode(WifiOpMode mode);

  /// SoftAP SSID (returns custom SSID or derived default e.g. "LoRaIoT-Node-1").
  const char* getApSsid() const;

  /// Set SoftAP SSID (1-32 chars). Empty or null resets to default.
  void setApSsid(const char* newSsid);

  /// SoftAP WPA2 password (empty string = open network; >= 8 chars = WPA2).
  const char* getApPassword() const;

  /// Set SoftAP password. Returns false if 1-7 chars (invalid WPA2).
  bool setApPassword(const char* newPass);

  /// SoftAP IP address (default 192.168.4.1).
  IPAddress getApIp() const;

  /// SoftAP Subnet mask (default 255.255.255.0).
  IPAddress getApSubnet() const;

  /// Set SoftAP IP address from IPAddress object.
  void setApIp(const IPAddress& ip);

  /// Set SoftAP IP address from string (e.g. "192.168.4.1"). Returns false if invalid IP string.
  bool setApIpFromString(const char* ipStr);

  /// SoftAP 2.4 GHz WiFi channel (1-13).
  uint8_t getApChannel() const;

  /// Set SoftAP channel (1-13). Returns false if out of range.
  bool setApChannel(uint8_t channel);

  /// Validate WPA2 password length (0 for open network, or 8..64 chars).
  static bool isValidPassword(const char* pass);

  /// Validate 2.4 GHz WiFi channel range (1..13).
  static bool isValidChannel(uint8_t channel);

 private:
  void buildDefaultSsid();

  WifiOpMode opMode                            = WifiOpMode::STA;
  char       apSsid[WIFI_SSID_BUF_SIZE]        = "";
  char       apPass[WIFI_PASS_BUF_SIZE]        = "";
  char       defaultSsid[WIFI_SSID_BUF_SIZE]   = "";
  IPAddress  apIp                              = IPAddress(192, 168, 4, 1);
  IPAddress  apSubnet                          = IPAddress(255, 255, 255, 0);
  uint8_t    apChannel                         = WIFI_DEFAULT_AP_CHANNEL;
};

extern WiFiConfigClass WiFiConfig;
