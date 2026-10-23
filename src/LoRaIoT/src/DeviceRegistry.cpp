#include "DeviceRegistry.h"

#include "../../util/src/LogBuffer.h"

// Global singleton memory allocation
DeviceRegistryClass DeviceRegistry;

void DeviceRegistryClass::begin() {
  memset(nodes, 0, sizeof(nodes));
  nodeCount_ = 0;
  mutex = xSemaphoreCreateMutex();
  if (mutex == nullptr) {
    Log_error("DeviceRegistry::begin - Failed to create mutex!");
  }
}

bool DeviceRegistryClass::updateNodeMeta(uint8_t addr, uint32_t bootId,
                                          bool hasRegistryRev, uint32_t registryRev) {
  if (mutex == nullptr) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findOrCreateNode(addr);
  if (idx < 0) {
    Log_warn("DeviceRegistry::updateNodeMeta - node table full, cannot track addr=0x%02X", addr);
    xSemaphoreGive(mutex);
    return false;
  }

  NodeEntry& node = nodes[idx];
  bool crawlNeeded = false;

  // Detect reboot: boot_id changed means the remote node has restarted;
  // all cached definitions are stale and must be discarded.
  if (node.bootId != DEVICE_REGISTRY_BOOT_ID_NONE && node.bootId != bootId) {
    Log_info("DeviceRegistry: node 0x%02X rebooted (boot_id 0x%08X -> 0x%08X), clearing cache",
             addr, node.bootId, bootId);
    node.deviceCount  = 0;
    node.registryRev  = DEVICE_REGISTRY_REV_NONE;
    node.crawlPending = false;
    crawlNeeded       = true;
  }

  node.addr        = addr;
  node.bootId      = bootId;
  node.lastSeenMs  = millis();

  // A BEACON proves the node is alive, so a node presumed dead comes back online.
  // Its cached definitions were kept while offline: if boot_id and registry_rev are
  // unchanged (checked above and below) no new Describe crawl is needed.
  if (!node.online) {
    Log_info("DeviceRegistry: node 0x%02X is online again (BEACON)", addr);
    node.online = true;
  }

  if (hasRegistryRev) {
    node.pendingRev = registryRev;
    // Schedule a crawl if we have never crawled, or the rev has advanced.
    if (!crawlNeeded &&
        (node.registryRev == DEVICE_REGISTRY_REV_NONE || node.registryRev != registryRev)) {
      Log_info("DeviceRegistry: node 0x%02X registry_rev changed (%u -> %u), scheduling Describe crawl",
               addr, node.registryRev, registryRev);
      crawlNeeded = true;
    }
  }

  if (crawlNeeded) {
    node.crawlPending = true;
  }

  xSemaphoreGive(mutex);
  return crawlNeeded;
}

void DeviceRegistryClass::storeDevice(uint8_t addr, const loraiot_Device& dev) {
  if (mutex == nullptr) return;
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findOrCreateNode(addr);
  if (idx < 0) {
    Log_warn("DeviceRegistry::storeDevice - node table full, cannot store device for addr=0x%02X", addr);
    xSemaphoreGive(mutex);
    return;
  }

  NodeEntry& node = nodes[idx];

  // Check whether this device index already exists (update) or is new (insert).
  for (uint8_t i = 0; i < node.deviceCount; i++) {
    if (node.devices[i].def.device == dev.device) {
      node.devices[i].def = dev;
      Log_debug("DeviceRegistry: updated device[%u] '%s' for node 0x%02X",
                dev.device, dev.name, addr);
      xSemaphoreGive(mutex);
      return;
    }
  }

  if (node.deviceCount >= DEVICE_REGISTRY_MAX_DEVICES) {
    Log_warn("DeviceRegistry::storeDevice - device table full for node 0x%02X", addr);
    xSemaphoreGive(mutex);
    return;
  }

  CachedDevice& slot = node.devices[node.deviceCount++];
  slot.def        = dev;
  slot.hasData    = false;
  slot.lastDataMs = 0;
  memset(&slot.lastData, 0, sizeof(slot.lastData));

  Log_info("DeviceRegistry: stored device[%u] '%s' for node 0x%02X (%u metrics)",
           dev.device, dev.name, addr, dev.metrics_count);

  xSemaphoreGive(mutex);
}

