# MQTT Integration Implementation Plan

**Goal:** Implement lightweight MQTT telemetry forwarding (LoRa to MQTT) and setpoint subscription (MQTT to LoRa) on the LOCAL node using a hierarchical topic tree and non-blocking FreeRTOS architecture.

---

## Architecture Overview

- **Target Node:** LOCAL node only (REMOTE nodes remain unaware of MQTT).
- **Placement:** `MqttService.cpp/.h` in `src/service/` (consistent with `WebSocketService`, `RestService`, and `WiFiConfig`).
- **Core Library:** `256dpi/MQTT @ ^2.5.3` (plain TCP, synchronous client inside dedicated task, ~10 KB flash).
- **Threading Model:** Dedicated FreeRTOS task `mqttTask` (priority 0 on Core 1, strictly below the Arduino loop task; priority 1 stalled the LoRa boot sequence, see `doc/Learnings.md` chapter 1) with non-blocking spinlock pending-set hand-off (depth 12 with de-duplication) bridging `loraTransportTask` and MQTT network I/O.
- **Failure Isolation:** Non-blocking pending-set hand-off ensures an unreachable or failing MQTT broker never blocks LoRa reception (`loraTransportTask`), the WiFi stack, or Core 0 web callbacks.
- **Topic Scheme:** `<prefix>/<node>/<device>/<metric>` where `<node>` is textual `Remote_Node_<n>` and `n` is the dashboard node number (`n = LoRa address - 1`, so `REMOTE (Node 1)` = address 0x02 = `Remote_Node_1`, e.g. `LoRaIoT/Remote_Node_1/SensorA/Voltage`), publishing scaled real decimal strings with `retain = true`.
- **Availability / LWT:** Broker Last Will and Testament (LWT) on `<prefix>/Local/status` (`offline` on ungraceful disconnect, `online` on connect) and per-node availability on `<prefix>/Remote_Node_<n>/status`.
- **Control Path:** Setpoint subscription on `<prefix>/Local/+/+/+/set` (topic `<prefix>/Local/Remote_Node_<n>/<device>/<metric>/set`, JSON payload identical to one metric object of `POST /api/lora/nodes/control`) translating into `ControlRegistry` updates and a per-device `PushTask` hand-off that transmits a `DATA_ACK` frame.

---

## Sub-Tasks

### Sub-Task 1: Library Dependency and MqttService Task Engine

- **Intent:** Add `256dpi/arduino-mqtt` library and implement minimal `MqttService` skeleton with a dedicated Core 1 FreeRTOS task to measure exact flash and heap usage before further feature implementation.
- **Expected Outcomes:**
  - `platformio.ini` includes `256dpi/MQTT @ ^2.5.3`.
  - `src/service/src/MqttService.h` and `src/service/src/MqttService.cpp` scaffolded.
  - `mqttServiceBegin()` invoked only on LOCAL nodes in `Application.cpp:applicationSetup()`.
  - Task `mqttTask` runs on Core 1 (`TASK_CREATE`, stack size 4096 bytes, priority 0) and registers with `TaskRegistry.add()`.
  - Connection management with exponential backoff and LWT (`<prefix>/Local/status` = `offline`).
  - Flash image size and heap headroom verified immediately after linking.
- **Todo List:**
  - [x] Add `256dpi/MQTT @ ^2.5.2` to `platformio.ini` under `[env]` `lib_deps`.
  - [x] Create `src/service/src/MqttService.h` and `src/service/src/MqttService.cpp`.
  - [x] Implement `mqttServiceBegin()` and hook into `Application.cpp:applicationSetup()`.
  - [x] Implement `mqttTask` skeleton with `TASK_CREATE`, `TaskRegistry.add()`, and `MQTTClient` instance.
  - [x] Configure LWT and broker connection loop.
  - [x] Verify build size and resource footprint on hardware (to be done by developer).
- **Relevant Context:**
  - [`src/service/src/WebSocketService.cpp`](src/service/src/WebSocketService.cpp:1)
  - [`src/util/src/TaskUtils.h`](src/util/src/TaskUtils.h:1) (`TASK_CREATE` macro)
- **Status:** `[x] done (LWT, connection loop, NVS config, REST API implemented; awaiting developer build & resource check)`

---

### Sub-Task 2: NVS Storage and REST Configuration API

