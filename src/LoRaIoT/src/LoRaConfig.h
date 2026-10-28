#pragma once

#include <Arduino.h>

#include "../../lora/src/LoRaTransport.h"   // AirRate enum

/**
 * LoRaConfig.h - LoRa config persistence (channel, address, encryption key, air rate, CPU frequency).
 *
 * Persisted in NVS (namespace LORA_NVS_NAMESPACE).
 *
 * Defaults:
 *   - channel: LORA_DEFAULT_CHANNEL
 *   - airRate: LoRaTransportClass::AIR_RATE_DEFAULT (62.5k)
 *   - address: LORA_DEFAULT_ADDR_LOCAL or LORA_DEFAULT_ADDR_REMOTE
 *   - key:     LORA_DEFAULT_KEY (0 = E220 encryption disabled)
 */

static constexpr const char* LORA_NVS_NAMESPACE = "sc_prefs";   // shared with LogBuffer and Tier
static constexpr const char* LORA_CHANNEL_KEY   = "loraChannel";
static constexpr const char* LORA_ADDR_KEY      = "loraAddr";
static constexpr const char* LORA_CPU_FREQ_KEY  = "cpuFreq";
static constexpr const char* LORA_KEY_KEY       = "loraKey";
static constexpr const char* LORA_AIR_RATE_KEY  = "loraAirRate";   // Stores the LoRaTransportClass::AirRate ordinal

// Default configuration constants
static constexpr uint8_t LORA_MIN_CHANNEL         = 0;      // Lowest E220 channel (410.125 MHz)
static constexpr uint8_t LORA_MAX_CHANNEL         = 31;     // Highest E220 channel (441.125 MHz)
static constexpr uint8_t LORA_DEFAULT_CHANNEL     = 0x17;   // Ch 23 - 433.125 MHz
static constexpr uint8_t LORA_ADDR_INVALID        = 0x00;   // Never a node address (E220 treats module address 0x0000 as monitor mode)
static constexpr uint8_t LORA_ADDR_BROADCAST      = 0xFF;   // Broadcast address; also the "no address stored in NVS" marker
static constexpr uint8_t LORA_DEFAULT_ADDR_LOCAL  = 0x01;   // Fixed address for LOCAL instance
static constexpr uint8_t LORA_DEFAULT_ADDR_REMOTE = 0x02;   // Default address for REMOTE instance (valid range 0x02-0xFE)
static constexpr uint8_t LORA_MAX_ADDR_REMOTE     = LORA_ADDR_BROADCAST - 1; // Highest REMOTE address (0xFE)
static constexpr float    LORA_BASE_FREQUENCY     = 410.125f; // MHz

// E220 encryption key (CRYPT_H / CRYPT_L registers).
// Suggested format ddmmy: day (01-31) + month (01-12) + year digit (1-9 for 2021-2029).
// Example: 25 Jun 2026 -> 25066. Max ddmmy value: 31129, well within uint16.
// 0 = encryption disabled (CRYPT_H=0x00, CRYPT_L=0x00).
static constexpr uint16_t LORA_DEFAULT_KEY = 0;       // 0 = no encryption
static constexpr uint16_t LORA_MAX_KEY     = 0xFFFF;  // full uint16 range accepted

// Supported CPU frequencies (Minimum with active WiFi PLL is 80 MHz)
static constexpr uint32_t LORA_CPU_FREQ_80MHZ     = 80;     // 80 MHz
static constexpr uint32_t LORA_CPU_FREQ_160MHZ    = 160;    // 160 MHz
static constexpr uint32_t LORA_CPU_FREQ_240MHZ    = 240;    // 240 MHz
static constexpr uint32_t LORA_CPU_FREQ_DEFAULT   = LORA_CPU_FREQ_240MHZ; // Default CPU frequency

/**
 * LoRaConfigClass - Singleton that manages LoRa parameters and their NVS persistence.
 *
 * This class follows the standard Arduino Singleton Pattern:
 * - 'LoRaConfigClass' is the underlying class type. A suffix is used here
 *   because C++ prevents a global variable from having the exact same name
 *   as its class type in the same scope (name shadowing collision).
 * - 'LoRaConfig' is the single globally exported object instance.
 * - Application code should always interact with the 'LoRaConfig' instance.
 */
class LoRaConfigClass {
 public:
  /** Load persisted values from NVS. Call once from applicationSetup() before LoRaTransport.begin(). */
  void begin();

  /** Write all current values back to NVS. Call after any set*() to make the change survive reboots. */
  void save() const;

  /// Active LoRa channel (0-31, maps to 410.125-441.125 MHz in 1 MHz steps).
  uint8_t getChannel() const;
  /// Set the LoRa channel. Does not write NVS; call save() afterwards.
  void setChannel(uint8_t chan);

  /// Active LoRa node address (LOCAL = 0x01, REMOTE = 0x02-0xFE).
  uint8_t getAddress() const;
  /// Set the LoRa node address. Does not write NVS; call save() afterwards.
  void setAddress(uint8_t addr);

  /// Active E220 encryption key (0 = disabled).
  uint16_t getKey() const;
  /// Set the E220 encryption key (CRYPT_H / CRYPT_L). Does not write NVS; call save() afterwards.
  void setKey(uint16_t k);

  /// Active CPU frequency in MHz (80, 160, or 240).
  uint32_t getCpuFreq() const;
  /// Set the CPU frequency. Applied at boot after the next reboot. Does not write NVS; call save() afterwards.
  void setCpuFreq(uint32_t freq);

  /// Active LoRa air data rate.
  LoRaTransportClass::AirRate getAirRate() const;
  /// Set the LoRa air data rate. Does not write NVS; call save() afterwards.
  void setAirRate(LoRaTransportClass::AirRate rate);

 private:
  uint8_t  channel = LORA_DEFAULT_CHANNEL;
  uint8_t  address = LORA_DEFAULT_ADDR_LOCAL;
  uint16_t key     = LORA_DEFAULT_KEY;
  uint32_t cpuFreq = LORA_CPU_FREQ_DEFAULT;
  LoRaTransportClass::AirRate airRate = LoRaTransportClass::AIR_RATE_DEFAULT;
};

// Global singleton instance exported for direct use across the application.
extern LoRaConfigClass LoRaConfig;
