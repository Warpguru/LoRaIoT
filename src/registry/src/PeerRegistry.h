#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../lora/src/nanopb/loraiot.pb.h"
#include "DeviceTable.h"

// Maximum number of peer addresses tracked per PeerRegistry instance.
// LoRaState uses the same constant so both stay in sync.
static constexpr uint8_t LORA_MAX_PEERS = 3;

// Sentinel: no valid boot_id seen yet.
static constexpr uint32_t PEER_BOOT_ID_NONE = 0;

// Sentinel: no valid registry_rev seen yet.
static constexpr uint32_t PEER_REV_NONE = UINT32_MAX;

/**
 * PeerEntry - everything one node knows about one peer address.
 * Holds liveness scalars plus one DeviceTable (definitions + values).
 */
struct PeerEntry {
  uint8_t     addr;           ///< LoRa node address (0x01..0xFE).
  uint32_t    bootId;         ///< Last seen boot_id from a PING or BEACON frame.
  uint32_t    registryRev;    ///< Last successfully completed Describe crawl revision.
  uint32_t    pendingRev;     ///< registry_rev from the most recent BEACON or PING
                              ///<  (may differ from registryRev while a crawl is in progress).
  uint32_t    lastSeenMs;     ///< millis() of the last received frame from addr.
  bool        online;         ///< False when presumed dead (keep-alive PING failed).
                              ///<  The entry and cached definitions are kept; the slot is
                              ///<  only reused when the table is full and no online slot is free.
  bool        crawlPending;   ///< True when a Describe crawl has been requested for this peer.
  DeviceTable table;          ///< Device/metric definitions and current values for this peer.
};

/**
 * PeerRegistry - mutex-protected registry of peer entries.
 *
 * Used in two roles:
 *   Owner role  - registerMetric(addr,...) / storeValue(addr,...) create entries from REST.
 *                 Answers Describe and Read from peers using the peer's table.
 *   Cache role  - updatePeerMeta()/storeDevice()/storeData() fill entries from LoRa frames.
 *
 * Two global instances exist on both nodes:
 *   SensorRegistry  - sensor data (REMOTE owns, LOCAL caches)
 *   ControlRegistry - control data (LOCAL owns, REMOTE caches) [unused until Step 9]
 *
 * Thread safety: all public methods are protected by an internal FreeRTOS mutex.
 */
class PeerRegistry {
 public:
  /**
   * Initialize the registry and create the internal FreeRTOS mutex.
   * Call once from applicationSetup() before LoRaTransport.begin().
   */
  void begin();

  // ---- Owner functions (REST-driven, called on the owning node) ----

  /**
   * Register or update a device+metric entry for peer addr.
   * On REMOTE, addr is always LORA_DEFAULT_ADDR_LOCAL (0x01): the owner table has
   * exactly one peer key, the LOCAL node that crawls it.
   *
   * Returns true on success, false on table-full / name-too-long / payload-too-large.
   * Increments registry_rev for that peer on any schema change.
   */
  bool registerMetric(uint8_t addr, const char* deviceName, const char* metricName,
                      const char* unit, uint32_t scale,
                      uint32_t& outDeviceIdx, uint32_t& outMetricId);

  /**
   * Store a measured numeric value for an already-registered metric.
   * No-op if addr/deviceIdx/metricId are unknown.
   */
  void storeValue(uint8_t addr, uint32_t deviceIdx, uint32_t metricId,
                  int32_t scaledValue);

  /**
   * Store an opaque text value for an already-registered metric.
   * No-op if addr/deviceIdx/metricId are unknown or text is null.
   */
  void storeTextValue(uint8_t addr, uint32_t deviceIdx, uint32_t metricId,
                      const char* text);

  /**
   * Current registry_rev for peer addr.
   * Returns 0 if addr is not known.
   */
  uint32_t getRegistryRev(uint8_t addr) const;

  // ---- Cache functions (LoRa-driven, called on the caching node) ----

  /**
   * Update metadata (boot_id, registry_rev) for a peer seen via BEACON or PING.
   * If boot_id changed the device cache for that peer is cleared and a crawl is
   * scheduled. If registry_rev changed a crawl is scheduled.
   * Returns true if a Describe crawl should be started for addr.
   */
  bool updatePeerMeta(uint8_t addr, uint32_t bootId,
                      bool hasRegistryRev, uint32_t registryRev);

  /**
   * Store a Device definition received from a Describe response.
   * Creates the PeerEntry for addr if it does not exist.
   */
  void storeDevice(uint8_t addr, const loraiot_Device& dev);

