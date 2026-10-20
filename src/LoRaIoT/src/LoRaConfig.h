#pragma once

#include <Arduino.h>

/**
 * LoRaConfig.h - LoRa config persistence (channel and address).
 *
 * Persisted in NVS (namespace LORA_NVS_NAMESPACE, keys LORA_CHANNEL_KEY and LORA_ADDR_KEY).
 *
 * Defaults:
 *   - channel: LORA_DEFAULT_CHANNEL
 *   - address: LORA_DEFAULT_ADDR_LOCAL or LORA_DEFAULT_ADDR_REMOTE
 */

static constexpr const char* LORA_NVS_NAMESPACE = "sc_prefs";   // shared with LogBuffer and Tier
static constexpr const char* LORA_CHANNEL_KEY   = "loraChannel";
static constexpr const char* LORA_ADDR_KEY      = "loraAddr";
static constexpr const char* LORA_CPU_FREQ_KEY   = "cpuFreq";

// Default configuration constants
static constexpr uint8_t LORA_MIN_CHANNEL         = 0;      // Lowest E220 channel (410.125 MHz)
static constexpr uint8_t LORA_MAX_CHANNEL         = 31;     // Highest E220 channel (441.125 MHz)
static constexpr uint8_t LORA_DEFAULT_CHANNEL     = 0x17;   // Ch 23 - 433.125 MHz
static constexpr uint8_t LORA_ADDR_INVALID        = 0x00;   // Never a node address (E220 treats module address 0x0000 as monitor mode)
static constexpr uint8_t LORA_ADDR_BROADCAST      = 0xFF;   // Broadcast address; also the "no address stored in NVS" marker
static constexpr uint8_t LORA_DEFAULT_ADDR_LOCAL  = 0x01;   // Fixed address for LOCAL instance
static constexpr uint8_t LORA_DEFAULT_ADDR_REMOTE = 0x02;   // Default address for REMOTE instance (valid range 0x02-0xFE)
static constexpr uint8_t LORA_MAX_ADDR_REMOTE     = LORA_ADDR_BROADCAST - 1; // Highest REMOTE address (0xFE)
static constexpr float LORA_BASE_FREQUENCY        = 410.125f; // MHz

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
  void begin();
  void save() const;

  uint8_t getChannel() const;
  void setChannel(uint8_t chan);

  uint8_t getAddress() const;
  void setAddress(uint8_t addr);

  uint32_t getCpuFreq() const;
  void setCpuFreq(uint32_t freq);

 private:
  uint8_t channel = LORA_DEFAULT_CHANNEL;
  uint8_t address = LORA_DEFAULT_ADDR_LOCAL;
  uint32_t cpuFreq = LORA_CPU_FREQ_DEFAULT;
};

// Global singleton instance exported for direct use across the application.
extern LoRaConfigClass LoRaConfig;
