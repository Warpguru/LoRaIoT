#include "LoRaState.h"

#include "../../util/src/LogBuffer.h"

/**
 * LoRaState.cpp - Per-SRC link-layer state: duplicate detection, reboot detection, LRU registry.
 *
 * Design notes:
 *
 * Duplicate detection (doc/LoRaProtocol.md §6):
 *   A frame is a duplicate when its SEQ equals the last accepted SEQ *and* its TYPE equals the
 *   last accepted TYPE for the same SRC. Both fields must match because the same SEQ value can
 *   be legitimately reused by different frame types (the counter is per-node, shared across types).
 *
 * Reboot detection (doc/LoRaProtocol.md §6.1):
 *   Every PING and BEACON carries a non-zero boot_id. When updateAndCheckDuplicate() sees a
 *   boot_id that differs from the stored one (or sees a non-zero value for the first time), it
 *   resets seqValid so the very next frame from that src is always treated as new, regardless of
 *   its SEQ value. This prevents the new run's SEQ=N from being falsely matched against a SEQ=N
 *   that happened to be the last one in the previous run.
 *
 * LRU purge (doc/LoRaProtocol.md §8):
 *   purgeStale() removes entries unseen for LORA_STATE_STALE_MS (180 s). It compacts the array
 *   in-place by shifting remaining entries left, so entryCount stays accurate.
 *
 * Thread safety:
 *   All public methods acquire the internal FreeRTOS mutex before touching shared state, so
 *   they can be called from any task (loraTransportTask, applicationLoop, REST callbacks).
 */

// Global singleton instance.
LoRaStateClass LoRaState;

// ---- Private helpers -------------------------------------------------------

int8_t LoRaStateClass::findEntry(uint8_t src) const {
  for (uint8_t i = 0; i < entryCount; i++) {
    if (entries[i].src == src) return static_cast<int8_t>(i);
  }
  return -1;
}

int8_t LoRaStateClass::findOrCreateEntry(uint8_t src) {
  int8_t idx = findEntry(src);
  if (idx >= 0) return idx;

  if (entryCount >= LORA_MAX_PEERS) {
    // Table full: evict the oldest entry (smallest lastSeenMs).
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < entryCount; i++) {
      if (entries[i].lastSeenMs < entries[oldest].lastSeenMs) {
        oldest = i;
      }
    }
    Log_warn("LoRaState: registry full (%u entries); evicting oldest src=0x%02X",
             LORA_MAX_PEERS, entries[oldest].src);
    idx = static_cast<int8_t>(oldest);
  } else {
    idx = static_cast<int8_t>(entryCount++);
  }

  LoRaRemoteEntry& e = entries[idx];
  e.src        = src;
  e.lastSeq    = 0;
  e.lastType   = 0;
  e.lastBootId = 0;
  e.lastSeenMs = millis();
  e.seqValid   = false;
  return idx;
}

// ---- Public API ------------------------------------------------------------

void LoRaStateClass::begin() {
  if (mutex != nullptr) return;   // already initialised
  mutex = xSemaphoreCreateMutex();
  if (mutex == nullptr) {
    Log_error("LoRaState::begin - failed to create mutex");
  }
}

bool LoRaStateClass::updateAndCheckDuplicate(uint8_t src, uint16_t seq,
                                              uint8_t type, uint32_t bootId) {
  if (mutex == nullptr) return true;   // not initialised: let all frames through
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findOrCreateEntry(src);
  if (idx < 0) {
    // Should not happen (findOrCreateEntry evicts if full), but be safe.
    xSemaphoreGive(mutex);
    return true;
  }

  LoRaRemoteEntry& e = entries[idx];
  e.lastSeenMs = millis();

  // Reboot detection: a non-zero boot_id that differs from the stored one means the
  // remote has rebooted. Reset SEQ state so the first frame of the new run is always new.
  if (bootId != 0 && bootId != e.lastBootId) {
    Log_info("LoRaState: src=0x%02X boot_id changed 0x%08X -> 0x%08X; resetting SEQ state",
             src, e.lastBootId, bootId);
    e.lastBootId = bootId;
    e.seqValid   = false;   // next frame is always new, whatever its SEQ
  }

  // Duplicate detection: seqValid is false on first frame and after a reboot.
  bool isDuplicate = false;
  if (e.seqValid && e.lastSeq == seq && e.lastType == type) {
    isDuplicate = true;
    Log_debug("LoRaState: duplicate frame src=0x%02X seq=%u type=0x%02X", src, seq, type);
  }

  if (!isDuplicate) {
    e.lastSeq   = seq;
    e.lastType  = type;
    e.seqValid  = true;
  }

  xSemaphoreGive(mutex);
  return !isDuplicate;
}

void LoRaStateClass::purgeStale() {
  if (mutex == nullptr) return;
  xSemaphoreTake(mutex, portMAX_DELAY);

  const uint32_t now = millis();
  uint8_t i = 0;
  while (i < entryCount) {
    if (now - entries[i].lastSeenMs > LORA_STATE_STALE_MS) {
      Log_info("LoRaState: purging stale entry src=0x%02X (last seen %lu ms ago)",
               entries[i].src, static_cast<unsigned long>(now - entries[i].lastSeenMs));
      // Compact: overwrite with the last entry.
      entries[i] = entries[--entryCount];
      // Do not increment i: re-check the slot that was just moved into position i.
    } else {
      i++;
    }
  }

  xSemaphoreGive(mutex);
}

bool LoRaStateClass::getEntry(uint8_t src, LoRaRemoteEntry& out) const {
  if (mutex == nullptr) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  const int8_t idx = findEntry(src);
  if (idx >= 0) out = entries[idx];
  xSemaphoreGive(mutex);
  return idx >= 0;
}

uint8_t LoRaStateClass::listRemotes(uint8_t* outBuf, uint8_t maxCount) const {
  if (mutex == nullptr || outBuf == nullptr) return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  const uint8_t count = (entryCount < maxCount) ? entryCount : maxCount;
  for (uint8_t i = 0; i < count; i++) {
    outBuf[i] = entries[i].src;
  }
  xSemaphoreGive(mutex);
  return count;
}