  /**
   * Mark the Describe crawl for addr complete with the given final rev.
   * Clears the crawlPending flag.
   */
  void markCrawlComplete(uint8_t addr, uint32_t finalRev);

  /**
   * Apply a DATA frame. Partial update: only named metrics are changed.
   * Returns OK when every metric was applied, UNKNOWN_METRIC when the device is known
   * but at least one metric_id is not (the known ones were still applied), and
   * UNKNOWN_DEVICE when the peer or device is unknown. Anything but OK means the
   * cached schema is stale and the caller should request a Describe crawl.
   */
  DeviceTableResult storeData(uint8_t addr, const loraiot_Data& data);

  // ---- Liveness (called by PingTask, Application) ----

  /**
   * Record that any valid frame was received from addr: updates lastSeenMs and sets
   * online=true. Does nothing for an unknown addr (peers are only created by a BEACON
   * or an owner registration). Returns true if the peer was offline and is online again.
   */
  bool markSeen(uint8_t addr);

  /**
   * Mark addr as presumed dead. The entry and its cached definitions are kept so a peer
   * that returns with the same boot_id and registry_rev needs no new Describe crawl.
   */
  void markOffline(uint8_t addr);

  /**
   * Lightweight liveness lookup (does not copy the PeerEntry, so it is safe on small
   * task stacks). Returns false if addr is unknown.
   */
  bool getLiveness(uint8_t addr, uint32_t& lastSeenMs, bool& online) const;

  /**
   * Last boot_id learned from a BEACON for addr. Returns false if addr is unknown or
   * no boot_id has been seen yet (PEER_BOOT_ID_NONE).
   */
  bool getBootId(uint8_t addr, uint32_t& bootId) const;

  // ---- Query (called by RestService, DescribeCrawler) ----

  /** Lightweight peer info - only scalars, no table copy. Returns false if addr unknown. */
  bool    getNodeInfo(uint8_t addr, uint32_t& bootId, uint32_t& registryRev,
                      uint32_t& lastSeenMs, bool& online, bool& crawlPending) const;

  /** Copy one DeviceSlot. Returns false if addr or deviceIdx unknown. */
  bool    getDeviceSlot(uint8_t addr, uint8_t deviceIdx, DeviceSlot& out) const;

  /** Read crawlPending scalar for addr. Returns false if addr unknown. */
  bool    isCrawlPending(uint8_t addr) const;

  /**
   * Force a Describe crawl of addr by setting its crawlPending flag (REST ?refresh=true).
   * Only sets the flag: the crawl itself is started later by describeCrawlerLoop(), so this
   * never blocks on the LoRa link and is safe to call from a REST handler. The cached
   * definitions are kept and overwritten by the crawl; registryRev is not reset. Calling it
   * while a crawl is already pending is a no-op. Returns false (and creates nothing) if addr
   * is unknown.
   */
  bool    requestCrawl(uint8_t addr);

  /** Read pendingRev scalar for addr. Returns false if addr unknown. */
  bool    getPendingRev(uint8_t addr, uint32_t& pendingRev) const;

  /** Read device count for addr. Returns 0 if addr unknown. */
  uint8_t getDeviceCount(uint8_t addr) const;

  /** Fill outBuf with up to maxCount tracked peer addresses. Returns the count written. */
  uint8_t listPeers(uint8_t* outBuf, uint8_t maxCount) const;

  /** Return the number of tracked peers. */
  uint8_t peerCount() const;

  // ---- Proto helpers (owner role) ----

  /** Fill a loraiot_Device from the stored definition. Returns false if not found. */
  bool toProtoDevice(uint8_t addr, uint32_t deviceIdx, loraiot_Device& out) const;

  /** Fill a loraiot_Data with the most recent values. Returns false if not found. */
  bool toProtoData(uint8_t addr, uint32_t deviceIdx, loraiot_Data& out) const;

 private:
  PeerEntry         peers[LORA_MAX_PEERS];
  uint8_t           peerCount_ = 0;
  SemaphoreHandle_t mutex      = nullptr;

  /** Find the index of addr in peers[], or -1 if absent. */
  int8_t findPeer(uint8_t addr) const;

  /**
   * Find or create a PeerEntry slot for addr. When the table is full, the offline peer
   * with the oldest lastSeenMs is overwritten in place. Returns -1 if the table is full
   * and every peer is online. Caller must hold mutex.
   */
  int8_t findOrCreatePeer(uint8_t addr);
};

/** Global instances. Sensor data: REMOTE owns, LOCAL caches. */
extern PeerRegistry SensorRegistry;

/** Control data: LOCAL owns, REMOTE caches. Unused until Step 9. */
extern PeerRegistry ControlRegistry;
