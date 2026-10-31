#pragma once

#include <Arduino.h>
#include <stdio.h>
#include <WiFiClient.h>
#include <MQTT.h>

#include "../../registry/src/DeviceTable.h"

/**
 * MqttService.h - MQTT telemetry forwarding and setpoint subscription service.
 *
 * Implements UC1 (Sensor publish) and UC2 (Setpoint subscribe) on the LOCAL node.
 * Runs on a dedicated FreeRTOS task (mqttTask) on Core 1 to ensure that network
 * I/O never blocks the LoRa receive path (loraTransportTask) or the web server.
 */

// ---------------------------------------------------------------------------
// Task sizing and timing constants
// ---------------------------------------------------------------------------
// Raised from 4096 for the /set path: ArduinoJson parsing plus PeerRegistry::registerMetric()
// run on this task. Measured minimum free stack was 1744 B before; re-check GET /status
// (MqttTask freeBytesMin) after the first /set messages and tune.
static constexpr uint32_t    MQTT_TASK_STACK_SIZE       = 5120;
// Priority 0 (idle+1) - lower than the Arduino loop task (priority 1).
// WiFiClient::read() inside client.loop() can block Core 1 for up to the socket
// timeout. Keeping mqttTask below the Arduino loop task ensures applicationLoop()
// (and the boot diagnostics it drives) is never starved, even when the broker is
// immediately reachable at boot.
static constexpr UBaseType_t MQTT_TASK_PRIORITY         = 0;

// Exponential backoff for broker reconnect: 2 s -> 4 -> 8 -> ... -> 64 s cap.
static constexpr uint32_t    MQTT_BACKOFF_MIN_MS        = 2000;
static constexpr uint32_t    MQTT_BACKOFF_MAX_MS        = 64000;
static constexpr uint32_t    MQTT_IDLE_CHECK_MS         = 5000; // period to recheck config when disabled
static constexpr uint32_t    MQTT_LOOP_MS               = 100;  // period for client.loop() while connected
static constexpr uint32_t    MQTT_CONNECT_TIMEOUT_MS    = 5000; // max time given to client.connect()

// ---------------------------------------------------------------------------
// Telemetry publishing and pending-set constants
// ---------------------------------------------------------------------------
static constexpr uint8_t     MQTT_PENDING_MAX           = 12;   // LORA_MAX_PEERS (3) * DEVICE_TABLE_MAX_DEVICES (4)
static constexpr size_t      MQTT_TOPIC_MAX_LEN         = 96;   // Max topic length: <prefix>/Local/Remote_Node_<n>/<dev>/<metric>/set
static constexpr size_t      MQTT_PAYLOAD_MAX_LEN       = 32;   // Formatted decimal or verbatim text buffer
static constexpr uint8_t     MQTT_MAX_DECIMALS          = 6;    // Maximum decimal places for scaled integers

// ---------------------------------------------------------------------------
// Node topic level: "Remote_Node_<n>", n = dashboard node number = LoRa address - 1
// (REMOTE (Node 1) = address 0x02 = Remote_Node_1). See Tier.cpp.
// ---------------------------------------------------------------------------
static constexpr const char* MQTT_NODE_TOPIC_PREFIX     = "Remote_Node_";
static constexpr uint8_t     MQTT_NODE_ADDR_OFFSET      = 1;    // address = n + offset
static constexpr uint8_t     MQTT_NODE_MAX_DIGITS       = 3;    // n <= 253
static constexpr size_t      MQTT_NODE_LEVEL_MAX_LEN    = 16;   // "Remote_Node_" (12) + 3 digits + NUL

