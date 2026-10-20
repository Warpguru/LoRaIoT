#include "LoRaConfig.h"

#include <Arduino.h>
#include <Preferences.h>

#include "../../util/src/LogBuffer.h"
#include "Tier.h"

LoRaConfigClass LoRaConfig;

void LoRaConfigClass::begin() {
  Preferences prefs;

  if (prefs.begin(LORA_NVS_NAMESPACE, /*readOnly=*/true)) {
    // getUChar automatically returns LORA_DEFAULT_CHANNEL if the key does not exist
    channel = prefs.getUChar(LORA_CHANNEL_KEY, LORA_DEFAULT_CHANNEL);
    cpuFreq = prefs.getUInt(LORA_CPU_FREQ_KEY, LORA_CPU_FREQ_DEFAULT);
    
    // LORA_ADDR_BROADCAST doubles as the "not stored" marker (never a node address).
    // LORA_ADDR_INVALID was the LOCAL address in an earlier firmware and would make the
    // E220 monitor all traffic, so a stored value of it is discarded and the tier default is used.
    uint8_t addr = prefs.getUChar(LORA_ADDR_KEY, LORA_ADDR_BROADCAST);
    if (addr != LORA_ADDR_BROADCAST && addr != LORA_ADDR_INVALID) {
      address = addr;
    } else {
      // Default address based on active tier
      if (strcmp(Gateway.getTierName(), "REMOTE") == 0) {
        address = LORA_DEFAULT_ADDR_REMOTE;
      } else {
        address = LORA_DEFAULT_ADDR_LOCAL;
      }
    }
    prefs.end();
    Log_info("LoRa config loaded: channel=0x%02X (%d), address=0x%02X (%d), cpuFreq=%d MHz",
             channel, channel, address, address, cpuFreq);
  } else {
    // Namespace doesn't exist yet, use default values
    channel = LORA_DEFAULT_CHANNEL;
    cpuFreq = LORA_CPU_FREQ_DEFAULT;
    if (strcmp(Gateway.getTierName(), "REMOTE") == 0) {
      address = LORA_DEFAULT_ADDR_REMOTE;
    } else {
      address = LORA_DEFAULT_ADDR_LOCAL;
    }
    Log_info("LoRa config using defaults: channel=0x%02X (%d), address=0x%02X (%d), cpuFreq=%d MHz",
             channel, channel, address, address, cpuFreq);
  }
}

void LoRaConfigClass::save() const {
  Preferences prefs;
  if (prefs.begin(LORA_NVS_NAMESPACE, /*readOnly=*/false)) {
    prefs.putUChar(LORA_CHANNEL_KEY, channel);
    prefs.putUChar(LORA_ADDR_KEY, address);
    prefs.putUInt(LORA_CPU_FREQ_KEY, cpuFreq);
    prefs.end();
    Log_info("LoRa config saved to NVS: channel=0x%02X (%d), address=0x%02X (%d), cpuFreq=%d MHz",
             channel, channel, address, address, cpuFreq);
  } else {
    Log_error("LoRa config save failed - could not open NVS namespace '%s'", LORA_NVS_NAMESPACE);
  }
}

uint8_t LoRaConfigClass::getChannel() const {
  return channel;
}

void LoRaConfigClass::setChannel(uint8_t chan) {
  channel = chan;
}

uint8_t LoRaConfigClass::getAddress() const {
  return address;
}

void LoRaConfigClass::setAddress(uint8_t addr) {
  address = addr;
}

uint32_t LoRaConfigClass::getCpuFreq() const {
  return cpuFreq;
}

void LoRaConfigClass::setCpuFreq(uint32_t freq) {
  cpuFreq = freq;
}
