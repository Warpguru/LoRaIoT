#include "PeerRegistry.h"

#include "../../util/src/LogBuffer.h"

// Global instances memory allocation
PeerRegistry SensorRegistry;
PeerRegistry ControlRegistry;

void PeerRegistry::begin() {
  memset(peers, 0, sizeof(peers));
  peerCount_ = 0;
  mutex = xSemaphoreCreateMutex();
  if (mutex == nullptr) {
    Log_error("PeerRegistry::begin - Failed to create mutex!");
  }
}

// ---- Owner functions -------------------------------------------------------

bool PeerRegistry::registerMetric(uint8_t addr, const char* deviceName, const char* metricName,
                                  const char* unit, uint32_t scale,
                                  uint32_t& outDeviceIdx, uint32_t& outMetricId) {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findOrCreatePeer(addr);
  if (idx < 0) {
    Log_warn("PeerRegistry::registerMetric - peer table full, cannot track addr=0x%02X", addr);
    xSemaphoreGive(mutex);
    return false;
  }

  bool ok = peers[idx].table.registerMetric(deviceName, metricName, unit, scale,
                                            outDeviceIdx, outMetricId);
  xSemaphoreGive(mutex);
  return ok;
}

void PeerRegistry::storeValue(uint8_t addr, uint32_t deviceIdx, uint32_t metricId,
                              int32_t scaledValue) {
  if (mutex == nullptr) {
    return;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findPeer(addr);
  if (idx >= 0) {
    peers[idx].table.storeValue(deviceIdx, metricId, scaledValue, millis());
  }

  xSemaphoreGive(mutex);
}

uint32_t PeerRegistry::getRegistryRev(uint8_t addr) const {
  if (mutex == nullptr) {
    return 0;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findPeer(addr);
  uint32_t rev = (idx >= 0) ? peers[idx].table.getRev() : 0;

  xSemaphoreGive(mutex);
  return rev;
}

// ---- Cache functions -------------------------------------------------------

bool PeerRegistry::updatePeerMeta(uint8_t addr, uint32_t bootId,
                                  bool hasRegistryRev, uint32_t registryRev) {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findOrCreatePeer(addr);
  if (idx < 0) {
    Log_warn("PeerRegistry::updatePeerMeta - peer table full, cannot track addr=0x%02X", addr);
    xSemaphoreGive(mutex);
    return false;
  }

  PeerEntry& peer = peers[idx];
  bool crawlNeeded = false;

  // Detect reboot: boot_id changed means the peer has restarted;
  // all cached definitions are stale and must be discarded.
  if (peer.bootId != PEER_BOOT_ID_NONE && peer.bootId != bootId) {
    Log_info("PeerRegistry: peer 0x%02X rebooted (boot_id 0x%08X -> 0x%08X), clearing cache",
             addr, peer.bootId, bootId);
    peer.table.clear();
    peer.registryRev  = PEER_REV_NONE;
    peer.crawlPending = false;
    crawlNeeded       = true;
  }

  peer.addr       = addr;
  peer.bootId     = bootId;
  peer.lastSeenMs = millis();

  // A BEACON proves the peer is alive, so a peer presumed dead comes back online.
  // Its cached definitions were kept while offline: if boot_id and registry_rev are
  // unchanged (checked above and below) no new Describe crawl is needed.
  if (!peer.online) {
    Log_info("PeerRegistry: peer 0x%02X is online again (BEACON)", addr);
    peer.online = true;
  }

  if (hasRegistryRev) {
    peer.pendingRev = registryRev;
    // Schedule a crawl if we have never crawled, or the rev has advanced.
    if (!crawlNeeded &&
        (peer.registryRev == PEER_REV_NONE || peer.registryRev != registryRev)) {
      Log_info("PeerRegistry: peer 0x%02X registry_rev changed (%u -> %u), scheduling Describe crawl",
               addr, peer.registryRev, registryRev);
      crawlNeeded = true;
    }
  }

  if (crawlNeeded) {
    peer.crawlPending = true;
  }

  xSemaphoreGive(mutex);
  return crawlNeeded;
}

void PeerRegistry::storeDevice(uint8_t addr, const loraiot_Device& dev) {
  if (mutex == nullptr) {
    return;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findOrCreatePeer(addr);
  if (idx < 0) {
    Log_warn("PeerRegistry::storeDevice - peer table full, cannot store device for addr=0x%02X", addr);
    xSemaphoreGive(mutex);
    return;
  }

  PeerEntry& peer = peers[idx];
  DeviceTableResult res = peer.table.applyDevice(dev);
  if (res == DeviceTableResult::OK) {
    Log_info("PeerRegistry: stored/updated device[%u] '%s' for peer 0x%02X (%u metrics)",
             dev.device, dev.name, addr, dev.metrics_count);
  } else {
    Log_warn("PeerRegistry: failed to store device[%u] for peer 0x%02X (err=%u)",
             dev.device, addr, static_cast<uint8_t>(res));
  }

  xSemaphoreGive(mutex);
}

void PeerRegistry::markCrawlComplete(uint8_t addr, uint32_t finalRev) {
  if (mutex == nullptr) {
    return;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findPeer(addr);
  if (idx >= 0) {
    peers[idx].registryRev  = finalRev;
    peers[idx].crawlPending = false;
    Log_info("PeerRegistry: Describe crawl complete for peer 0x%02X, rev=%u, devices=%u",
             addr, finalRev, peers[idx].table.getDeviceCount());
  }

  xSemaphoreGive(mutex);
}

bool PeerRegistry::storeData(uint8_t addr, const loraiot_Data& data) {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findPeer(addr);
  if (idx < 0) {
    xSemaphoreGive(mutex);
    return false;
  }

  DeviceTableResult res = peers[idx].table.applyData(data, millis());
  xSemaphoreGive(mutex);
  return (res == DeviceTableResult::OK || res == DeviceTableResult::UNKNOWN_METRIC);
}

// ---- Liveness --------------------------------------------------------------

// markSeen() is called for EVERY valid frame from a peer (not only BEACON), so the
// keep-alive logic sees DATA, RESPONSE, ACK etc. as proof of life and stays silent
// while a peer is busy. It never creates an entry: only a BEACON may add a peer,
// otherwise a stray frame could occupy one of the few slots.
bool PeerRegistry::markSeen(uint8_t addr) {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  bool wasOffline = false;
  int8_t idx = findPeer(addr);
  if (idx >= 0) {
    wasOffline = !peers[idx].online;
    peers[idx].online     = true;
    peers[idx].lastSeenMs = millis();
  }

  xSemaphoreGive(mutex);

  // Log after releasing the mutex: logging can be slow and must not block other tasks.
  if (wasOffline) {
    Log_info("PeerRegistry: peer 0x%02X is online again", addr);
  }
  return wasOffline;
}

// markOffline() keeps the entry on purpose (see PeerEntry::online): a peer that was
// only out of range returns with the same boot_id and registry_rev, so its cached
// definitions stay valid and no Describe crawl is needed.
void PeerRegistry::markOffline(uint8_t addr) {
  if (mutex == nullptr) {
    return;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  bool changed = false;
  int8_t idx = findPeer(addr);
  if (idx >= 0 && peers[idx].online) {
    peers[idx].online = false;
    changed = true;
  }

  xSemaphoreGive(mutex);

  if (changed) {
    Log_warn("PeerRegistry: peer 0x%02X marked offline", addr);
  }
}

// getLiveness()/getBootId() exist because copying a whole PeerEntry (about 1.4 KB)
// onto a small task stack risks a stack overflow; callers only need a few scalars.
bool PeerRegistry::getLiveness(uint8_t addr, uint32_t& lastSeenMs, bool& online) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findPeer(addr);
  const bool found = (idx >= 0);
  if (found) {
    lastSeenMs = peers[idx].lastSeenMs;
    online     = peers[idx].online;
  }

  xSemaphoreGive(mutex);
  return found;
}

// The Ack message carries no boot_id (doc/LoRaProtocol.md section 6.1), so a REST ping
// reports the value LOCAL learned from the node's last BEACON.
bool PeerRegistry::getBootId(uint8_t addr, uint32_t& bootId) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findPeer(addr);
  const bool found = (idx >= 0) && (peers[idx].bootId != PEER_BOOT_ID_NONE);
  if (found) {
    bootId = peers[idx].bootId;
  }

  xSemaphoreGive(mutex);
  return found;
}

// ---- Query -----------------------------------------------------------------

bool PeerRegistry::getNodeInfo(uint8_t addr, uint32_t& bootId, uint32_t& registryRev,
                               uint32_t& lastSeenMs, bool& online, bool& crawlPending) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  int8_t idx = findPeer(addr);
  if (idx < 0) {
    xSemaphoreGive(mutex);
    return false;
  }
  const PeerEntry& p = peers[idx];
  bootId       = p.bootId;
  registryRev  = (p.registryRev != PEER_REV_NONE) ? p.registryRev : p.table.getRev();
  lastSeenMs   = p.lastSeenMs;
  online       = p.online;
  crawlPending = p.crawlPending;
  xSemaphoreGive(mutex);
  return true;
}

bool PeerRegistry::getDeviceSlot(uint8_t addr, uint8_t deviceIdx, DeviceSlot& out) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  int8_t idx = findPeer(addr);
  if (idx < 0) {
    xSemaphoreGive(mutex);
    return false;
  }
  bool ok = peers[idx].table.copyDevice(deviceIdx, out);
  xSemaphoreGive(mutex);
  return ok;
}

bool PeerRegistry::isCrawlPending(uint8_t addr) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  int8_t idx = findPeer(addr);
  bool pending = (idx >= 0) && peers[idx].crawlPending;
  xSemaphoreGive(mutex);
  return pending;
}

