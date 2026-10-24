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
    key     = prefs.getUShort(LORA_KEY_KEY, LORA_DEFAULT_KEY);

    // The stored value is the AirRate ordinal; an unknown ordinal falls back to the default.
    const uint8_t storedRate = prefs.getUChar(LORA_AIR_RATE_KEY, LoRaTransportClass::AIR_RATE_DEFAULT);
    airRate = (storedRate < LoRaTransportClass::AIR_RATE_COUNT) ? static_cast<LoRaTransportClass::AirRate>(storedRate)
                                                                : LoRaTransportClass::AIR_RATE_DEFAULT;

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
    Log_info("LoRa config loaded: logLevel=%s loraChannel=0x%02X (%d), loraAddress=0x%02X (%d), key=0x%04X (%s), airRate=%s, cpuFreq=%d MHz",
             LogBuffer.getLevelName(), channel, channel, address, address, key, key ? "on" : "off",
             LoRaTransportClass::airRateName(airRate), cpuFreq);
  } else {
    // Namespace doesn't exist yet, use default values
    channel = LORA_DEFAULT_CHANNEL;
    cpuFreq = LORA_CPU_FREQ_DEFAULT;
    key     = LORA_DEFAULT_KEY;
    airRate = LoRaTransportClass::AIR_RATE_DEFAULT;
    if (strcmp(Gateway.getTierName(), "REMOTE") == 0) {
      address = LORA_DEFAULT_ADDR_REMOTE;
    } else {
      address = LORA_DEFAULT_ADDR_LOCAL;
    }
    Log_info("LoRa config using defaults: logLevel=%s loraChannel=0x%02X (%d), loraAddress=0x%02X (%d), key=0x%04X (%s), airRate=%s, cpuFreq=%d MHz",
             LogBuffer.getLevelName(), channel, channel, address, address, key, key ? "on" : "off",
             LoRaTransportClass::airRateName(airRate), cpuFreq);
  }
}

void LoRaConfigClass::save() const {
  Preferences prefs;
  if (prefs.begin(LORA_NVS_NAMESPACE, /*readOnly=*/false)) {
    prefs.putUChar(LORA_CHANNEL_KEY, channel);
    prefs.putUChar(LORA_ADDR_KEY, address);
    prefs.putUShort(LORA_KEY_KEY, key);
    prefs.putUInt(LORA_CPU_FREQ_KEY, cpuFreq);
    prefs.putUChar(LORA_AIR_RATE_KEY, static_cast<uint8_t>(airRate));
    prefs.end();
    Log_info("LoRa config saved to NVS: logLevel=%s loraChannel=0x%02X (%d), loraAddress=0x%02X (%d), key=0x%04X (%s), airRate=%s, cpuFreq=%d MHz",
             LogBuffer.getLevelName(), channel, channel, address, address, key, key ? "on" : "off",
             LoRaTransportClass::airRateName(airRate), cpuFreq);
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

uint16_t LoRaConfigClass::getKey() const {
  return key;
}

void LoRaConfigClass::setKey(uint16_t k) {
  key = k;
}

uint32_t LoRaConfigClass::getCpuFreq() const {
  return cpuFreq;
}

void LoRaConfigClass::setCpuFreq(uint32_t freq) {
  cpuFreq = freq;
}

LoRaTransportClass::AirRate LoRaConfigClass::getAirRate() const {
  return airRate;
}

void LoRaConfigClass::setAirRate(LoRaTransportClass::AirRate rate) {
  airRate = rate;
}
