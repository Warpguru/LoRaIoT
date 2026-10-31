#include "MqttService.h"

#include <stdio.h>
#include <string.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "ControlSetpoint.h"
#include "../../diagnostics/src/TaskRegistry.h"
#include "../../LoRaIoT/src/LoRaConfig.h"
#include "../../LoRaIoT/src/PingTask.h"
#include "../../LoRaIoT/src/PushTask.h"
#include "../../registry/src/PeerRegistry.h"
#include "../../util/src/LogBuffer.h"
#include "../../util/src/TaskUtils.h"

MqttServiceClass MqttService;

// The per-peer availability table must cover every peer the registry can track.
static_assert(MQTT_PEER_STATUS_MAX == LORA_MAX_PEERS, "MQTT_PEER_STATUS_MAX must equal LORA_MAX_PEERS");

// Scratch DeviceSlot (about 470 B) shared by the telemetry drain and the /set name resolution.
// Both run only on mqttTask, never at the same time, so one static copy is enough.
static DeviceSlot slotScratch;

// Dashboard node number used in topics: n = LoRa address - MQTT_NODE_ADDR_OFFSET.
static uint8_t mqttNodeNumber(uint8_t addr) {
  return (addr >= MQTT_NODE_ADDR_OFFSET) ? static_cast<uint8_t>(addr - MQTT_NODE_ADDR_OFFSET) : 0;
}

// ---------------------------------------------------------------------------
// Pure helper functions
// ---------------------------------------------------------------------------
void mqttSanitizeTopicLevel(const char* in, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  if (in == nullptr || in[0] == '\0') {
    out[0] = '_';
    if (outLen > 1) {
      out[1] = '\0';
    } else {
      out[0] = '\0';
    }
    return;
  }

  size_t w = 0;
  for (size_t r = 0; in[r] != '\0' && (w + 1) < outLen; ++r) {
    char c = in[r];
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-') {
      out[w++] = c;
    } else {
      out[w++] = '_';
    }
  }

  if (w == 0) {
    if (outLen > 1) {
      out[0] = '_';
      out[1] = '\0';
    } else {
      out[0] = '\0';
    }
  } else {
    out[w] = '\0';
  }
}

void mqttFormatScaledDecimal(int32_t value, uint32_t scale, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }

  uint8_t decimals = 0;
  if (scale == 10) {
    decimals = 1;
  } else if (scale == 100) {
    decimals = 2;
  } else if (scale == 1000) {
    decimals = 3;
  } else if (scale == 10000) {
    decimals = 4;
  } else if (scale == 100000) {
    decimals = 5;
  } else if (scale >= 1000000) {
    decimals = 6;
    scale = 1000000;
  } else {
    scale = 1; // scale <= 1 or scale == 0 (verbatim integer)
  }

  int64_t v = value;
  bool negative = (v < 0);
  uint64_t absV = negative ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);

  uint64_t intPart = absV / scale;
  uint64_t fracPart = absV % scale;

  const char* sign = (negative && (intPart != 0 || fracPart != 0)) ? "-" : "";

  if (decimals == 0) {
    snprintf(out, outLen, "%s%llu", sign, (unsigned long long)intPart);
  } else {
    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%s%%llu.%%0%ullu", (unsigned)decimals);
    snprintf(out, outLen, fmt, sign, (unsigned long long)intPart, (unsigned long long)fracPart);
  }
}

bool mqttParseNodeLevel(const char* level, uint8_t& addrOut) {
  if (level == nullptr) {
    return false;
  }
  const size_t prefixLen = strlen(MQTT_NODE_TOPIC_PREFIX);
  if (strncmp(level, MQTT_NODE_TOPIC_PREFIX, prefixLen) != 0) {
    return false;
  }

  const char* digits = level + prefixLen;
  uint16_t number = 0;
  uint8_t count = 0;
  for (; digits[count] != '\0'; ++count) {
    if (digits[count] < '0' || digits[count] > '9' || count >= MQTT_NODE_MAX_DIGITS) {
      return false;
    }
    number = static_cast<uint16_t>(number * 10 + (digits[count] - '0'));
  }
  // No digits, n = 0, or a leading zero (would alias another node level).
  if (count == 0 || number == 0 || (count > 1 && digits[0] == '0')) {
    return false;
  }

  const uint16_t addr = static_cast<uint16_t>(number + MQTT_NODE_ADDR_OFFSET);
  if (addr < LORA_DEFAULT_ADDR_REMOTE || addr > LORA_MAX_ADDR_REMOTE) {
    return false;
  }
  addrOut = static_cast<uint8_t>(addr);
  return true;
}

bool mqttBuildTelemetryTopic(const char* prefix, uint8_t addr,
                             const DeviceSlot& devSlot, const MetricSlot& metric,
                             char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return false;
  }

  const char* pfx = (prefix != nullptr && prefix[0] != '\0') ? prefix : MQTT_DEFAULT_PREFIX;

  char devName[DEVICE_TABLE_NAME_LEN + 2];
  if (devSlot.name[0] != '\0') {
    mqttSanitizeTopicLevel(devSlot.name, devName, sizeof(devName));
  } else {
    snprintf(devName, sizeof(devName), "dev%u", (unsigned)devSlot.index);
  }

  char metricName[DEVICE_TABLE_NAME_LEN + 2];
  if (metric.name[0] != '\0') {
    mqttSanitizeTopicLevel(metric.name, metricName, sizeof(metricName));
  } else {
    snprintf(metricName, sizeof(metricName), "m%u", (unsigned)metric.id);
  }

  int written = snprintf(out, outLen, "%s/%s%u/%s/%s", pfx, MQTT_NODE_TOPIC_PREFIX,
                         (unsigned)mqttNodeNumber(addr), devName, metricName);
  return (written > 0 && static_cast<size_t>(written) < outLen);
}