// Only raises the crawlPending flag; no frame is sent here. describeCrawlerLoop() (on the
// applicationLoop task) polls isCrawlPending() and starts the Describe crawl on its next tick,
// so this is safe to call from an ESPAsyncWebServer handler: it takes the registry mutex
// briefly and never blocks on the LoRa link.
//
// Unlike updatePeerMeta() it does not clear the cached device table or touch registryRev /
// pendingRev. The crawl overwrites the cached definitions in place and, when it completes,
// markCrawlComplete() records the pendingRev learned from the peer's last BEACON/PING.
// Calling it while a crawl is already pending or in progress is harmless (the flag is
// already set; no second crawl is queued).
//
// Unknown peers are not created: a crawl needs a peer that has been seen at least once.
bool PeerRegistry::requestCrawl(uint8_t addr) {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findPeer(addr);
  const bool found = (idx >= 0);
  if (found) {
    peers[idx].crawlPending = true;
  }

  xSemaphoreGive(mutex);
  return found;
}

bool PeerRegistry::getPendingRev(uint8_t addr, uint32_t& pendingRev) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  int8_t idx = findPeer(addr);
  if (idx < 0) {
    xSemaphoreGive(mutex);
    return false;
  }
  pendingRev = peers[idx].pendingRev;
  xSemaphoreGive(mutex);
  return true;
}

