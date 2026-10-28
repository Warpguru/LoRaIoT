#pragma once

#include <Arduino.h>

/**
 * Tier.h - Gateway tier selection (LOCAL vs REMOTE).
 *
 * "Tier" describes the role of this LoRaIoT instance:
 *
 *   LOCAL  - Connected to the local WLAN. Receives LoRa frames
 *            from REMOTE instances and forwards sensor data upstream.
 *
 *   REMOTE - Hosts the WiFi SoftAP that IoT sensors connect to. Forwards their
 *            data over the LoRa link to the LOCAL instance.
 *
 * The active tier is:
 *   - Selected once in the WiFiManager captive-portal form.
 *   - Persisted in NVS (namespace TIER_NVS_NAMESPACE, key TIER_NVS_KEY).
 *   - Loaded at boot via Gateway.begin() before setupServer().
 *   - Readable at runtime via Gateway.getTier() / GET /api/tier.
 *
 * Design mirrors LogLevel / LogBufferClass so both follow the same pattern:
 *   - Tier      = the immutable value type (like LogLevel)
 *   - GatewayClass = the singleton that owns NVS persistence (like LogBufferClass)
 *
 * NVS namespace and key are separate constants so they can be referenced from
 * any translation unit without including the whole class.
 */

// NVS namespace and key used to persist the tier across reboots.
static constexpr const char* TIER_NVS_NAMESPACE = "sc_prefs";   // shared with LogBuffer
static constexpr const char* TIER_NVS_KEY        = "tier";

/**
 * Tier - immutable value type representing one gateway role.
 *
 * Only static constants (LOCAL, REMOTE_1, REMOTE_2, REMOTE_3) may exist - the constructor
 * is private. String literals live in flash; no heap allocation.
 */
class Tier {
 public:
  /** Human-readable name used in NVS, REST API responses, and portal HTML. */
  const char* name() const { return tierName; }

  /** Default LoRa node address for this tier. */
  uint8_t defaultAddress() const { return defaultAddr; }

  /** True if this is the LOCAL tier. */
  bool isLocal() const { return defaultAddr == 0x01; }

  /** True if this is any REMOTE tier. */
  bool isRemote() const { return defaultAddr >= 0x02; }

  /** The valid tiers. */
  static const Tier LOCAL;     ///< Handles data received from REMOTE; connects to local WLAN.
  static const Tier REMOTE_1;  ///< REMOTE Node 1 (address 0x02).
  static const Tier REMOTE_2;  ///< REMOTE Node 2 (address 0x03).
  static const Tier REMOTE_3;  ///< REMOTE Node 3 (address 0x04).

  /**
   * All tiers in definition order as a plain C array.
   * Usage:
   *   size_t count;
   *   const Tier* const* tiers = Tier::values(count);
   */
  static const Tier* const* values(size_t& count);

 private:
  constexpr Tier(const char* name, uint8_t addr) : tierName(name), defaultAddr(addr) {}
  const char* tierName;
  uint8_t defaultAddr;
};

/**
 * GatewayClass - singleton that owns the active Tier and its NVS persistence.
 *
 * Mirrors LogBufferClass. The global singleton is `Gateway` (defined in Tier.cpp).
 *
 * Call begin() once from applicationSetup() before setupServer(). It loads
 * the persisted tier from NVS; if none is found the default (UNSET) is used
 * until the user selects one in the captive portal.
 */
class GatewayClass {
 public:
  /**
   * Load the persisted tier from NVS. Must be called once before setupServer()
   * so the portal pre-selects the previously chosen tier.
   */
  void begin();

  /** Persist the current tier to NVS. Called after every portal save. */
  void saveTier() const;

  /** Return the currently active tier. */
  const Tier& getTier() const;

  /** Set the active tier. */
  void setTier(const Tier& tier);

  /**
   * Set the tier from a name string (e.g. "LOCAL", "REMOTE (Node 1)").
   * Returns false and leaves the tier unchanged if the name is unrecognised.
   */
  bool setTierFromString(const char* name);

  /** Canonical name of the active tier, e.g. "LOCAL". */
  const char* getTierName() const;

  /** True when a valid tier has been selected (not the boot default). */
  bool isConfigured() const;

  /** True if the configured tier is LOCAL. */
  bool isLocal() const;

  /** True if the configured tier is any REMOTE instance. */
  bool isRemote() const;

  /** Default LoRa node address for the active tier. */
  uint8_t defaultAddress() const;

 private:
  const Tier* activeTier = nullptr;  ///< nullptr = not yet set
};

/** Global singleton. Include Tier.h and use Gateway.getTier() anywhere. */
extern GatewayClass Gateway;