// ---------------------------------------------------------------------------
// Control setpoint subscription ("<prefix>/Local/Remote_Node_<n>/<device>/<metric>/set")
// ---------------------------------------------------------------------------
// The 256dpi client allocates one read and one write buffer of this size (default 128).
// 256 holds the largest accepted /set packet (topic <= 95 B, payload <= 95 B, ~10 B header).
// Heap cost versus the default: +256 B per buffer, +512 B total.
static constexpr int         MQTT_CLIENT_BUFFER_SIZE    = 256;
static constexpr const char* MQTT_SET_NAMESPACE         = "Local";      // command namespace under the prefix
static constexpr const char* MQTT_SET_FILTER_TAIL       = "+/+/+/set";  // node / device / metric / set
static constexpr const char* MQTT_SET_SUFFIX            = "set";
static constexpr uint8_t     MQTT_SET_LEVELS            = 4;            // node, device, metric, "set"
static constexpr int         MQTT_SET_QOS               = 0;
static constexpr size_t      MQTT_SET_PAYLOAD_MAX_LEN   = 96;           // JSON metric object incl. NUL
static constexpr uint8_t     MQTT_SET_QUEUE_LEN         = 2;            // messages copied by the callback per loop() pass (2 * 192 B static)
// A retained /set is replayed by the broker right after SUBSCRIBE. The 256dpi advanced callback
// does not pass the retained flag, so every message that arrives while subscribing or within this
// window after a successful SUBSCRIBE is discarded (non-blocking: the callback compares millis()).
static constexpr uint32_t    MQTT_SET_RETAINED_DRAIN_MS = 1000;
static constexpr uint32_t    MQTT_SET_RESUBSCRIBE_MS    = 30000;        // retry period after a failed SUBSCRIBE
static constexpr size_t      MQTT_SET_HEAD_MAX_LEN      = 40;           // "<prefix>/Local/" incl. NUL (prefix <= 31)
static constexpr size_t      MQTT_SET_FILTER_MAX_LEN    = 56;           // "<prefix>/Local/+/+/+/set" incl. NUL

// ---------------------------------------------------------------------------
// Per-peer availability constants ("<prefix>/Remote_Node_<n>/status")
// ---------------------------------------------------------------------------
static constexpr uint8_t     MQTT_PEER_STATUS_MAX       = 3;    // == LORA_MAX_PEERS (static_assert in MqttService.cpp)
static constexpr const char* MQTT_PAYLOAD_ONLINE        = "online";
static constexpr const char* MQTT_PAYLOAD_OFFLINE       = "offline";
static constexpr const char* MQTT_PEER_STATUS_SUFFIX    = "status";

// ---------------------------------------------------------------------------
// NVS key constants (namespace "sc_prefs", all keys <= 15 chars for ESP-IDF NVS limit)
// ---------------------------------------------------------------------------
static constexpr const char* MQTT_NVS_KEY_BROKER        = "mqttBroker";
static constexpr const char* MQTT_NVS_KEY_PORT          = "mqttPort";
static constexpr const char* MQTT_NVS_KEY_USER          = "mqttUser";
static constexpr const char* MQTT_NVS_KEY_PASS          = "mqttPass";
// mqttPassSet is a derived read-only API field (not an NVS key) that indicates a password is stored.
static constexpr const char* MQTT_NVS_KEY_PASS_SET      = "mqttPassSet";
static constexpr const char* MQTT_NVS_KEY_PREFIX        = "mqttPrefix";
static constexpr const char* MQTT_NVS_KEY_RETAIN        = "mqttRetain";
static constexpr const char* MQTT_NVS_KEY_DISCOVERY     = "mqttDiscovery";
static constexpr const char* MQTT_NVS_KEY_ENABLED       = "mqttEnabled";

// Reuse the same NVS namespace as all other services.
static constexpr const char* MQTT_NVS_NAMESPACE         = "sc_prefs";

// Default values
static constexpr uint16_t    MQTT_DEFAULT_PORT          = 1883;
static constexpr const char* MQTT_DEFAULT_PREFIX        = "LoRaIoT";