- **Intent:** Provide persistent storage in NVS and REST endpoints to configure, enable/disable, and query MQTT broker settings.
- **Expected Outcomes:**
  - NVS keys read and written under namespace `sc_prefs`.
  - `GET /api/mqtt/config` returns current settings with password masked.
  - `PUT /api/mqtt/config` updates settings in NVS and validates parameters.
  - `GET /api/mqtt/status` returns live connection state (`idle`, `disconnected`, `connected`, `error`).
- **Todo List:**
  - [x] Define NVS key constants (`mqttBroker`, `mqttPort`, `mqttUser`, `mqttPass`, `mqttPrefix`, `mqttRetain`, `mqttDiscovery`, `mqttEnabled`) in `MqttService.h`.
  - [x] Implement configuration struct, load/save helpers using ESP32 `Preferences` (`sc_prefs` namespace).
  - [x] Add route `GET /api/mqtt/config` in `RestService::registerRoutes()`.
  - [x] Add route `PUT /api/mqtt/config` in `RestService::registerRoutes()` with JSON input validation.
  - [x] Add route `GET /api/mqtt/status` in `RestService::registerRoutes()`.
- **Relevant Context:**
  - [`src/service/src/RestService.cpp`](src/service/src/RestService.cpp:116)
  - [`src/service/src/WiFiConfig.cpp`](src/service/src/WiFiConfig.cpp:14) (reference for `sc_prefs` NVS pattern)
- **Status:** `[x] done`

---

### Sub-Task 3: LoRa-to-MQTT Forwarding Pipeline (Telemetry Publishing)

- **Intent:** Bridge received sensor data from `loraTransportTask` to `mqttTask` and publish per-metric scalar topics with real decimal values.
- **Expected Outcomes:**
  - Successful `SensorRegistry.storeData()` posts `{addr, deviceIdx}` to non-blocking pending-set and wakes `mqttTask` via task notification.
  - `mqttTask` drains pending entries, snapshots `DeviceSlot` under mutex via `SensorRegistry.getDeviceSlot()`.
  - Topic strings sanitized and formatted as `<prefix>/Remote_Node_<n>/<device>/<metric>` (first verified as `Remote_<addr>`; changed to `Remote_Node_<n>` together with Sub-Task 5, re-verify).
  - Values formatted directly using integer arithmetic with scale applied (e.g. value 335, scale 100 -> `"3.35"`), omitting scale metadata.
  - Text and boolean values forwarded verbatim.
  - Metrics published with `retain = true` (or configured retain), `qos = 0`.
- **Todo List:**
  - [x] In `Application.cpp:onLoRaReceive()`, add `mqttServiceNotifyPublish(frame.src, data.device)` in `DATA` and `DATA_ACK` handlers.
  - [x] Implement non-blocking pending-set drain loop in `MqttService.cpp`.
  - [x] Implement topic sanitization helper (replace invalid MQTT chars with `_`).
  - [x] Implement real-value decimal formatting helper for numeric metrics (based on `scale`) and text metrics using pure integer arithmetic.
  - [x] Implement fallback topic formatting (`<prefix>/Remote_Node_<n>/dev<idx>/m<id>`) for un-cached Describe states.
  - [x] Publish all active metrics in the snapshotted device slot.
- **Relevant Context:**
  - [`src/LoRaIoT/src/Application.cpp`](src/LoRaIoT/src/Application.cpp:610)
  - [`src/registry/src/PeerRegistry.h`](src/registry/src/PeerRegistry.h:160) (`getDeviceSlot()`)
  - [`src/registry/src/DeviceTable.h`](src/registry/src/DeviceTable.h:78) (`MetricSlot`)
- **Status:** `[x] done (awaiting hardware verification by developer)`

---

### Sub-Task 4: Node Availability & Liveness Tracking

- **Intent:** Publish retained availability status for individual remote nodes based on peer discovery and timeout events.
- **Expected Outcomes:**
  - Remote nodes report `<prefix>/Remote_Node_<n>/status` with payload `online` or `offline` (`retain = true`; first verified as `Remote_<addr>`, changed with Sub-Task 5, re-verify).
  - Liveness transitions in `SensorRegistry` trigger MQTT availability publishes.