bool mqttBuildPeerStatusTopic(const char* prefix, uint8_t addr, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return false;
  }

  const char* pfx = (prefix != nullptr && prefix[0] != '\0') ? prefix : MQTT_DEFAULT_PREFIX;
  int written = snprintf(out, outLen, "%s/%s%u/%s", pfx, MQTT_NODE_TOPIC_PREFIX,
                         (unsigned)mqttNodeNumber(addr), MQTT_PEER_STATUS_SUFFIX);
  return (written > 0 && static_cast<size_t>(written) < outLen);
}

bool mqttFormatMetricPayload(const MetricSlot& metric, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0 || !metric.hasValue) {
    return false;
  }

  if (metric.isText) {
    int written = snprintf(out, outLen, "%s", metric.text);
    return (written >= 0 && static_cast<size_t>(written) < outLen);
  } else {
    mqttFormatScaledDecimal(metric.value, metric.scale, out, outLen);
    return true;
  }
}

// ---------------------------------------------------------------------------
// mqttStateName
// ---------------------------------------------------------------------------
const char* mqttStateName(MqttState s) {
  switch (s) {
    case MqttState::Idle:         return "idle";
    case MqttState::Disconnected: return "disconnected";
    case MqttState::Connected:    return "connected";
    case MqttState::Error:        return "error";
    default:                      return "idle";
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void mqttServiceBegin() {
  MqttService.begin();
}

void mqttServiceNotifyPublish(uint8_t addr, uint8_t deviceIdx) {
  MqttService.notifyPublish(addr, deviceIdx);
}

void MqttServiceClass::notifyPublish(uint8_t addr, uint8_t deviceIdx) {
  if (taskHandle == nullptr) {
    return;
  }

  bool notify = false;
  portENTER_CRITICAL(&pendingMux);
  bool found = false;
  for (uint8_t i = 0; i < pendingCount; ++i) {
    if (pendingEntries[i].addr == addr && pendingEntries[i].deviceIdx == deviceIdx) {
      found = true;
      break;
    }
  }

  if (!found) {
    if (pendingCount < MQTT_PENDING_MAX) {
      pendingEntries[pendingCount].addr = addr;
      pendingEntries[pendingCount].deviceIdx = deviceIdx;
      pendingCount++;
      notify = true;
    } else {
      publishDropCount++;
    }
  }
  portEXIT_CRITICAL(&pendingMux);

  if (notify && taskHandle != nullptr) {
    xTaskNotifyGive(taskHandle);
  }
}

void mqttServiceNotifyPeerStatus(uint8_t addr, bool online) {
  MqttService.notifyPeerStatus(addr, online);
}

// Caller holds pendingMux (no logging, no blocking calls in here).
// Returns the slot of addr; otherwise a free slot, otherwise a slot with nothing pending
// (target == published) is recycled. Returns nullptr when every slot has a pending change.
MqttPeerStatusEntry* MqttServiceClass::findPeerSlot(uint8_t addr) {
  MqttPeerStatusEntry* spare    = nullptr;
  MqttPeerStatusEntry* reusable = nullptr;
  for (uint8_t i = 0; i < MQTT_PEER_STATUS_MAX; ++i) {
    MqttPeerStatusEntry* entry = &peerStatus[i];
    if (entry->addr == addr) {
      return entry;
    }
    if (entry->addr == 0) {
      if (spare == nullptr) {
        spare = entry;
      }
    } else if (reusable == nullptr && entry->target == entry->published) {
      reusable = entry;
    }
  }

  MqttPeerStatusEntry* slot = (spare != nullptr) ? spare : reusable;
  if (slot != nullptr) {
    slot->addr      = addr;
    slot->target    = MqttPeerState::Unknown;
    slot->published = MqttPeerState::Unknown;
  }
  return slot;
}

void MqttServiceClass::notifyPeerStatus(uint8_t addr, bool online) {
  if (taskHandle == nullptr || addr == 0) {
    return;
  }

  const MqttPeerState state = online ? MqttPeerState::Online : MqttPeerState::Offline;
  bool notify = false;

  portENTER_CRITICAL(&pendingMux);
  MqttPeerStatusEntry* slot = findPeerSlot(addr);
  if (slot != nullptr) {
    slot->target = state;                      // latest state wins
    notify = (state != slot->published);       // nothing to do if the broker already has it
  } else {
    publishDropCount++;
  }
  portEXIT_CRITICAL(&pendingMux);

  if (notify) {
    xTaskNotifyGive(taskHandle);
  }
}

void MqttServiceClass::begin() {
  if (taskHandle != nullptr) {
    Log_warn("MqttService::begin called when task is already running");
    return;
  }

  // Create the config mutex before spawning the task.
  configMutex = xSemaphoreCreateMutex();
  if (configMutex == nullptr) {
    Log_error("MqttService: failed to create config mutex");
    return;
  }

  // Load persisted config from NVS before the task starts.
  loadConfig();

  // setTimeout() takes milliseconds. Pass MQTT_CONNECT_TIMEOUT_MS directly.
  // The actual hostname / port are passed in connectBroker(), not here.
  client.setOptions(/*keepAlive=*/15, /*cleanSession=*/true,
                    /*timeout=*/MQTT_CONNECT_TIMEOUT_MS);

  // Advanced callback for /set messages: invoked inside client.loop() on mqttTask.
  client.onMessageAdvanced(messageReceived);

  TASK_CREATE(mqttTask, "MqttTask", MQTT_TASK_STACK_SIZE, this, MQTT_TASK_PRIORITY, &taskHandle);
  if (taskHandle != nullptr) {
    TaskRegistry.add("MqttTask", taskHandle, MQTT_TASK_STACK_SIZE);
    Log_info("MqttService: Task created (stack=%u, priority=%u)", MQTT_TASK_STACK_SIZE, MQTT_TASK_PRIORITY);
  } else {
    Log_error("MqttService: Failed to create mqttTask");
  }
}

bool MqttServiceClass::isConnected() const {
  return (status.state == MqttState::Connected);
}

MqttState MqttServiceClass::getState() const {
  return status.state;
}

MqttStatus MqttServiceClass::getStatus() const {
  MqttStatus snap = {};
  if (configMutex != nullptr && xSemaphoreTake(configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snap = status;
    xSemaphoreGive(configMutex);
  }
  return snap;
}

MqttConfig MqttServiceClass::getConfig() const {
  MqttConfig copy = {};
  if (configMutex != nullptr && xSemaphoreTake(configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    copy = config;
    xSemaphoreGive(configMutex);
  }
  return copy;
}

void MqttServiceClass::applyConfig(const MqttConfig& cfg) {
  if (configMutex != nullptr && xSemaphoreTake(configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    config = cfg;
    reconnectRequested = true;
    xSemaphoreGive(configMutex);
  }
  saveConfig(cfg);
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
void MqttServiceClass::loadConfig() {
  // Apply defaults first so fields not stored in NVS get sensible values.
  memset(&config, 0, sizeof(config));
  config.port      = MQTT_DEFAULT_PORT;
  config.retain    = 1;
  config.discovery = 0;
  config.enabled   = 0;
  strncpy(config.prefix, MQTT_DEFAULT_PREFIX, sizeof(config.prefix) - 1);

  Preferences prefs;
  if (prefs.begin(MQTT_NVS_NAMESPACE, /*readOnly=*/true)) {
    // broker
    String val = prefs.getString(MQTT_NVS_KEY_BROKER, "");
    if (val.length() > 0 && val.length() < MQTT_BROKER_MAX_LEN) {
      strncpy(config.broker, val.c_str(), sizeof(config.broker) - 1);
    }
    // port
    config.port = prefs.getUShort(MQTT_NVS_KEY_PORT, MQTT_DEFAULT_PORT);
    if (config.port == 0) {
      config.port = MQTT_DEFAULT_PORT;
    }
    // user
    val = prefs.getString(MQTT_NVS_KEY_USER, "");
    if (val.length() > 0 && val.length() < MQTT_USER_MAX_LEN) {
      strncpy(config.user, val.c_str(), sizeof(config.user) - 1);
    }
    // pass
    val = prefs.getString(MQTT_NVS_KEY_PASS, "");
    if (val.length() > 0 && val.length() < MQTT_PASS_MAX_LEN) {
      strncpy(config.pass, val.c_str(), sizeof(config.pass) - 1);
    }
    // prefix
    val = prefs.getString(MQTT_NVS_KEY_PREFIX, MQTT_DEFAULT_PREFIX);
    if (val.length() > 0 && val.length() < MQTT_PREFIX_MAX_LEN) {
      strncpy(config.prefix, val.c_str(), sizeof(config.prefix) - 1);
    }
    // retain, discovery, enabled
    config.retain    = prefs.getUChar(MQTT_NVS_KEY_RETAIN,    1);
    config.discovery = prefs.getUChar(MQTT_NVS_KEY_DISCOVERY, 0);
    config.enabled   = prefs.getUChar(MQTT_NVS_KEY_ENABLED,   0);

    prefs.end();
    // Note: password is never logged.
    Log_info("MqttService: config loaded - broker='%s' port=%u user='%s' prefix='%s' enabled=%u",
             config.broker, config.port, config.user, config.prefix, config.enabled);
  } else {
    Log_info("MqttService: using default config (no NVS entry)");
  }
}

void MqttServiceClass::saveConfig(const MqttConfig& cfg) {
  Preferences prefs;
  if (prefs.begin(MQTT_NVS_NAMESPACE, /*readOnly=*/false)) {
    prefs.putString(MQTT_NVS_KEY_BROKER,    cfg.broker);
    prefs.putUShort(MQTT_NVS_KEY_PORT,      cfg.port);
    prefs.putString(MQTT_NVS_KEY_USER,      cfg.user);
    prefs.putString(MQTT_NVS_KEY_PASS,      cfg.pass);
    prefs.putString(MQTT_NVS_KEY_PREFIX,    cfg.prefix);
    prefs.putUChar (MQTT_NVS_KEY_RETAIN,    cfg.retain);
    prefs.putUChar (MQTT_NVS_KEY_DISCOVERY, cfg.discovery);
    prefs.putUChar (MQTT_NVS_KEY_ENABLED,   cfg.enabled);
    prefs.end();
    // Note: password is never logged.
    Log_info("MqttService: config saved - broker='%s' port=%u prefix='%s' enabled=%u",
             cfg.broker, cfg.port, cfg.prefix, cfg.enabled);
  } else {
    Log_error("MqttService: save failed - could not open NVS namespace '%s'", MQTT_NVS_NAMESPACE);
  }
}

// ---------------------------------------------------------------------------
// Private state helpers
// ---------------------------------------------------------------------------

// Must be called under configMutex.
void MqttServiceClass::setState(MqttState newState) {
  status.state        = newState;
  status.stateSinceMs = millis();
}

// ---------------------------------------------------------------------------
// Connection helpers
// ---------------------------------------------------------------------------
void MqttServiceClass::buildStatusTopic(const char* prefix, char* buf, size_t bufLen) {
  snprintf(buf, bufLen, "%s/%s", prefix, MQTT_LOCAL_STATUS_SUFFIX);
}

bool MqttServiceClass::connectBroker(const MqttConfig& cfg) {
  char statusTopic[MQTT_STATUS_TOPIC_MAX_LEN];
  buildStatusTopic(cfg.prefix, statusTopic, sizeof(statusTopic));

  // Configure LWT before connect so the broker knows to publish "offline"
  // when the TCP connection drops unexpectedly.
  client.setWill(statusTopic, "offline", /*retained=*/true, /*qos=*/0);

  // Associate the transport with this connect attempt.
  client.begin(cfg.broker, cfg.port, net);

  const char* user = (cfg.user[0] != '\0') ? cfg.user  : nullptr;
  const char* pass = (cfg.pass[0] != '\0') ? cfg.pass  : nullptr;

  Log_info("MqttService: connecting to broker '%s:%u' (user='%s')...",
           cfg.broker, cfg.port, cfg.user[0] ? cfg.user : "(none)");

  // client.connect() is blocking up to the timeout set in begin().
  // Use a static client ID derived from the chip MAC to survive broker restarts.
  char clientId[24];
  snprintf(clientId, sizeof(clientId), "LoRaIoT-%06X",
           (unsigned)(ESP.getEfuseMac() & 0xFFFFFFULL));

  // A new session starts: nothing is subscribed and no /set message may be pending.
  setSubscribed = false;
  setCount      = 0;
  setRetryAtMs  = 0;

  bool ok = client.connect(clientId, user, pass);

  if (!ok) {
    Log_warn("MqttService: connect failed (rc=%d)", client.lastError());
    return false;
  }

  // Publish "online" status with retain so new subscribers see the state immediately.
  bool retained = (cfg.retain != 0);
  client.publish(statusTopic, "online", retained, /*qos=*/0);

  // The broker's retained per-peer state may be stale: republish all peers from the loop.
  resetPeerStatus();

  // Listen for setpoints. A failure keeps the connection (telemetry still works) and is
  // retried from the connected loop every MQTT_SET_RESUBSCRIBE_MS. A prefix change reconnects
  // through this same path, so the filter always follows the active prefix.
  subscribeSet(cfg);

  Log_info("MqttService: connected, LWT topic='%s'", statusTopic);
  return true;
}

// ---------------------------------------------------------------------------
// Per-peer availability
// ---------------------------------------------------------------------------
void MqttServiceClass::resetPeerStatus() {
  // Forget what was published so every known peer is sent again.
  portENTER_CRITICAL(&pendingMux);
  for (uint8_t i = 0; i < MQTT_PEER_STATUS_MAX; ++i) {
    peerStatus[i].published = MqttPeerState::Unknown;
  }
  portEXIT_CRITICAL(&pendingMux);

  // Make sure every peer already in the registry has a slot (e.g. state reached before
  // MQTT was up and the slot was recycled). The state itself is resolved at publish time.
  uint8_t addrs[LORA_MAX_PEERS];
  const uint8_t count = SensorRegistry.listPeers(addrs, LORA_MAX_PEERS);
  for (uint8_t i = 0; i < count; ++i) {
    uint32_t lastSeenMs = 0;
    bool     online     = false;
    if (!SensorRegistry.getLiveness(addrs[i], lastSeenMs, online)) {
      continue;
    }
    portENTER_CRITICAL(&pendingMux);
    MqttPeerStatusEntry* slot = findPeerSlot(addrs[i]);
    if (slot != nullptr && slot->target == MqttPeerState::Unknown) {
      slot->target = online ? MqttPeerState::Online : MqttPeerState::Offline;
    }
    portEXIT_CRITICAL(&pendingMux);
  }
}

void MqttServiceClass::drainPeerStatus(const MqttConfig& cfg) {
  struct Todo {
    uint8_t       addr;
    MqttPeerState target;
    MqttPeerState published;
  };
  Todo    todo[MQTT_PEER_STATUS_MAX];
  uint8_t todoCount = 0;

  portENTER_CRITICAL(&pendingMux);
  for (uint8_t i = 0; i < MQTT_PEER_STATUS_MAX; ++i) {
    const MqttPeerStatusEntry& entry = peerStatus[i];
    if (entry.addr != 0 && entry.target != MqttPeerState::Unknown && entry.target != entry.published) {
      todo[todoCount].addr      = entry.addr;
      todo[todoCount].target    = entry.target;
      todo[todoCount].published = entry.published;
      todoCount++;
    }
  }
  portEXIT_CRITICAL(&pendingMux);

  for (uint8_t i = 0; i < todoCount; ++i) {
    // The registry is the source of truth: it also covers a notification that was overtaken
    // by a newer state change from the other task. The notification only triggers the publish.
    uint32_t lastSeenMs = 0;
    bool     online     = false;
    if (!SensorRegistry.getLiveness(todo[i].addr, lastSeenMs, online)) {
      // The registry evicted this peer: free the slot, there is no state to publish.
      portENTER_CRITICAL(&pendingMux);
      for (uint8_t s = 0; s < MQTT_PEER_STATUS_MAX; ++s) {
        if (peerStatus[s].addr == todo[i].addr) {
          peerStatus[s] = {};
          break;
        }
      }
      portEXIT_CRITICAL(&pendingMux);
      continue;
    }
    const MqttPeerState state = online ? MqttPeerState::Online : MqttPeerState::Offline;

    if (state != todo[i].published) {
      // Topic length is bounded (prefix <= 31 chars + "/Remote_Node_253/status"), so it always fits.
      char topic[MQTT_TOPIC_MAX_LEN];
      mqttBuildPeerStatusTopic(cfg.prefix, todo[i].addr, topic, sizeof(topic));

      const char* payload = (state == MqttPeerState::Online) ? MQTT_PAYLOAD_ONLINE : MQTT_PAYLOAD_OFFLINE;
      if (!client.publish(topic, payload, cfg.retain != 0, /*qos=*/0)) {
        // Keep the slot pending: it is retried on the next loop pass, or republished
        // after the reconnect when the connection dropped.
        Log_warn("MqttService: publish failed for %s (rc=%d)", topic, client.lastError());
        if (!client.connected()) {
          return;
        }
        continue;
      }
      Log_info("MqttService: %s = %s", topic, payload);
    }

    // Record the outcome. A newer target set meanwhile by notifyPeerStatus() is kept.
    portENTER_CRITICAL(&pendingMux);
    for (uint8_t s = 0; s < MQTT_PEER_STATUS_MAX; ++s) {
      if (peerStatus[s].addr == todo[i].addr) {
        if (peerStatus[s].target == todo[i].target) {
          peerStatus[s].target = state;
        }
        peerStatus[s].published = state;
        break;
      }
    }
    portEXIT_CRITICAL(&pendingMux);
  }
}

// ---------------------------------------------------------------------------
// mqttTask - broker connection loop
// ---------------------------------------------------------------------------
void MqttServiceClass::drainPublishQueue(const MqttConfig& cfg) {
  MqttPendingEntry batch[MQTT_PENDING_MAX];
  uint8_t batchCount = 0;

  portENTER_CRITICAL(&pendingMux);
  batchCount = pendingCount;
  if (batchCount > 0) {
    memcpy(batch, pendingEntries, batchCount * sizeof(MqttPendingEntry));
    pendingCount = 0;
  }
  portEXIT_CRITICAL(&pendingMux);

  if (batchCount == 0) {
    return;
  }

  // Static snapshot buffer for single task owner to avoid stack consumption
  DeviceSlot& devSlot = slotScratch;

  bool publishWarnLogged = false;

  for (uint8_t i = 0; i < batchCount; ++i) {
    const MqttPendingEntry& entry = batch[i];

    if (!SensorRegistry.getDeviceSlot(entry.addr, entry.deviceIdx, devSlot)) {
      continue;
    }

    for (uint8_t m = 0; m < devSlot.metricCount; ++m) {
      const MetricSlot& metric = devSlot.metrics[m];
      if (!metric.hasValue) {
        continue;
      }

      char topic[MQTT_TOPIC_MAX_LEN];
      if (!mqttBuildTelemetryTopic(cfg.prefix, entry.addr, devSlot, metric, topic, sizeof(topic))) {
        if (!publishWarnLogged) {
          Log_warn("MqttService: topic truncated for node 0x%02X dev %u metric %u",
                   entry.addr, entry.deviceIdx, metric.id);
          publishWarnLogged = true;
        }
        continue;
      }

      char payload[MQTT_PAYLOAD_MAX_LEN];
      if (!mqttFormatMetricPayload(metric, payload, sizeof(payload))) {
        if (!publishWarnLogged) {
          Log_warn("MqttService: payload format failed for %s", topic);
          publishWarnLogged = true;
        }
        continue;
      }

      const bool retain = (cfg.retain != 0);
      const bool ok = client.publish(topic, payload, retain, /*qos=*/0);

      if (!ok) {
        if (!publishWarnLogged) {
          Log_warn("MqttService: publish failed for %s (rc=%d)", topic, client.lastError());
          publishWarnLogged = true;
        }
        if (!client.connected()) {
          return;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Control setpoints: "<prefix>/Local/Remote_Node_<n>/<device>/<metric>/set"
// ---------------------------------------------------------------------------

// Copy the next '/'-separated level of *cursor into out and advance the cursor past the
// separator. Sets last when no '/' followed. Returns false for an empty level or one that
// does not fit into out (outLen includes the NUL).
static bool mqttNextLevel(const char*& cursor, char* out, size_t outLen, bool& last) {
  const char* end = strchr(cursor, '/');
  last = (end == nullptr);
  const size_t len = last ? strlen(cursor) : static_cast<size_t>(end - cursor);
  if (len == 0 || len >= outLen) {
    return false;
  }
  memcpy(out, cursor, len);
  out[len] = '\0';
  cursor = last ? (cursor + len) : (end + 1);
  return true;
}

// True if the sanitized form of name equals level. This is how a topic level is mapped back to
// a registered name: telemetry publishes the sanitized name, so the round trip is exact.
static bool mqttLevelMatches(const char* level, const char* name) {
  char sanitized[DEVICE_TABLE_NAME_LEN + 2];
  mqttSanitizeTopicLevel(name, sanitized, sizeof(sanitized));
  return strcmp(level, sanitized) == 0;
}

bool MqttServiceClass::subscribeSet(const MqttConfig& cfg) {
  char filter[MQTT_SET_FILTER_MAX_LEN];
  snprintf(filter, sizeof(filter), "%s/%s/%s", cfg.prefix, MQTT_SET_NAMESPACE, MQTT_SET_FILTER_TAIL);

  // The callback discards everything until the SUBSCRIBE succeeded and the retained replay passed.
  setSubscribed = false;
  if (!client.subscribe(filter, MQTT_SET_QOS)) {
    setRetryAtMs = millis() + MQTT_SET_RESUBSCRIBE_MS;
    Log_warn("MqttService: subscribe '%s' failed (rc=%d), retry in %u ms",
             filter, client.lastError(), MQTT_SET_RESUBSCRIBE_MS);
    return false;
  }

  setAcceptAtMs = millis() + MQTT_SET_RETAINED_DRAIN_MS;
  setSubscribed = true;
  Log_info("MqttService: subscribed to '%s'", filter);
  return true;
}

// Runs inside client.loop() on mqttTask: only copies into the fixed queue (no heap, no
// registry access, no LoRa, no blocking). Processing happens in drainSetQueue().
void MqttServiceClass::messageReceived(MQTTClient* /*source*/, char topic[], char bytes[], int length) {
  MqttServiceClass& self = MqttService;

  // Not subscribed yet, or inside the window in which the broker replays retained messages.
  // The 256dpi advanced callback does not expose the retained flag, hence the time window.
  if (!self.setSubscribed || static_cast<int32_t>(millis() - self.setAcceptAtMs) < 0) {
    Log_debug("MqttService: message on '%s' ignored (retained replay window)", topic);
    return;
  }

  if (length < 0 || static_cast<size_t>(length) >= MQTT_SET_PAYLOAD_MAX_LEN ||
      strlen(topic) >= MQTT_TOPIC_MAX_LEN) {
    Log_warn("MqttService: /set message dropped (topic or payload too long, payload %d B, max %u B)",
             length, MQTT_SET_PAYLOAD_MAX_LEN - 1);
    return;
  }

  if (self.setCount >= MQTT_SET_QUEUE_LEN) {
    Log_warn("MqttService: /set queue full, message dropped");
    return;
  }

  MqttSetEntry& entry = self.setQueue[self.setCount++];
  snprintf(entry.topic, sizeof(entry.topic), "%s", topic);
  memcpy(entry.payload, bytes, static_cast<size_t>(length));
  entry.payload[length] = '\0';
}

void MqttServiceClass::drainSetQueue(const MqttConfig& cfg) {
  for (uint8_t i = 0; i < setCount; ++i) {
    processSetMessage(setQueue[i], cfg);
  }
  setCount = 0;
}

void MqttServiceClass::processSetMessage(const MqttSetEntry& entry, const MqttConfig& cfg) {
  // 1. Strip "<prefix>/Local/".
  char head[MQTT_SET_HEAD_MAX_LEN];
  snprintf(head, sizeof(head), "%s/%s/", cfg.prefix, MQTT_SET_NAMESPACE);
  const size_t headLen = strlen(head);
  if (strncmp(entry.topic, head, headLen) != 0) {
    Log_warn("MqttService: /set topic '%s' is outside '%s', ignored", entry.topic, head);
    return;
  }

  // 2. Split "Remote_Node_<n>/<device>/<metric>/set". Names longer than the registry allows
  //    (DEVICE_TABLE_NAME_LEN - 1 chars) can never match or be registered.
  char nodeLevel[MQTT_NODE_LEVEL_MAX_LEN];
  char deviceLevel[DEVICE_TABLE_NAME_LEN];
  char metricLevel[DEVICE_TABLE_NAME_LEN];
  const char* cursor = entry.topic + headLen;
  bool last = false;
  if (!mqttNextLevel(cursor, nodeLevel, sizeof(nodeLevel), last) || last ||
      !mqttNextLevel(cursor, deviceLevel, sizeof(deviceLevel), last) || last ||
      !mqttNextLevel(cursor, metricLevel, sizeof(metricLevel), last) || last ||
      strcmp(cursor, MQTT_SET_SUFFIX) != 0) {
    Log_error("MqttService: /set topic '%s' malformed (expected %sRemote_Node_<n>/<device>/<metric>/%s, names max %u chars)",
              entry.topic, head, MQTT_SET_SUFFIX, (unsigned)(DEVICE_TABLE_NAME_LEN - 1));
    return;
  }

  // 3. Node: addr 2..254 (rejects 0, 1 = LOCAL itself, 0xFF broadcast), and a known peer
  //    (same precondition as the REST handler).
  uint8_t addr = 0;
  if (!mqttParseNodeLevel(nodeLevel, addr)) {
    Log_error("MqttService: /set node level '%s' invalid (expected %s<1..%u>)",
              nodeLevel, MQTT_NODE_TOPIC_PREFIX, (unsigned)(LORA_MAX_ADDR_REMOTE - MQTT_NODE_ADDR_OFFSET));
    return;
  }
  uint32_t bootId, registryRev, lastSeenMs;
  bool online, crawlPending;
  if (!SensorRegistry.getNodeInfo(addr, bootId, registryRev, lastSeenMs, online, crawlPending)) {
    Log_warn("MqttService: /set for unknown node 0x%02X ignored", addr);
    return;
  }

  // 4. A name that is changed by sanitizing cannot be represented in a topic, so it can
  //    neither be matched nor registered from here.
  if (!mqttLevelMatches(deviceLevel, deviceLevel) || !mqttLevelMatches(metricLevel, metricLevel)) {
    Log_error("MqttService: /set '%s/%s' rejected - names may only contain A-Z a-z 0-9 _ -",
              deviceLevel, metricLevel);
    return;
  }

  // 5. Map the topic names back to the registered names (sanitized comparison), so a
  //    "Sensor A" shown as Sensor_A in telemetry is found again. Unknown names are registered
  //    under the topic spelling, exactly like a REST POST creates them.
  char deviceName[DEVICE_TABLE_NAME_LEN];
  char metricName[DEVICE_TABLE_NAME_LEN];
  snprintf(deviceName, sizeof(deviceName), "%s", deviceLevel);
  snprintf(metricName, sizeof(metricName), "%s", metricLevel);
  for (uint8_t idx = 1; idx <= DEVICE_TABLE_MAX_DEVICES; ++idx) {
    if (!ControlRegistry.getDeviceSlot(addr, idx, slotScratch) ||
        !mqttLevelMatches(deviceLevel, slotScratch.name)) {
      continue;
    }
    snprintf(deviceName, sizeof(deviceName), "%s", slotScratch.name);
    for (uint8_t m = 0; m < slotScratch.metricCount; ++m) {
      if (mqttLevelMatches(metricLevel, slotScratch.metrics[m].name)) {
        snprintf(metricName, sizeof(metricName), "%s", slotScratch.metrics[m].name);
        break;
      }
    }
    break;
  }

  // 6. Payload: one metric object, identical to an entry of the REST "metrics" map.
  JsonDocument doc;
  if (deserializeJson(doc, entry.payload) || !doc.is<JsonObject>()) {
    Log_error("MqttService: /set %s/%s payload is not a JSON object", deviceName, metricName);
    return;
  }

  // 7. Same registration and storage as POST /api/lora/nodes/control.
  const uint32_t revBefore = ControlRegistry.getRegistryRev(addr);
  uint32_t deviceIdx = 0;
  uint32_t metricId  = 0;
  if (!controlApplyMetric(addr, deviceName, metricName, doc.as<JsonVariant>(), deviceIdx, metricId)) {
    Log_error("MqttService: /set 0x%02X %s/%s rejected (invalid value, table full or payload too large)",
              addr, deviceName, metricName);
    return;
  }

  // New or changed schema: PING so the REMOTE crawls it (the REST handler does the same).
  if (ControlRegistry.getRegistryRev(addr) != revBefore) {
    pingTaskNotifyControlChange(addr);
  }

  // Hand the transmission to PushTask: it blocks on the LoRa ACK, mqttTask must not.
  pushTaskNotifySetpoint(addr, static_cast<uint8_t>(deviceIdx));
  Log_info("MqttService: setpoint 0x%02X %s/%s stored (dev=%u metric=%u), push queued",
           addr, deviceName, metricName, deviceIdx, metricId);
}

// ---------------------------------------------------------------------------
// mqttTask - broker connection loop
// ---------------------------------------------------------------------------
void MqttServiceClass::mqttTask(void* param) {
  MqttServiceClass* self = static_cast<MqttServiceClass*>(param);
  Log_info("MqttTask started on Core %d", xPortGetCoreID());

  uint32_t backoffMs = MQTT_BACKOFF_MIN_MS;

  for (;;) {
    // Snapshot current config and clear the reconnect flag atomically under mutex.
    MqttConfig cfg = {};
    if (self->configMutex != nullptr &&
        xSemaphoreTake(self->configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      cfg = self->config;
      self->reconnectRequested = false;
      xSemaphoreGive(self->configMutex);
    }

    // Store the active config snapshot used by this connect attempt.
    self->activeConfig = cfg;

    // Discard any stale pending publishes accumulated while not connected.
    portENTER_CRITICAL(&self->pendingMux);
    self->pendingCount = 0;
    portEXIT_CRITICAL(&self->pendingMux);

    // -----------------------------------------------------------------------
    // IDLE / not configured: wait and poll for config changes.
    // -----------------------------------------------------------------------
    if (cfg.enabled == 0 || cfg.broker[0] == '\0') {
      if (self->client.connected()) {
        char statusTopic[MQTT_STATUS_TOPIC_MAX_LEN];
        buildStatusTopic(cfg.prefix, statusTopic, sizeof(statusTopic));
        self->client.publish(statusTopic, "offline", /*retained=*/true, /*qos=*/0);
        self->client.disconnect();
        Log_info("MqttService: disabled or broker cleared - disconnected");
      }
      if (self->configMutex != nullptr &&
          xSemaphoreTake(self->configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        self->setState(MqttState::Idle);
        self->status.retryAtMs = 0;
        xSemaphoreGive(self->configMutex);
      }

      // Wait up to MQTT_IDLE_CHECK_MS, but wake early if applyConfig() signals.
      for (uint32_t waited = 0; waited < MQTT_IDLE_CHECK_MS; waited += 500) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (self->reconnectRequested) {
          break;
        }
      }
      backoffMs = MQTT_BACKOFF_MIN_MS; // reset backoff when transitioning from idle
      continue;
    }

    // -----------------------------------------------------------------------
    // ENABLED - connect or reconnect.
    // -----------------------------------------------------------------------
    if (self->configMutex != nullptr &&
        xSemaphoreTake(self->configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      self->setState(MqttState::Disconnected);
      self->status.retryAtMs = 0;
      xSemaphoreGive(self->configMutex);
    }

    if (!self->connectBroker(cfg)) {
      uint32_t retryAt = millis() + backoffMs;
      if (self->configMutex != nullptr &&
          xSemaphoreTake(self->configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        self->setState(MqttState::Error);
        self->status.lastError      = self->client.lastError();
        self->status.lastReturnCode = self->client.returnCode();
        self->status.retryAtMs      = retryAt;
        xSemaphoreGive(self->configMutex);
      }
      Log_warn("MqttService: will retry in %u ms (backoff)", backoffMs);

      // Wait for backoffMs, but wake early on a config change.
      uint32_t waited = 0;
      while (waited < backoffMs) {
        uint32_t slice = (backoffMs - waited < 500) ? (backoffMs - waited) : 500;
        vTaskDelay(pdMS_TO_TICKS(slice));
        waited += slice;
        if (self->reconnectRequested) {
          break;
        }
      }

      // Double the backoff, capped at MQTT_BACKOFF_MAX_MS.
      backoffMs = (backoffMs * 2 > MQTT_BACKOFF_MAX_MS) ? MQTT_BACKOFF_MAX_MS : backoffMs * 2;
      continue;
    }

    // Connected successfully - reset backoff and clear error fields.
    if (self->configMutex != nullptr &&
        xSemaphoreTake(self->configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      self->setState(MqttState::Connected);
      self->status.lastError      = 0;
      self->status.lastReturnCode = 0;
      self->status.retryAtMs      = 0;
      xSemaphoreGive(self->configMutex);
    }
    backoffMs = MQTT_BACKOFF_MIN_MS;

    // -----------------------------------------------------------------------
    // CONNECTED - run the client loop until disconnect or config change.
    // -----------------------------------------------------------------------
    while (self->client.connected() && !self->reconnectRequested) {
      // Wait for notification up to MQTT_LOOP_MS (100 ms) or wake early on new data.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MQTT_LOOP_MS));

      self->client.loop();

      if (!self->client.connected() || self->reconnectRequested) {
        break;
      }

      // Retry a failed SUBSCRIBE, then process the /set messages client.loop() just delivered.
      if (!self->setSubscribed && static_cast<int32_t>(millis() - self->setRetryAtMs) >= 0) {
        self->subscribeSet(self->activeConfig);
      }
      self->drainSetQueue(self->activeConfig);
      if (!self->client.connected()) {
        break;
      }

      // Drain any pending publishes (availability first so "online" precedes the data)
      self->drainPeerStatus(self->activeConfig);
      self->drainPublishQueue(self->activeConfig);
    }

    // Detect the cause of the loop exit.
    if (!self->client.connected()) {
      Log_warn("MqttService: connection lost (rc=%d) - will reconnect", self->client.lastError());
      uint32_t retryAt = millis() + backoffMs;
      if (self->configMutex != nullptr &&
          xSemaphoreTake(self->configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        self->setState(MqttState::Disconnected);
        self->status.retryAtMs = retryAt;
        xSemaphoreGive(self->configMutex);
      }
    } else {
      // reconnectRequested: config changed. Disconnect gracefully before re-reading config.
      char statusTopic[MQTT_STATUS_TOPIC_MAX_LEN];
      buildStatusTopic(cfg.prefix, statusTopic, sizeof(statusTopic));
      self->client.publish(statusTopic, "offline", /*retained=*/true, /*qos=*/0);
      self->client.disconnect();
      Log_info("MqttService: config change detected - reconnecting");
      if (self->configMutex != nullptr &&
          xSemaphoreTake(self->configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        self->setState(MqttState::Disconnected);
        self->status.retryAtMs = 0;
        xSemaphoreGive(self->configMutex);
      }
    }
    // Loop back to the top where the new config is re-read and reconnect is attempted.
  }
}