// Buffer sizes (include null terminator)
static constexpr size_t      MQTT_BROKER_MAX_LEN        = 64;
static constexpr size_t      MQTT_USER_MAX_LEN          = 32;
static constexpr size_t      MQTT_PASS_MAX_LEN          = 32;
static constexpr size_t      MQTT_PREFIX_MAX_LEN        = 32;
// LOCAL's own availability topic (LWT + online), a sibling of "Remote_Node_<n>".
static constexpr const char* MQTT_LOCAL_STATUS_SUFFIX   = "Local/status";
static constexpr size_t      MQTT_STATUS_TOPIC_MAX_LEN  = MQTT_PREFIX_MAX_LEN + 14; // "<prefix>/Local/status"

// ---------------------------------------------------------------------------
// Connection state
// ---------------------------------------------------------------------------
enum class MqttState : uint8_t {
  Idle         = 0,  // not active: disabled or no broker configured
  Disconnected = 1,
  Connected    = 2,
  Error        = 3   // last connect attempt failed
};

// ---------------------------------------------------------------------------
// Status snapshot - all fields read atomically under configMutex.
// Populated only by mqttTask; read by the REST handler on Core 0.
// ---------------------------------------------------------------------------
struct MqttStatus {
  MqttState state         = MqttState::Idle;
  uint32_t  stateSinceMs  = 0;   // millis() when state was last changed
  int       lastError     = 0;   // lwmqtt error code from the last failed connect
  int       lastReturnCode= 0;   // CONNACK return code when non-zero
  uint32_t  retryAtMs     = 0;   // millis() when next reconnect will be attempted (0 = N/A)
};

// ---------------------------------------------------------------------------
// Configuration struct
// ---------------------------------------------------------------------------
struct MqttConfig {
  char     broker[MQTT_BROKER_MAX_LEN];
  uint16_t port;
  char     user[MQTT_USER_MAX_LEN];
  char     pass[MQTT_PASS_MAX_LEN];
  char     prefix[MQTT_PREFIX_MAX_LEN];
  uint8_t  retain;
  uint8_t  discovery;
  uint8_t  enabled;
};

// ---------------------------------------------------------------------------
// Pending publish entry for (addr, deviceIdx) hand-off
// ---------------------------------------------------------------------------
struct MqttPendingEntry {
  uint8_t addr;
  uint8_t deviceIdx;
};

// ---------------------------------------------------------------------------
// One received /set message, copied by the MQTT callback and processed after client.loop().
// ---------------------------------------------------------------------------
struct MqttSetEntry {
  char topic[MQTT_TOPIC_MAX_LEN];
  char payload[MQTT_SET_PAYLOAD_MAX_LEN];
};

// ---------------------------------------------------------------------------
// Per-peer availability state. Unknown means "nothing requested / nothing published yet".
// ---------------------------------------------------------------------------
enum class MqttPeerState : uint8_t {
  Unknown = 0,
  Offline = 1,
  Online  = 2
};

// One slot per tracked peer. target = latest state requested by the LoRa side (latest wins),
// published = last state successfully handed to the broker. addr 0 (invalid address) = free slot.
struct MqttPeerStatusEntry {
  uint8_t       addr;
  MqttPeerState target;
  MqttPeerState published;
};

// ---------------------------------------------------------------------------
// Pure helper functions (exposed for testability)
// ---------------------------------------------------------------------------

/**
 * Sanitize one topic level (device or metric name).
 * Replaces every character not in [A-Za-z0-9_-] with '_'.
 * If in is empty or null, output is "_".
 * Guaranteed null-terminated.
 */
void mqttSanitizeTopicLevel(const char* in, char* out, size_t outLen);

/**
 * Format a scaled integer into a decimal string using pure integer arithmetic (no float).
 * Number of decimals derived from scale (scale <= 1 -> 0 decimals, 10 -> 1, 100 -> 2, etc.).
 * Scale 0 is treated as 1.
 */
void mqttFormatScaledDecimal(int32_t value, uint32_t scale, char* out, size_t outLen);

/**
 * Parse a node topic level "Remote_Node_<n>" (n = 1..253, decimal digits only) into the LoRa
 * address n + 1 (2..254). Returns false for anything else (wrong prefix, no digits, trailing
 * characters, n = 0, address above LORA_MAX_ADDR_REMOTE).
 */