void DeviceRegistryClass::markCrawlComplete(uint8_t addr, uint32_t finalRev) {
  if (mutex == nullptr) return;
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findNode(addr);
  if (idx >= 0) {
    nodes[idx].registryRev  = finalRev;
    nodes[idx].crawlPending = false;
    Log_info("DeviceRegistry: Describe crawl complete for node 0x%02X, rev=%u, devices=%u",
             addr, finalRev, nodes[idx].deviceCount);
  }

  xSemaphoreGive(mutex);
}

void DeviceRegistryClass::storeData(uint8_t addr, const loraiot_Data& data) {
  if (mutex == nullptr) return;
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findNode(addr);
  if (idx < 0) {
    xSemaphoreGive(mutex);
    return;
  }

  NodeEntry& node = nodes[idx];
  for (uint8_t i = 0; i < node.deviceCount; i++) {
    if (node.devices[i].def.device == data.device) {
      node.devices[i].lastData    = data;
      node.devices[i].hasData     = true;
      node.devices[i].lastDataMs  = millis();
      break;
    }
  }

  xSemaphoreGive(mutex);
}

// markSeen() is called for EVERY valid frame from a node (not only BEACON), so the
// keep-alive logic sees DATA, RESPONSE, ACK etc. as proof of life and stays silent
// while a node is busy. It never creates an entry: only a BEACON may add a node,
// otherwise a stray frame could occupy one of the few slots.
bool DeviceRegistryClass::markSeen(uint8_t addr) {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  bool wasOffline = false;
  int8_t idx = findNode(addr);
  if (idx >= 0) {
    wasOffline = !nodes[idx].online;
    nodes[idx].online     = true;
    nodes[idx].lastSeenMs = millis();
  }

  xSemaphoreGive(mutex);

  // Log after releasing the mutex: logging can be slow and must not block other tasks.
  if (wasOffline) {
    Log_info("DeviceRegistry: node 0x%02X is online again", addr);
  }
  return wasOffline;
}

// markOffline() keeps the entry on purpose (see NodeEntry::online): a node that was
// only out of range returns with the same boot_id and registry_rev, so its cached
// definitions stay valid and no Describe crawl is needed.
void DeviceRegistryClass::markOffline(uint8_t addr) {
  if (mutex == nullptr) {
    return;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  bool changed = false;
  int8_t idx = findNode(addr);
  if (idx >= 0 && nodes[idx].online) {
    nodes[idx].online = false;
    changed = true;
  }

  xSemaphoreGive(mutex);

  if (changed) {
    Log_warn("DeviceRegistry: node 0x%02X marked offline", addr);
  }
}

// getLiveness()/getBootId() exist because getNode() copies the whole NodeEntry
// (about 2 KB). The ping task and REST handlers only need a few scalar fields, and
// copying 2 KB onto a 4 KB task stack risks a stack overflow.
bool DeviceRegistryClass::getLiveness(uint8_t addr, uint32_t& lastSeenMs, bool& online) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findNode(addr);
  const bool found = (idx >= 0);
  if (found) {
    lastSeenMs = nodes[idx].lastSeenMs;
    online     = nodes[idx].online;
  }

  xSemaphoreGive(mutex);
  return found;
}

