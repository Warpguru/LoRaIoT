#include "Tier.h"

#include <Arduino.h>
#include <Preferences.h>

#include "../../util/src/LogBuffer.h"

/**
 * Tier.cpp - Gateway tier (LOCAL / REMOTE) definition and NVS persistence.
 *
 * Pattern mirrors LogBuffer.cpp / LogLevel:
 *   - Tier static constants are defined here with their string names.
 *   - GatewayClass::begin() loads the persisted value from NVS.
 *   - GatewayClass::saveTier() writes it back after a portal or API change.
 */

// ---- Tier static constants -------------------------------------------------

const Tier Tier::LOCAL   ("LOCAL",            0x01);
const Tier Tier::REMOTE_1("REMOTE (Node 1)", 0x02);
const Tier Tier::REMOTE_2("REMOTE (Node 2)", 0x03);
const Tier Tier::REMOTE_3("REMOTE (Node 3)", 0x04);

// values() - static array, initialised once, no heap allocation.
const Tier* const* Tier::values(size_t& count) {
  static const Tier* const all[] = {
    &Tier::LOCAL,
    &Tier::REMOTE_1,
    &Tier::REMOTE_2,
    &Tier::REMOTE_3,
  };
  count = sizeof(all) / sizeof(all[0]);
  return all;
}

// ---- GatewayClass ----------------------------------------------------------

// Global singleton defined here; declared extern in Tier.h.
GatewayClass Gateway;

void GatewayClass::begin() {
  Preferences prefs;
  if (prefs.begin(TIER_NVS_NAMESPACE, /*readOnly=*/true)) {
    String stored = prefs.getString(TIER_NVS_KEY, "");
    prefs.end();
    if (stored.length() > 0) {
      if (setTierFromString(stored.c_str())) {
        Log_info("Gateway tier loaded from NVS: %s", getTierName());
        return;
      }
      Log_warn("Gateway tier NVS value '%s' unrecognised - defaulting to unset", stored.c_str());
    }
  }
  Log_info("Gateway tier not set (no NVS value). Select a tier in the captive portal.");
}

void GatewayClass::saveTier() const {
  Preferences prefs;
  if (prefs.begin(TIER_NVS_NAMESPACE, /*readOnly=*/false)) {
    prefs.putString(TIER_NVS_KEY, getTierName());
    prefs.end();
    Log_info("Gateway tier saved to NVS: %s", getTierName());
  } else {
    Log_error("Gateway tier save failed - could not open NVS namespace '%s'", TIER_NVS_NAMESPACE);
  }
}

const Tier& GatewayClass::getTier() const {
  // Return LOCAL as a safe fallback when nothing has been configured yet.
  return (activeTier != nullptr) ? *activeTier : Tier::LOCAL;
}

void GatewayClass::setTier(const Tier& tier) {
  activeTier = &tier;
}

bool GatewayClass::setTierFromString(const char* name) {
  if (name == nullptr) {
    return false;
  }
  size_t count;
  const Tier* const* all = Tier::values(count);
  for (size_t i = 0; i < count; i++) {
    if (strcmp(all[i]->name(), name) == 0) {
      activeTier = all[i];
      return true;
    }
  }
  return false;
}

const char* GatewayClass::getTierName() const {
  return (activeTier != nullptr) ? activeTier->name() : "UNSET";
}

bool GatewayClass::isConfigured() const {
  return activeTier != nullptr;
}

bool GatewayClass::isLocal() const {
  return activeTier != nullptr && activeTier->isLocal();
}

bool GatewayClass::isRemote() const {
  return activeTier != nullptr && activeTier->isRemote();
}

uint8_t GatewayClass::defaultAddress() const {
  return getTier().defaultAddress();
}
