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
  if (idx >= 0) return idx;

  if (nodeCount_ >= DEVICE_REGISTRY_MAX_NODES) return -1;

  idx = static_cast<int8_t>(nodeCount_++);
  memset(&nodes[idx], 0, sizeof(NodeEntry));
  nodes[idx].addr        = addr;
  nodes[idx].bootId      = DEVICE_REGISTRY_BOOT_ID_NONE;
  nodes[idx].registryRev = DEVICE_REGISTRY_REV_NONE;
  nodes[idx].pendingRev  = DEVICE_REGISTRY_REV_NONE;
  return idx;
}