// The Ack message carries no boot_id (doc/LoRaProtocol.md section 6.1), so a REST ping
// reports the value LOCAL learned from the node's last BEACON.
bool DeviceRegistryClass::getBootId(uint8_t addr, uint32_t& bootId) const {
  if (mutex == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findNode(addr);
  const bool found = (idx >= 0) && (nodes[idx].bootId != DEVICE_REGISTRY_BOOT_ID_NONE);
  if (found) {
    bootId = nodes[idx].bootId;
  }

  xSemaphoreGive(mutex);
  return found;
}

bool DeviceRegistryClass::getNode(uint8_t addr, NodeEntry& out) const {
  if (mutex == nullptr) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t idx = findNode(addr);
  bool found = (idx >= 0);
  if (found) {
    out = nodes[idx];
  }

  xSemaphoreGive(mutex);
  return found;
}

uint8_t DeviceRegistryClass::listNodes(uint8_t* outBuf, uint8_t maxCount) const {
  if (mutex == nullptr) return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);

  uint8_t count = (nodeCount_ < maxCount) ? nodeCount_ : maxCount;
  for (uint8_t i = 0; i < count; i++) {
    outBuf[i] = nodes[i].addr;
  }

  xSemaphoreGive(mutex);
  return count;
}

bool DeviceRegistryClass::getDevice(uint8_t addr, uint8_t deviceIdx, CachedDevice& out) const {
  if (mutex == nullptr) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);

  int8_t nIdx = findNode(addr);
  if (nIdx < 0) {
    xSemaphoreGive(mutex);
    return false;
  }

  const NodeEntry& node = nodes[nIdx];
  for (uint8_t i = 0; i < node.deviceCount; i++) {
    if (node.devices[i].def.device == deviceIdx) {
      out = node.devices[i];
      xSemaphoreGive(mutex);
      return true;
    }
  }

  xSemaphoreGive(mutex);
  return false;
}

uint8_t DeviceRegistryClass::nodeCount() const {
  return nodeCount_;
}

// ----- private helpers -------------------------------------------------------

int8_t DeviceRegistryClass::findNode(uint8_t addr) const {
  for (uint8_t i = 0; i < nodeCount_; i++) {
    if (nodes[i].addr == addr) return static_cast<int8_t>(i);
  }
  return -1;
}

int8_t DeviceRegistryClass::findOrCreateNode(uint8_t addr) {
  int8_t idx = findNode(addr);
  if (idx >= 0) {
    return idx;
  }

  if (nodeCount_ >= DEVICE_REGISTRY_MAX_NODES) {
    // Table full. Offline nodes are kept (not removed) so a returning node keeps its
    // cache; the slot is only reclaimed now, when a new address needs it. Evict the
    // offline node that has been silent the longest. Unsigned subtraction keeps the
    // age correct when millis() wraps. Online nodes are never evicted.
    const uint32_t now = millis();
    int8_t   victim    = -1;
    uint32_t victimAge = 0;
    for (uint8_t i = 0; i < nodeCount_; i++) {
      if (nodes[i].online) {
        continue;
      }
      const uint32_t age = now - nodes[i].lastSeenMs;
      if (victim < 0 || age > victimAge) {
        victim    = static_cast<int8_t>(i);
        victimAge = age;
      }
    }
    if (victim < 0) {
      return -1;   // every node is online: the new node is rejected
    }
    Log_info("DeviceRegistry: table full, reusing slot of offline node 0x%02X for 0x%02X",
             nodes[victim].addr, addr);
    idx = victim;
  } else {
    idx = static_cast<int8_t>(nodeCount_++);
  }

  // Start from a clean entry; a new node counts as online, because it was just heard.
  memset(&nodes[idx], 0, sizeof(NodeEntry));
  nodes[idx].online      = true;
  nodes[idx].lastSeenMs  = millis();
  nodes[idx].addr        = addr;
  nodes[idx].bootId      = DEVICE_REGISTRY_BOOT_ID_NONE;
  nodes[idx].registryRev = DEVICE_REGISTRY_REV_NONE;
  nodes[idx].pendingRev  = DEVICE_REGISTRY_REV_NONE;
  return idx;
}