- **Todo List:**
  - [x] Add `mqttServiceNotifyPeerStatus(uint8_t addr, bool online)` in `MqttService`.
  - [x] Hook into `Application.cpp` BEACON/PING receive and `PingTask` offline transitions.
  - [x] Publish `<prefix>/Remote_Node_<n>/status` whenever a peer's online state changes.
- **Relevant Context:**
  - [`src/registry/src/PeerRegistry.h`](src/registry/src/PeerRegistry.h:128) (`markSeen`, `markOffline`)
  - [`src/LoRaIoT/src/Application.cpp`](src/LoRaIoT/src/Application.cpp:570)
- **Status:** `[x] done (awaiting hardware verification by developer)`

---

### Sub-Task 5: Actuator and Control Setpoint Subscription

- **Intent:** Enable external MQTT publishers (Home Assistant, Node-RED, scripts) to push setpoints to REMOTE actuators via LoRa `DATA_ACK` frames, with exactly the same data and processing as `POST /api/lora/nodes/control`.
- **Design (agreed):**
  - Topic `<prefix>/Local/Remote_Node_<n>/<device>/<metric>/set` (node address = `n + 1`). The `Local/` namespace marks "command into LOCAL" and keeps command topics apart from telemetry (`<prefix>/Remote_Node_<n>/...`) and from `<prefix>/Local/status`.
  - Payload: JSON object, identical to one metric entry of the REST body, e.g. `{"value":336,"unit":"V","scale":100}` (numeric, scaled integer) or `{"value":"online"}` (text/boolean). `unit` and `scale` are optional (defaults `""` and `1`), as in REST. No plain-value payload form.
  - One message pushes one control data message (the whole device the metric belongs to) to the REMOTE.
  - Retained `/set` messages are ignored.
- **Expected Outcomes:**
  - `mqttTask` subscribes to `<prefix>/Local/+/+/+/set` after every successful broker connect.
  - The message callback only copies topic and payload into a small fixed queue (no allocation, no blocking).
  - `mqttTask` (outside the callback) parses the topic, resolves device/metric names against the registered (sanitized) names, parses the JSON, registers the metric and stores the value through the same helper the REST handler uses (`controlApplyMetric()`).
  - The push is handed to `PushTask` (`pushTaskNotifySetpoint(addr, deviceIdx)`), which transmits via `loraSendDataReliable()` on its own task. `mqttTask` never blocks on LoRa.
- **Todo List:**
  - [x] Extract the per-metric REST logic (value type handling, `registerMetric`, `storeValue`/`storeTextValue`) into `controlApplyMetric()` (`ControlSetpoint.h/.cpp`) and use it from `RestService` and `MqttService`.
  - [x] Add `pushTaskNotifySetpoint(addr, deviceIdx)` to `PushTask` (per-device push, non-blocking queue send) instead of calling `loraSendDataReliable()` from `mqttTask`.
  - [x] Change telemetry and status topics to `Remote_Node_<n>`.
  - [x] Subscribe to `<prefix>/Local/+/+/+/set` in `MqttServiceClass::connectBroker()` after every (re)connect (a live prefix change reconnects and therefore re-subscribes).
  - [x] Implement the MQTT message callback (`onMessageAdvanced`) with a bounded queue; enlarge the client buffer to 256 bytes.
  - [x] Parse topic levels (`Remote_Node_<n>`, device, metric, `set`); reject malformed topics and names that cannot be represented in a topic (error log).
  - [x] Resolve topic names to the registered names by comparing sanitized names.
  - [x] Update `ControlRegistry` under its own mutex (inside the registry API) and notify `PingTask` when the schema revision changed.
  - [x] Hand the push off to `PushTask`.
- **Relevant Context:**
  - [`src/service/src/RestService.cpp`](src/service/src/RestService.cpp:828) (reference setpoint dispatch flow)
  - [`src/LoRaIoT/src/PushTask.h`](src/LoRaIoT/src/PushTask.h) (non-blocking push queue)
  - [`src/registry/src/PeerRegistry.h`](src/registry/src/PeerRegistry.h:215) (`ControlRegistry`)
  - `doc/MqttFeasibility.md` section 4.3 (implemented behaviour, limits, open points)
- **Status:** `[x] done (awaiting hardware verification by developer)`

---

### Sub-Task 6: Optional Home Assistant MQTT Auto-Discovery