uint8_t PeerRegistry::getDeviceCount(uint8_t addr) const {
  if (mutex == nullptr) {
    return 0;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  int8_t idx = findPeer(addr);
  uint8_t count = (idx >= 0) ? peers[idx].table.getDeviceCount() : 0;
  xSemaphoreGive(mutex);
  return count;
}

uint8_t PeerRegistry::listPeers(uint8_t* outBuf, uint8_t maxCount) const {
  if (mutex == nullptr) {
    return 0;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  uint8_t count = (peerCount_ < maxCount) ? peerCount_ : maxCount;
  for (uint8_t i = 0; i < count; i++) {
    outBuf[i] = peers[i].addr;
  }

  xSemaphoreGive(mutex);
  return count;
}

uint8_t PeerRegistry::peerCount() const {
  return peerCount_;
}

// ---- Proto helpers ---------------------------------------------------------

bool PeerRegistry::toProtoDevice(uint8_t addr, uint32_t deviceIdx, loraiot_Device& out) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  int8_t idx = findPeer(addr);
  bool ok = (idx >= 0) && peers[idx].table.toProtoDevice(deviceIdx, out);
  xSemaphoreGive(mutex);
  return ok;
}

bool PeerRegistry::toProtoData(uint8_t addr, uint32_t deviceIdx, loraiot_Data& out) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  int8_t idx = findPeer(addr);
  bool ok = (idx >= 0) && peers[idx].table.toProtoData(deviceIdx, out, millis());
  xSemaphoreGive(mutex);
  return ok;
}

// ----- private helpers -------------------------------------------------------

int8_t PeerRegistry::findPeer(uint8_t addr) const {
  for (uint8_t i = 0; i < peerCount_; i++) {
    if (peers[i].addr == addr) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

int8_t PeerRegistry::findOrCreatePeer(uint8_t addr) {
  int8_t idx = findPeer(addr);
  if (idx >= 0) {
    return idx;
  }

  if (peerCount_ >= LORA_MAX_PEERS) {
    // Table full. Offline peers are kept (not removed) so a returning peer keeps its
    // cache; the slot is only reclaimed now, when a new address needs it. Evict the
    // offline peer that has been silent the longest. Unsigned subtraction keeps the
    // age correct when millis() wraps. Online peers are never evicted.
    const uint32_t now = millis();
    int8_t   victim    = -1;
    uint32_t victimAge = 0;
    for (uint8_t i = 0; i < peerCount_; i++) {
      if (peers[i].online) {
        continue;
      }
      const uint32_t age = now - peers[i].lastSeenMs;
      if (victim < 0 || age > victimAge) {
        victim    = static_cast<int8_t>(i);
        victimAge = age;
      }
    }
    if (victim < 0) {
      return -1;   // every peer is online: the new peer is rejected
    }
    Log_info("PeerRegistry: table full, reusing slot of offline peer 0x%02X for 0x%02X",
             peers[victim].addr, addr);
    idx = victim;
  } else {
    idx = static_cast<int8_t>(peerCount_++);
  }

  // Start from a clean entry; a new peer counts as online, because it was just heard.
  memset(&peers[idx], 0, sizeof(PeerEntry));
  peers[idx].online      = true;
  peers[idx].lastSeenMs  = millis();
  peers[idx].addr        = addr;
  peers[idx].bootId      = PEER_BOOT_ID_NONE;
  peers[idx].registryRev = PEER_REV_NONE;
  peers[idx].pendingRev  = PEER_REV_NONE;
  return idx;
}