bool mqttParseNodeLevel(const char* level, uint8_t& addrOut);

/**
 * Build telemetry topic "<prefix>/Remote_Node_<n>/<device>/<metric>".
 * Uses device and metric names (sanitized) if non-empty, otherwise falls back to
 * "dev<index>" and "m<id>".
 * Returns true if built successfully without truncation.
 */
bool mqttBuildTelemetryTopic(const char* prefix, uint8_t addr,
                             const DeviceSlot& devSlot, const MetricSlot& metric,
                             char* out, size_t outLen);

/**
 * Format payload for a metric. Text metrics are copied verbatim; numeric metrics
 * are formatted via mqttFormatScaledDecimal.
 * Returns true if formatted successfully without truncation.
 */
bool mqttFormatMetricPayload(const MetricSlot& metric, char* out, size_t outLen);

/**
 * Build peer availability topic "<prefix>/Remote_Node_<n>/status" (same node formatting
 * as mqttBuildTelemetryTopic). Returns true if built successfully without truncation.
 */
bool mqttBuildPeerStatusTopic(const char* prefix, uint8_t addr, char* out, size_t outLen);

// ---------------------------------------------------------------------------
// MqttServiceClass
// ---------------------------------------------------------------------------
class MqttServiceClass {
 public:
  /**
   * Initialize MQTT service and spawn mqttTask.
   * Call once from Application.cpp applicationSetup() on LOCAL tier only.
   */
  void begin();

  /**
   * Return true when the MQTT client is currently connected to the broker.
   */
  bool isConnected() const;

  /**
   * Return the current connection state (thread-safe snapshot).
   * Kept for callers that need only the state (e.g. isConnected).
   */
  MqttState getState() const;

  /**
   * Return a full status snapshot under configMutex.
   * All fields are consistent with each other; safe to call from Core 0.
   */
  MqttStatus getStatus() const;

  /**
   * Return a copy of the current configuration under the config mutex.
   */
  MqttConfig getConfig() const;

  /**
   * Overwrite the stored configuration, persist to NVS, and signal the task
   * to reconnect with the new settings.
   * Must be called from the web-server callback (Core 0).
   */
  void applyConfig(const MqttConfig& cfg);

  /**
   * Schedule a telemetry publish for (addr, deviceIdx).
   * Thread-safe, non-blocking spinlock hand-off (safe to call from loraTransportTask).
   */
  void notifyPublish(uint8_t addr, uint8_t deviceIdx);

  /**
   * Record the latest online/offline state of a peer for publication.
   * Thread-safe, non-blocking spinlock hand-off (safe to call from loraTransportTask and
   * PingTask). Only the latest state per peer is kept; an unchanged state is ignored.
   */
  void notifyPeerStatus(uint8_t addr, bool online);

 private:
  TaskHandle_t taskHandle = nullptr;
  WiFiClient   net;
  MQTTClient   client{MQTT_CLIENT_BUFFER_SIZE};

  // Config and its protecting mutex. Written from Core 0 HTTP handlers;
  // read by mqttTask on Core 1. Never access directly - use getConfig() / applyConfig().
  MqttConfig        config       = {};
  mutable SemaphoreHandle_t configMutex = nullptr;

  // Stable snapshot of the config currently used by mqttTask (task-local, no mutex needed).
  MqttConfig      activeConfig = {};

  // Pending reconnect flag: set by applyConfig() so mqttTask notices the change.
  volatile bool   reconnectRequested = false;

  // Status fields: written only by mqttTask under configMutex, read via getStatus().
  MqttStatus status;

  // Pending publish queue / set (fixed max 12 entries with de-duplication)
  MqttPendingEntry pendingEntries[MQTT_PENDING_MAX] = {};
  uint8_t          pendingCount = 0;
  portMUX_TYPE     pendingMux   = portMUX_INITIALIZER_UNLOCKED;
  uint32_t         publishDropCount = 0;

