#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../LoRaIoT/src/DeviceRegistry.h"

/**
 * LoRaState.h - Mutex-protected per-SRC link state (doc/LoRaProtocol.md §6, §8).
 *
 * Responsibilities:
 *   - LRU registry of known remote addresses (capacity = DEVICE_REGISTRY_MAX_NODES)
 *   - Per-SRC last SEQ and last boot_id for duplicate detection and reboot detection
 *   - Purge of entries unseen for LORA_STATE_STALE_MS (180 s)
 *
 * Used on both LOCAL and REMOTE (both sides track their peer's SEQ and boot_id).
 * On LOCAL, DeviceRegistry additionally holds the cached device/metric definitions;
 * LoRaState is the lower-level link-layer state only.
 *
 * Capacity is intentionally tied to DEVICE_REGISTRY_MAX_NODES (currently 3): both
 * registries track the same set of remote nodes so they must stay in sync.
 *
 * ARDUINO SINGLETON PATTERN:
 *   'LoRaStateClass' is the class type; 'LoRaState' is the global instance.
 */

// Entry is purged if no frame has been received from it for this many milliseconds (180 s).
// doc/LoRaProtocol.md §8: "3 missed slow beacons".
static constexpr uint32_t LORA_STATE_STALE_MS    = 180000;

/**
 * One entry in the per-SRC registry.
 *
 * lastSeq is the SEQ of the last accepted (non-duplicate) frame from src.
 * A frame is a duplicate when its SEQ equals lastSeq AND its TYPE equals lastType
 * (doc/LoRaProtocol.md §6). The pair is stored as uint16_t + uint8_t.
 *
 * lastBootId is the boot_id most recently seen in a PING or BEACON from src.
 * When a new boot_id arrives, lastSeq and lastType are reset so the fresh run's
 * SEQ=0 is not treated as a duplicate of a previous run (§6.1).
 */
struct LoRaRemoteEntry {
  uint8_t  src;           // LoRa node address of the remote (0x01..0xFE)
  uint16_t lastSeq;       // SEQ of the last accepted frame
  uint8_t  lastType;      // TYPE of the last accepted frame (LoRaFrameType cast to uint8_t)
  uint32_t lastBootId;    // boot_id last seen from this src (0 = not yet seen)
  uint32_t lastSeenMs;    // millis() when any frame was last received from src
  bool     seqValid;      // false until the first frame from src is processed
};

class LoRaStateClass {
 public:
  /**
   * Initialize the internal mutex. Call once from applicationSetup() before
   * LoRaTransport.begin(). Safe to call multiple times.
   */
  void begin();

  /**
   * Called on every accepted incoming frame (after CRC passes, before duplicate check).
   * Updates lastSeenMs and, if boot_id has changed, resets SEQ state for src.
   *
   * Parameters:
   *   src    - SRC address from the frame header.
   *   seq    - SEQ from the frame header.
   *   type   - TYPE from the frame header (uint8_t value).
   *   bootId - boot_id carried in the payload (0 if the frame type does not carry one,
   *            e.g. ACK, DATA; the caller passes 0 in that case).
   *
   * Returns true if the frame is NEW (should be processed).
   * Returns false if the frame is a DUPLICATE (same SEQ+TYPE as the last accepted frame
   * from the same src, with no boot_id change in between).
   */
  bool updateAndCheckDuplicate(uint8_t src, uint16_t seq, uint8_t type, uint32_t bootId);

  /**
   * Remove entries that have not been seen for more than LORA_STATE_STALE_MS.
   * Call periodically from applicationLoop() (e.g. once per second).
   */
  void purgeStale();

  /**
   * Return a snapshot of the entry for src, or false if src is not in the registry.
   * Thread-safe.
   */
  bool getEntry(uint8_t src, LoRaRemoteEntry& out) const;

  /**
   * Fill outBuf with up to maxCount currently tracked SRC addresses.
   * Returns the actual count written. Thread-safe.
   */
  uint8_t listRemotes(uint8_t* outBuf, uint8_t maxCount) const;

 private:
  LoRaRemoteEntry entries[DEVICE_REGISTRY_MAX_NODES];
  uint8_t         entryCount = 0;
  SemaphoreHandle_t mutex    = nullptr;

  /** Find the index of src, or -1 if absent. Caller must hold mutex. */
  int8_t findEntry(uint8_t src) const;

  /** Find or create an entry for src. Returns -1 if the table is full. Caller must hold mutex. */
  int8_t findOrCreateEntry(uint8_t src);
};

/** Global singleton. Include LoRaState.h and use LoRaState.* anywhere. */
extern LoRaStateClass LoRaState;