- **Intent:** Provide optional Home Assistant auto-discovery configs published when a Describe crawl completes.
- **Expected Outcomes:**
  - When `mqttDiscovery` is enabled (1), completing a Describe crawl triggers generation of discovery JSON payloads.
  - Published to `homeassistant/sensor/<unique_id>/config` with `retain = true`.
  - Zero permanent heap usage (generated on stack / temporary buffer).
- **Discovery Message Examples (Project Test Data):**

**Example 1: Voltage Metric (`SensorA/Voltage` on Node 0x02)**
- Topic: `homeassistant/sensor/loraiot_2_SensorA_Voltage/config`
- Payload:
```json
{
  "name": "Voltage",
  "stat_t": "LoRaIoT/Remote_Node_1/SensorA/Voltage",
  "unit_of_meas": "V",
  "dev_cla": "voltage",
  "stat_cla": "measurement",
  "uniq_id": "loraiot_2_SensorA_Voltage",
  "avty_t": "LoRaIoT/Remote_Node_1/status",
  "dev": {
    "ids": ["loraiot_2_SensorA"],
    "name": "SensorA",
    "mf": "LoRaIoT",
    "via_device": "loraiot_local"
  }
}
```

**Example 2: Temperature Metric (`SensorA/Temp` on Node 0x02)**
- Topic: `homeassistant/sensor/loraiot_2_SensorA_Temp/config`
- Payload:
```json
{
  "name": "Temp",
  "stat_t": "LoRaIoT/Remote_Node_1/SensorA/Temp",
  "unit_of_meas": "°C",
  "dev_cla": "temperature",
  "stat_cla": "measurement",
  "uniq_id": "loraiot_2_SensorA_Temp",
  "avty_t": "LoRaIoT/Remote_Node_1/status",
  "dev": {
    "ids": ["loraiot_2_SensorA"],
    "name": "SensorA",
    "mf": "LoRaIoT",
    "via_device": "loraiot_local"
  }
}
```

- **Todo List:**
  - [ ] Add `mqttServicePublishDiscovery(uint8_t addr)` hook.
  - [ ] In `Application.cpp:onLoRaReceive()`, trigger discovery publish when `markCrawlComplete()` finishes.
  - [ ] Format discovery JSON payload with entity name, state topic, unit of measurement, unique ID, and device metadata.
  - [ ] Publish discovery messages for all metrics of the described peer.
- **Relevant Context:**
  - [`doc/MqttFeasibility.md`](doc/MqttFeasibility.md:275) (Section 8)
- **Status:** `[ ] pending - skipped for now by developer decision`

---

### Sub-Task 7: Documentation, OpenAPI Specification & Web UI Links

- **Intent:** Update API documentation, OpenAPI JSON served by the node, and web UI links.
- **Expected Outcomes:**
  - `doc/RestApi.md` documents `/api/mqtt/config` and `/api/mqtt/status`.
  - `src/LoRaIoT/src/openapi_json.h` includes the new MQTT endpoints.
  - `src/LoRaIoT/src/index_html.h` provides links or status display if applicable.
- **Todo List:**
  - [x] Add endpoint documentation to `doc/RestApi.md` (MQTT section with field-by-field tables, examples, pointer to `doc/MqttFeasibility.md`; `POST /api/lora/nodes/control` updated to mention the MQTT `/set` topic).
  - [x] Update `src/LoRaIoT/src/openapi_json.h` with OpenAPI 3.0 schema for the new endpoints (`MqttConfigResponse`, `MqttConfigPutRequest`, `MqttStatusResponse`, `x-tier: LOCAL`; `POST /api/lora/nodes/control` description updated to mention the MQTT `/set` topic).
  - [x] Add manual test links to `src/LoRaIoT/src/index_html.h` (links `/api/mqtt/config` and `/api/mqtt/status` already present and hidden on non-LOCAL tiers via `d.tier === 'LOCAL'` check; MQTT icon also present).
  - [x] Add short MQTT section to `README.md` pointing to `doc/MqttFeasibility.md` with the topic scheme summary (telemetry, availability, LWT, control `/set`).
- **Relevant Context:**
  - [`doc/RestApi.md`](doc/RestApi.md:1)
  - [`src/LoRaIoT/src/openapi_json.h`](src/LoRaIoT/src/openapi_json.h:1)
  - [`src/LoRaIoT/src/index_html.h`](src/LoRaIoT/src/index_html.h:1)
- **Status:** `[x] done`