  // Per-peer availability slots (guarded by pendingMux). Survive disconnects: only the
  // latest state per peer is kept and nothing is discarded while the broker is away.
  MqttPeerStatusEntry peerStatus[MQTT_PEER_STATUS_MAX] = {};

  // /set messages: filled by the MQTT callback and drained by mqttTask, which is the SAME task
  // (the 256dpi client invokes the callback inside client.loop()), so no lock is needed.
  MqttSetEntry setQueue[MQTT_SET_QUEUE_LEN] = {};
  uint8_t      setCount      = 0;
  bool         setSubscribed = false;  // false: callback discards (not subscribed yet)
  uint32_t     setAcceptAtMs = 0;      // callback discards until millis() reaches this (retained replay)
  uint32_t     setRetryAtMs  = 0;      // millis() of the next SUBSCRIBE retry while !setSubscribed

  // NVS helpers (called from begin() and applyConfig()).
  void loadConfig();
  void saveConfig(const MqttConfig& cfg);

  // Build the "<prefix>/Local/status" LWT/online topic into buf (must be MQTT_STATUS_TOPIC_MAX_LEN).
  static void buildStatusTopic(const char* prefix, char* buf, size_t bufLen);

  // Set state and stamp stateSinceMs; must be called under configMutex.
  void setState(MqttState newState);

  // Connect with LWT configured. Returns true on success.
  bool connectBroker(const MqttConfig& cfg);

  // Drain pending telemetry publish queue and publish active metrics.
  void drainPublishQueue(const MqttConfig& cfg);

  // Publish every peer whose target state differs from the last published state.
  void drainPeerStatus(const MqttConfig& cfg);

  // After (re)connect: forget what was published (the broker's retained state may be stale)
  // and seed missing targets from the registry's current liveness.
  void resetPeerStatus();

  // Find or allocate the slot of addr; caller holds pendingMux. Returns nullptr if none free.
  MqttPeerStatusEntry* findPeerSlot(uint8_t addr);

  // Subscribe to "<prefix>/Local/+/+/+/set" and discard the retained replay. Returns true on success.
  bool subscribeSet(const MqttConfig& cfg);

  // Process (and empty) the queue filled by messageReceived().
  void drainSetQueue(const MqttConfig& cfg);

  // Parse one /set message, apply it to ControlRegistry and queue the push (PushTask).
  void processSetMessage(const MqttSetEntry& entry, const MqttConfig& cfg);

  // 256dpi advanced callback (runs inside client.loop() on mqttTask): copies only, no processing.
  static void messageReceived(MQTTClient* source, char topic[], char bytes[], int length);

  static void mqttTask(void* param);
};

extern MqttServiceClass MqttService;

/**
 * Global helper to start MQTT service (called from Application.cpp on LOCAL tier only).
 */
void mqttServiceBegin();

/**
 * Non-blocking publish notification invoked from loraTransportTask
 * when new sensor data has been stored in SensorRegistry.
 *
 * Thread-safe, non-blocking (returns immediately without waiting on mutex or broker).
 * On REMOTE nodes (where mqttTask was not created), this is a guaranteed no-op.
 */
void mqttServiceNotifyPublish(uint8_t addr, uint8_t deviceIdx);

/**
 * Non-blocking peer availability notification (LOCAL only): invoked from loraTransportTask
 * when a peer comes (back) online and from PingTask when it is marked offline.
 *
 * Thread-safe, non-blocking (spinlock hand-off, no broker I/O, no allocation). Repeated calls
 * with an unchanged state are cheap no-ops; the published state is deduplicated.
 * On REMOTE nodes (where mqttTask was not created), this is a guaranteed no-op.
 */
void mqttServiceNotifyPeerStatus(uint8_t addr, bool online);

/**
 * Convert MqttState to a human-readable string for REST responses.
 */
const char* mqttStateName(MqttState state);
