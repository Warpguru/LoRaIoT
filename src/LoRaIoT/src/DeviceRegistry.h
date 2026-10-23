#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../lora/src/nanopb/loraiot.pb.h"

/**
 * DeviceRegistry.h - LOCAL node in-memory cache for remote device/metric definitions.
 *
 * Populated by the Describe crawler (triggered whenever a BEACON carries a new
 * registry_rev or boot_id). Consumed by RestService to serve GET endpoints:
 *   GET /api/lora/nodes/{addr}/devices
 *   GET /api/lora/nodes/{addr}/devices/{device}
 *   GET /api/lora/nodes/{addr}/devices/{device}/metrics
 *
 * Thread safety: all public methods are protected by an internal FreeRTOS mutex.
 *
 * Capacity limits (matching the protobuf schema):
 *   - MAX_DEVICES devices per remote node address
 *   - Up to loraiot_Device.metrics[8] metrics per device
 *
 * Only exists on LOCAL. REMOTE uses SensorRegistry instead.
 */

// Maximum device slots cached per remote node: device 0 (the REMOTE node itself)
// + up to 4 SoftAP-connected sensors (indices 1..4).
static constexpr uint8_t DEVICE_REGISTRY_MAX_DEVICES = 5;

// Maximum number of remote node addresses tracked by LOCAL.
static constexpr uint8_t DEVICE_REGISTRY_MAX_NODES = 3;

// Sentinel value: no valid boot_id has been observed yet.
static constexpr uint32_t DEVICE_REGISTRY_BOOT_ID_NONE = 0;

// Sentinel value: no valid registry_rev has been observed yet.
static constexpr uint32_t DEVICE_REGISTRY_REV_NONE = UINT32_MAX;

/**
 * CachedDevice - one device definition received from a Describe response,
 * plus the most-recently-received measurement snapshot.
 */
struct CachedDevice {
  loraiot_Device  def;          // Full device definition (name + all metric definitions)
  loraiot_Data    lastData;     // Most recently received Data frame for this device
  bool            hasData;      // true once at least one Data frame has been stored
  uint32_t        lastDataMs;   // millis() when lastData was stored
};

/**
 * NodeEntry - everything LOCAL knows about one remote node address.
 */
struct NodeEntry {
  uint8_t      addr;                                   // LoRa node address (0x02..0xFE)
  uint32_t     bootId;                                 // Last seen boot_id from PING/BEACON
  uint32_t     registryRev;                            // Last successfully crawled registry_rev
  uint32_t     pendingRev;                             // registry_rev from the latest BEACON
                                                       // (may differ from registryRev while crawl is in progress)
  uint32_t     lastSeenMs;                             // millis() of the last received frame from addr
  bool         online;                                 // false = presumed dead (keep-alive PING failed);
                                                       // the entry and its cached definitions are kept and
                                                       // the slot is only reused when the table is full
  uint8_t      deviceCount;
  CachedDevice devices[DEVICE_REGISTRY_MAX_DEVICES];
  bool         crawlPending;                           // true = a Describe crawl has been requested
};

/**
 * DeviceRegistryClass - singleton that owns all NodeEntry records.
 *
 * The global singleton is `DeviceRegistry` (defined in DeviceRegistry.cpp).
 */
class DeviceRegistryClass {
 public:
  /**
   * Initialize the registry and create the internal FreeRTOS mutex.
   * Call once from applicationSetup() before LoRaTransport.begin().
   */
  void begin();

  /**
   * Update the metadata (boot_id, registry_rev) for a node seen via PING or BEACON.
   * If boot_id has changed the device cache for that node is cleared and a new
   * crawl is scheduled. If registry_rev has changed a new crawl is scheduled.
   * Returns true if a Describe crawl should be started for addr.
   */
  bool updateNodeMeta(uint8_t addr, uint32_t bootId, bool hasRegistryRev, uint32_t registryRev);

  /**
   * Store a Device definition received from a Describe response.
   * Creates the NodeEntry for addr if it does not exist.
   */
  void storeDevice(uint8_t addr, const loraiot_Device& dev);

  /**
   * Mark the Describe crawl for addr as complete with the given final rev.
   * Clears the crawlPending flag.
   */
  void markCrawlComplete(uint8_t addr, uint32_t finalRev);

  /**
   * Store a Data measurement snapshot for a specific device on a node.
   */
  void storeData(uint8_t addr, const loraiot_Data& data);

  /**
   * Record that any valid frame was received from addr: updates lastSeenMs and sets
   * online=true. Does nothing for an unknown addr (nodes are only created by a BEACON,
   * see updateNodeMeta). Returns true if the node was offline and is online again.
   */
  bool markSeen(uint8_t addr);

  /**
   * Mark addr as presumed dead. The entry and its cached definitions are kept so a node
   * that returns with the same boot_id and registry_rev needs no new Describe crawl.
   */
  void markOffline(uint8_t addr);

  /**
   * Lightweight liveness lookup (does not copy the ~2 KB NodeEntry, so it is safe on
   * small task stacks). Returns false if addr is unknown.
   */
  bool getLiveness(uint8_t addr, uint32_t& lastSeenMs, bool& online) const;

  /**
   * Last boot_id learned from a BEACON for addr. Returns false if addr is unknown or
   * no boot_id has been seen yet (DEVICE_REGISTRY_BOOT_ID_NONE).
   */
  bool getBootId(uint8_t addr, uint32_t& bootId) const;

  /**
   * Return a copy of the NodeEntry for addr. Returns false if not found.
   */
  bool getNode(uint8_t addr, NodeEntry& out) const;

  /**
   * Fill outBuf with up to maxCount node addresses that LOCAL currently tracks.
   * Returns the actual count written.
   */
  uint8_t listNodes(uint8_t* outBuf, uint8_t maxCount) const;

  /**
   * Return a copy of the CachedDevice for the given addr+device index.
   * Returns false if not found.
   */
  bool getDevice(uint8_t addr, uint8_t deviceIdx, CachedDevice& out) const;

  /**
   * Return the number of registered nodes.
   */
  uint8_t nodeCount() const;

 private:
  NodeEntry nodes[DEVICE_REGISTRY_MAX_NODES];
  uint8_t   nodeCount_ = 0;
  SemaphoreHandle_t mutex = nullptr;

  /** Find the index of addr in nodes[], or -1 if absent. */
  int8_t findNode(uint8_t addr) const;

  /**
   * Find or create a NodeEntry slot for addr. When the table is full, the offline node
   * with the oldest lastSeenMs is overwritten in place. Returns -1 if the table is full
   * and every node is online. Caller must hold mutex.
   */
  int8_t findOrCreateNode(uint8_t addr);
};

/** Global singleton. Include DeviceRegistry.h and use DeviceRegistry.* anywhere on LOCAL. */
extern DeviceRegistryClass DeviceRegistry;
