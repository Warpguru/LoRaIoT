# Documentation Refactoring Plan

## Overview

**Goal:** a qualitative improvement of the existing documentation without adding, removing or
substantially rewriting content. Only structure, placement and references change.

**Audience split:**

- `ReadMe.md` is for a newcomer (for example on GitHub): what the project is, how it works, what
  it needs, how to get started. Plain language, with enough technical facts to judge fit.
  Detail is available through collapsible `<details>` sections, compact by default.
- All other live documents are for a technical specialist who clones the project and changes it.

**Approach:** each sub-task below is one small, separately reviewable change. After each one,
stop and wait for the user's review before starting the next.

## Decisions made by the user

- `doc/IoTProtocol.md` content is distributed to other documents and the file is then deleted.
- `doc/images/LoRaIoT-flow.png` goes into the executive summary part of `ReadMe.md`.
- ReadMe sections 1 to 3 (summary, hardware requirements, wiring) stay in `ReadMe.md`, using
  collapsible sections for a compact default view. The E220 operating-mode table moves out.
- Factual errors found in `ReadMe.md` are corrected.
- The `ReadMe.md` introduction reuses the plain wording of `FAQ.md`.
- The file name `ReadMe.md` is kept. Links pointing at `README.md` are fixed.
- The MQTT topic scheme and examples move to `doc/Architecture.md`, into the existing MQTT
  chapter (section 8) as new sub-sections. A new chapter is created only if no existing one fits.
- Unifying the example names (to those of `IoTProtocol.md`) is the LAST step, together with a
  fact verification of the MQTT content whose outcome is decided later.
- When the draw.io source of the flow diagram is corrected, the user re-exports the PNG.

## Frozen documents (decided by the user)

These files are never modified, never linked to from live documents, and nothing is moved into
them. Reading them as a source to copy facts from is allowed.

- Dead documentation: `doc/Feasibility.md`, `doc/MqttFeasibility.md`, `doc/CiFeasibility.md`
- `doc/Learnings.md` (also never referenced from other documents)
- `doc/TechDebt.md`
- `doc/private/**`, `mqtt-plan.md`
- `AGENTS.md`, `.bob/**`, `ESP/**`

Consequence: live documents currently pointing to `doc/MqttFeasibility.md`, `doc/Feasibility.md`
or `doc/Learnings.md` lose those references. Where such a pointer was the only place describing a
fact, the fact gets a live home (sub-task 7).

## Ground rules (apply to every sub-task)

- Do not renumber existing sections of a target document. Append new sections at the end, so
  existing section numbers and anchors stay valid (`FAQ.md` links to an `Architecture.md`
  anchor; `AGENTS.md` cites section numbers).
- If a target document already says the same thing, the moved text is replaced by a link. Each
  fact lives exactly once.
- Moved text is kept verbatim except for minimal link and reference fixes.
- No em dash or en dash anywhere. Use `-`.
- Do not build, flash or call `GET /reset`. Documentation work needs none of that.

## Target structure after the refactoring

| Document | Audience | Role |
|---|---|---|
| `ReadMe.md` | newcomer | Pitch, flow image, how it works, short examples, MQTT summary, dashboard, WiFi modes, hardware, getting started, antenna tips, documentation map |
| `FAQ.md` | user | unchanged except for link fixes |
| `doc/RestApi.md` | integrator | endpoints, plus request schema and limits, worked REST examples |
| `doc/Architecture.md` | specialist | threading, boot, runtime behaviour, E220 interface and diagnostics, MQTT service (threading constraints, topic scheme, examples) |
| `doc/LoRaProtocol.md` | specialist | wire spec, plus air rate and link budget theory |
| `doc/IoTProtocol.md` | - | deleted |

---

## Sub-task 1 - Correct the factual errors in ReadMe.md

**Intent:** fix known-wrong statements first, in place, so later steps are pure moves.

**Expected outcomes:** `ReadMe.md` agrees with the code and with `doc/LoRaProtocol.md`.

**Todo list:**
1. Section 5 Phase 2: change "default Channel 25 / `0x19`" to channel 23 / `0x17`. The range 0-31 is correct (`LORA_MAX_CHANNEL = 31`).
2. Section 5 Phase 3 item 7: change "After 60 s of silence" to 90 s (`PING_IDLE_THRESHOLD_MS`).
3. Re-read Phase 3 items 3 to 7 once against `doc/LoRaProtocol.md` sections 6.1 and 8 for further numeric mismatches. Report findings, fix only clear errors.
4. The introduction wording ("transparently extend local IP networks", "enterprise-grade") is NOT changed here. It is rewritten in sub-task 11.

**Relevant context:** `ReadMe.md` lines 247, 278; `src/LoRaIoT/src/LoRaConfig.h` (`LORA_DEFAULT_CHANNEL`, `LORA_MAX_CHANNEL`); `src/LoRaIoT/src/PingTask.h` (`PING_IDLE_THRESHOLD_MS`); `doc/LoRaProtocol.md` sections 6.1, 8.

**Status:** [x] done

**Note for later sub-tasks:** only lines 247 and 278 of `ReadMe.md` changed (channel 23 / `0x17`, keep-alive 90 s). Phase 3 items 3 to 7 were cross-checked and all other numbers match the code and `doc/LoRaProtocol.md` (5 s / 60 s beacons, 2 min revert = `BEACON_REVERT_TIMEOUT_MS`, 3 ping attempts, 180 s purge = `LORA_STATE_STALE_MS`). Line numbers in the "Relevant context" of later sub-tasks are unchanged because no lines were added or removed.

---

## Sub-task 2 - Move the runtime behaviour from the ReadMe into Architecture.md

**Intent:** the communication lifecycle (ReadMe section 5, Phase 3) is firmware behaviour for specialists.

**Expected outcomes:** new section "9. Runtime Behaviour" appended to `doc/Architecture.md`. In `ReadMe.md`, Phase 3 is replaced by a short summary and a link.

**Todo list:**
1. Append "9. Runtime Behaviour" with the content of Phase 3 items 1 to 7 (E220 init, boot diagnostics, beacon and discovery, ping and ack, sensor push, control push, self-healing).
2. Where `doc/LoRaProtocol.md` sections 6.1, 8 or 15 already contain the same rule or sequence, reduce the text to a link. Keep text that exists only in the ReadMe (for example the three self-healing scenarios and the `200/202/504` delivery results).
3. Fix relative links inside the moved text (`doc/LoRaProtocol.md` becomes `LoRaProtocol.md`).
4. Replace Phase 3 in `ReadMe.md` with a 3 to 4 line summary and a link. Phase 1 and 2 (build, flash, provision) stay.
5. Phase 3 item 8 (fixed-mode routing) is already in `doc/LoRaProtocol.md` section 2: link only. Item 9 (REST and WebSocket) is already in `doc/RestApi.md` and `Architecture.md` section 5: link only.

**Relevant context:** `ReadMe.md` lines 254-283; `doc/Architecture.md` (currently ends with section 8); `doc/LoRaProtocol.md` sections 2, 6.1, 8, 15.

**Status:** [x] done

**Note for later sub-tasks:** `ReadMe.md` Phase 3 is now 11 lines (was 30 lines). The heading
"### Phase 3: Run & Communicate" is at approximately line 254; the removal of ~22 lines
shifts everything from line 287 onward downward. Sub-task 3 uses `ReadMe.md` lines 197-207
and 445-485 - those lines are now approximately 197-207 and 423-463 respectively. Re-locate
by heading ("E220 Hardware Operating Modes", "7. On-Demand Diagnostics") rather than by line
number. `doc/Architecture.md` section 9 starts at line 951; the file previously ended at
line 948.

---

## Sub-task 3 - Move the E220 interface and diagnostics into Architecture.md

**Intent:** the E220 operating-mode table and the 6-step diagnostics are specialist material.

**Expected outcomes:** new section "10. E220 Interface and Diagnostics" appended to `doc/Architecture.md`. `ReadMe.md` keeps the pin mapping table (a builder needs it) and gets a short troubleshooting pointer.

**Todo list:**
1. Move the ReadMe "E220 Hardware Operating Modes" table to the new section. Do not replace it with a link into `doc/Feasibility.md` (frozen).
2. Move ReadMe section 7 (6-step diagnostics table, the `LoRaTransport.pause()/resume()` note, multi-node log comparison) to the same section.
3. In `ReadMe.md`, replace section 7 with a 3 line "Troubleshooting" text: call `GET /api/diag/e220`, read `GET /api/log`, link to the Architecture section.

**Relevant context:** `ReadMe.md` lines 197-207 and 445-485; `doc/Architecture.md`; `doc/RestApi.md` (`GET /api/diag/e220`).

**Status:** [x] done

**Note for later sub-tasks:** `doc/Architecture.md` now ends with "## 10. E220 Interface and Diagnostics" (10.1 E220 Hardware Operating Modes; 10.2 On-Demand Diagnostics and Multi-Node Debugging with the 6-step table, `pause()/resume()` note, `0xC2` note and Multi-Node Log Comparison). It links to `RestApi.md#get-apidiage220` and `../ReadMe.md#canonical-pin-mapping`. Em dashes in the moved text were replaced by `-`. In `ReadMe.md` the heading "### E220 Hardware Operating Modes" (section 3) is kept with a one-line pointer to Architecture §10.1, and "## 7. On-Demand Diagnostics & Multi-Node Debugging" became "## 7. Troubleshooting" (3 lines, links to Architecture §10.2). ReadMe section numbering is unchanged; no file links to the old section 7 or E220 mode table anchors. ReadMe section 7 shrank from about 36 to 5 lines, so section 8 and the glossary appendix moved up by about 31 lines: re-locate by heading. Sub-task 11 may fold "7. Troubleshooting" and the one-line "E220 Hardware Operating Modes" pointer into the new structure (and must then keep the links in the Architecture section 10 intro, which targets `ReadMe.md#canonical-pin-mapping`, valid). `AGENTS.md` line 121 cites `README.md` §3 for the pin mapping (frozen, unchanged; the pin mapping is still in section 3).

---

## Sub-task 4 - Move the air rate theory into LoRaProtocol.md

**Intent:** link budget and register constants are specialist material; antenna placement stays for installers.

**Expected outcomes:** new sub-section "Link budget and air rate selection" appended to section 18 of `doc/LoRaProtocol.md` (last section, nothing renumbered). `ReadMe.md` keeps the antenna rule and one sentence on air rate with a link.

**Todo list:**
1. Move ReadMe section 8 rule 2 (link budget, dB figures, `REG0` constants) into section 18.
2. Keep the existing `PUT /api/lora/airrate` reference.
3. Keep rule 1 (parallel-tilt antenna rule) in `ReadMe.md`.
4. ReadMe keeps one sentence: lowering the air rate trades throughput for range, with a link to `doc/LoRaProtocol.md` section 18.

**Relevant context:** `ReadMe.md` lines 486-502; `doc/LoRaProtocol.md` section 18.

**Status:** [x] done

**Note for later sub-tasks:** `doc/LoRaProtocol.md` section 18 now ends with sub-section `### Link budget and air rate selection` (lines 781-794), covering the mathematical coupling of sensitivity to spreading factor/bandwidth, `REG0` constants (`REG0_AIR_DATA_RATE_62K5` to `REG0_AIR_DATA_RATE_2K4`, $+14\text{ dB}$ link budget increase), the $6\text{ dB}$ range/penetration doubling rule, `PUT /api/lora/airrate`, and internal links to the test results and timeout note above. In `ReadMe.md` section 8, rule 1 (`1. The Parallel-Tilt Antenna Rule`) remains unchanged; rule 2 (`2. Dropping Air Data Rate for Link Budget Gains`) was reduced to two sentences stating that lowering air rate via `PUT /api/lora/airrate` trades throughput for range and linking to `doc/LoRaProtocol.md#link-budget-and-air-rate-selection`. No lines were added/removed from ReadMe.md that would affect earlier sections; section 8 is now lines 427-440.

---

## Sub-task 5 - Move the request schema and field limits into RestApi.md

**Intent:** the "Request Body Schema and Field Limits" part of `IoTProtocol.md` is API reference material.

**Expected outcomes:** `doc/RestApi.md` documents the shared JSON body, field table, topic-level sanitization rule and capacity limits table (with `DeviceTable.h` constants), next to the two endpoints that use it.

**Todo list:**
1. Add the schema, field reference, sanitization rule and capacity table as a sub-section of `POST /api/lora/sensor/data`. Refer to it from `POST /api/lora/nodes/control` (no duplication).
2. Merge with the existing "Value types" list in `POST /api/lora/sensor/data`. Remove only exact duplicates.
3. Keep the HTTP 400 and `rejected` array behaviour description.
4. Add an anchor that other documents can link to.

**Relevant context:** `doc/IoTProtocol.md` lines 30-90; `doc/RestApi.md` sections `POST /api/lora/sensor/data`, `POST /api/lora/nodes/control`; `src/registry/src/DeviceTable.h`.

**Status:** [x] done

**Note for later sub-tasks:** the schema now lives in `doc/RestApi.md` as `### Request body schema and field limits`, the last sub-section of `POST /api/lora/sensor/data` (with `#### Field Reference`, `#### Sanitization Rule for Topic Levels` and the `<details>` capacity table). GitHub anchor to link to: `#request-body-schema-and-field-limits` (from `doc/Architecture.md` and other documents: `RestApi.md#request-body-schema-and-field-limits`). The old "Value types" lists in `POST /api/lora/sensor/data` and `POST /api/lora/nodes/control` were replaced by one-line references to that anchor. The sub-section mentions the MQTT `/set` payload and topic levels neutrally and has no link to MQTT anchors: sub-task 7 may add a link from the Sanitization Rule to Architecture 8.5/8.6. Sub-task 6 should link its examples to this anchor. The two `MqttFeasibility.md` pointers in `RestApi.md` (in `POST /api/lora/nodes/control` and in the MQTT section) are untouched (sub-task 7). `doc/IoTProtocol.md` was not modified.

---

## Sub-task 6 - Move the REST worked examples into RestApi.md

**Intent:** curl examples, expected responses and notes of `IoTProtocol.md` belong with the API reference.

**Expected outcomes:** new section "Worked Examples" at the end of `doc/RestApi.md` with the placeholder table (`<Local>`, `<Remote-Node-x>`, `<broker>`), the REST part of Example 1 (sensor POST) and Example 2 (control POST), bash and PowerShell variants, expected responses and notes.

**Todo list:**
1. Append "Worked Examples". Keep the `<details>` blocks as in the source.
2. Move the placeholder reference.
3. Move the example responses, status code notes (`200`, `202`, `503`, `504`) and the notes lists. Skip MQTT parts (sub-task 7).
4. Link the examples to the schema (sub-task 5) and to the MQTT section in `doc/Architecture.md` (sub-task 7).

**Relevant context:** `doc/IoTProtocol.md` lines 1-30, 94-155, 190-202, 205-267, 360-372.

**Status:** [x] done

**Note for later sub-tasks:** `doc/RestApi.md` now ends with `## Worked Examples` (line ~950) with `### Address placeholders` (`#address-placeholders`), `### Example 1 - Environmental Sensor (REMOTE, sensor data POST)` (`#example-1---environmental-sensor-remote-sensor-data-post`) and `### Example 2 - Power Supply Control (LOCAL, control setpoint POST)` (`#example-2---power-supply-control-local-control-setpoint-post`). Sub-task 8 appends its read-back examples to this section as further `###` sub-headings. Facts fixed in the moved copy (verified against `RestService.cpp`, `DeviceTable.cpp`): `registry_rev` in the responses is now `3` (Example 1: 1 device + 2 metrics) and `4` (Example 2: 1 device + 3 metrics) instead of `1`, because `DeviceTable::registerMetric` increments the revision once per new device and once per new metric; Example 2 shows the unchanged-schema `200`/`"ok"` case and the text explains that the first call is `202`/`after_describe`; the `<Remote-Node-x>` placeholder now says the IP comes from `GET /api/wifi/mode` (`staIp`/`apIp`) and the LoRa address from `GET /api/lora/nodes` or `GET /api/lora/address`. The link `#schema` became `#request-body-schema-and-field-limits`; "see `doc/RestApi.md` for the status code table" became local links to the `POST /api/lora/sensor/data` and `POST /api/lora/nodes/control` sections. No link to MQTT anchors was added; sub-task 7 adds them (Worked Examples intro, "Request Body Explained" remarks, Notes).

**Left in `doc/IoTProtocol.md` for sub-task 7 (nothing of this was moved; the file is unchanged):**
- Title and intro paragraph (lines 1-10): MQTT-heavy ("the exact MQTT topics that LOCAL publishes or subscribes to", `mqttSanitizeTopicLevel` in `MqttService.cpp` passes `A-Z a-z 0-9 _ -` unchanged and replaces every other character (space, `%`, `/`, etc.) with `_`). The image `images/LoRaIoT-flow.png` was not moved (goes to the ReadMe in sub-task 11).
- Example 1 Scenario, last part of the last sentence: "LOCAL receives the DATA_ACK frame over LoRa and forwards each metric to the MQTT broker as a separate retained topic."
- Example 1 Assumptions: "REMOTE LoRa address: `0x02` (assumption - `Remote_Node_1`; confirm with `GET /api/lora/address` on the node). `n = addr - 1 = 1`, so the MQTT node level is `Remote_Node_1`. Adjust `Remote_Node_<n>` in the topics below if the address differs."; "MQTT broker: `<broker>`, user and password as configured."; "MQTT prefix: `LoRaIoT` (default)."; "`mqttRetain`: `1` (default) - all telemetry topics are published with `retain = true`."
- Example 1 sections "MQTT Topics Published by LOCAL" (table of `status`, `Env-Sensor/Temperature` `21.5`, `Env-Sensor/Humidity` `48.3`, plus the qos 0 / retain / sanitizer / `%` unit paragraph) and "Subscribe and See the Topics" (`mosquitto_sub`, expected output, retained replay).
- Example 1 Notes, left: "After the first POST with a new device or metric name, REMOTE increments `registry_rev` and pings LOCAL. LOCAL starts a Describe crawl to fetch the device/metric definitions (name, unit, scale) before the DATA_ACK arrives. Until the crawl completes, the MQTT topic levels fall back to `dev<idx>` / `m<id>`. After the crawl completes, topics switch to the real names." and "The node address `0x02` / `Remote_Node_1` is stated as an assumption above. If the node is provisioned at a different address, adjust the `Remote_Node_<n>` level in all topics accordingly."
- Moved whole, including short MQTT side remarks that sub-task 7 may reword when it adds the topic sections: Example 1 "Request Body Explained" ("It does not appear in the MQTT topic name; only the metric name ... is used to build the topic level"), Example 2 "Request Body Explained" ("published as `12.00`" / "`5.0`"), Example 2 `Pwr` note ("pass the sanitizer unchanged"). Sub-task 7 must re-check "published as `12.00`" against the MQTT formatter (`mqttFormatScaledDecimal`).
- Example 2 Scenario, last sentence: "An MQTT client can also push the same setpoints via the `/set` topic without calling the REST API."
- Example 2 Assumptions: "Use `addr=3` (and `Remote_Node_2` in topics) if the REMOTE is at address `0x03`, etc." (topic part; the `addr=3` part was kept), "MQTT broker: `<broker>`, user and password as configured.", "MQTT prefix: `LoRaIoT` (default)." The assumption "Target REMOTE node LoRa address: `0x02` (assumption - `Remote_Node_1`)" lost its `Remote_Node_1` part.
- Example 2 whole MQTT block: "MQTT Control (the `/set` Topic)" (filter `LoRaIoT/Local/+/+/+/set`, `mqttTask`, `controlApplyMetric`, `PushTask`), "/set Topic Pattern" (pattern, 3-row table, unit/scale optional rule), "mosquitto_pub Examples" (4 commands), the "Do not set the retain flag" paragraph (`MQTT_SET_RETAINED_DRAIN_MS`, 1 s), "What LOCAL Subscribes To", "REMOTE-Side Result".
- Example 2 Notes, left: "The device name `"Power-Supply"` is stored in `ControlRegistry` as `"Power-Supply"`. The hyphen is already valid in MQTT topic levels, so the sanitized topic level is identical to the stored name: `Power-Supply`. The `/set` parser resolves topic names back to registry names by comparing their sanitized forms, which match exactly here." and "Each `mosquitto_pub` call triggers one independent LoRa push for the whole device. If the PushTask queue (8 entries) is full, the push is dropped without retry; the next `/set` or REMOTE BEACON triggers a re-synchronisation."
- "Further Reading" section: links to `RestApi.md`, `MqttFeasibility.md` (frozen, must not be carried over) and `LoRaProtocol.md`.
- Also open for sub-task 7: the two `MqttFeasibility.md` pointers in `doc/RestApi.md` (POST /api/lora/nodes/control and the end of GET /api/mqtt/status) and links from "Worked Examples" to the new MQTT sub-sections.

**Heading coverage of `doc/IoTProtocol.md` so far (for sub-task 9):** moved to `doc/RestApi.md`: "Request Body Schema and Field Limits" with "Field Reference", "Sanitization Rule for Topic Levels" and the capacity `<details>` (sub-task 5); "Node Address Placeholders", "Example 1 - Environmental Sensor" (Scenario, curl Command, Request Body Explained, Expected HTTP Response, Notes: REST parts) and "Example 2 - Power Supply Control" (Scenario, curl Command - REST POST, Request Body Explained, Expected HTTP Response, Notes: REST parts) (sub-task 6). Still to move (sub-task 7): title/intro text, "MQTT Topics Published by LOCAL", "Subscribe and See the Topics", "MQTT Control (the /set Topic)" with "/set Topic Pattern", "mosquitto_pub Examples", "What LOCAL Subscribes To", "REMOTE-Side Result", and the MQTT notes and assumption lines listed above. "Further Reading" is dropped at deletion (it links to a frozen document and the other two targets are covered elsewhere). The image goes to sub-task 11.

---

## Sub-task 7 - Move the MQTT topics and examples into the MQTT chapter of Architecture.md

**Intent:** the MQTT part of `IoTProtocol.md` becomes the live description of the topic scheme, because `MqttFeasibility.md` is frozen and is currently the only place the live documents point to for it. `doc/Architecture.md` already has an MQTT chapter (section 8), so the content goes there and no new chapter is created.

**Expected outcomes:**
- Section 8 of `doc/Architecture.md` is retitled from "MQTT Telemetry Service - Threading Constraints" to "MQTT Telemetry Service", because it no longer covers only threading. A grep found no link to this heading anchor, so the retitle breaks nothing. (If the user prefers to keep the old title, only the heading line is skipped.)
- Existing 8.1 to 8.4 keep their numbers and text. The new content is appended as 8.5 onward.
- Pointers to `MqttFeasibility.md` in live documents are replaced by links to the new sub-sections.

**Proposed sub-sections (final split decided while reading the source text):**
- 8.5 Topic scheme and payloads (published topics table, sanitization of topic levels, scaled decimal payload)
- 8.6 Control via the `/set` topic (pattern, payloads, subscription filter, retain rule with `MQTT_SET_RETAINED_DRAIN_MS`, REMOTE-side result)
- 8.7 Worked MQTT examples (`mosquitto_sub` / `mosquitto_pub`)

**Todo list:**
1. Move from `IoTProtocol.md`: published topics table, `mosquitto_sub` / `mosquitto_pub` examples, `/set` topic pattern and payload, the "do not set the retain flag" rule with its reason, the single subscription filter, and the REMOTE-side result.
2. Move from the ReadMe MQTT bullets only what is not yet in the new sub-sections. The short summary stays in the ReadMe.
3. Do not duplicate 8.1 (service overview and `/set` flow in `MqttTask`), 8.3 (timeout unit) and 8.4 (topic case sensitivity). Link to them instead.
4. Replace the two pointers to `MqttFeasibility.md` in `doc/RestApi.md` (lines 591 and 885) and the one in `doc/Architecture.md` section 8.1 (line 874) with links to the new sub-sections.
5. Link to the REST examples (sub-task 6) and to `GET /api/mqtt/config` / `GET /api/mqtt/status` in `doc/RestApi.md`.
6. Do NOT copy anything from `MqttFeasibility.md` in this step. A fact check against it is deferred to sub-task 14.

**Relevant context:** `doc/IoTProtocol.md` lines 156-188, 269-358, 369-371; `ReadMe.md` lines 113-123; `doc/RestApi.md` lines 591, 806-885; `doc/Architecture.md` lines 855-948.

**Status:** [x] done

**Note for later sub-tasks:**
- `doc/Architecture.md` section 8 is now titled "## 8. MQTT Telemetry Service" (anchor `#8-mqtt-telemetry-service`; no link pointed at the old anchor, re-grepped). New sub-sections after 8.4 and before "## 9. Runtime Behaviour" (not renumbered): `### 8.5 Topic scheme and payloads` (`#85-topic-scheme-and-payloads`, with `#### Topics published by LOCAL`), `### 8.6 Control via the /set topic` (`#86-control-via-the-set-topic`, with `#### /set Topic Pattern` and `#### REMOTE-Side Result`), `### 8.7 Worked MQTT examples` (`#87-worked-mqtt-examples`, with `#### Example 1 - Environmental Sensor: subscribe and see the topics` = `#example-1---environmental-sensor-subscribe-and-see-the-topics` and `#### Example 2 - Power Supply Control: set values with mosquitto_pub` = `#example-2---power-supply-control-set-values-with-mosquitto_pub`). Sub-task 11 links the ReadMe MQTT pointer (line 123) to `doc/Architecture.md#85-topic-scheme-and-payloads` (and may add 8.6, 8.7).
- Pointers replaced: `doc/Architecture.md` 8.1 (now links 8.6), `doc/RestApi.md` `POST /api/lora/nodes/control` intro (8.6) and end of `GET /api/mqtt/status` (8.5 to 8.7). New links in `doc/RestApi.md`: Sanitization Rule (8.5), Example 1 Request Body Explained (8.5) and Notes (8.7 Example 1), Example 2 Request Body Explained (8.5, 8.6) and Notes (8.7 Example 2). No `MqttFeasibility` text is left in `doc/RestApi.md` or `doc/Architecture.md`. Still open elsewhere: `ReadMe.md` line 123 (sub-task 11), `doc/Architecture.md` lines 475 and 902 (`Feasibility.md`, `Learnings.md`, sub-task 10).
- Facts corrected in the moved copy (verified against `MqttService.cpp/.h`, `PushTask.*`, `DeviceTable.cpp`, `Application.cpp`): (1) the `dev<idx>` / `m<id>` fallback applies only to an empty cached name, not "until the Describe crawl completes" (DATA of an uncached device or metric is not stored and not published); (2) `<prefix>/Remote_Node_<n>/status` also carries `offline`, and `<prefix>/Local/status` was added to the topic table; retained `online` follows `mqttRetain`; (3) "published unscaled" for a /set of an unregistered metric was wrong (setpoints are not published); the stored unit and scale of an already registered metric are kept; (4) "the next REMOTE BEACON triggers a re-synchronisation" was wrong: a push is re-sent by the next `/set` for the device, by a peer returning online (`pushTaskNotifyNodeBack`) or by the end of a Describe crawl (`pushTaskNotifyCrawlComplete`). Also added the formatter rule for scales (10 to 100000 give 1 to 5 decimals, >= 1000000 gives 6, otherwise the integer verbatim).
- Not changed but unclear: the `unit` row of the Field Reference in `doc/RestApi.md` ("used in MQTT payload only for `/set` round-trips"). The code never publishes the unit; it is only read from a `/set` payload and used when the metric is first registered. Left for the user's decision (sub-task 14).
- For sub-task 9 (`doc/IoTProtocol.md` was not modified): every MQTT item of the left-over list of sub-task 6 now has a live home in 8.5 to 8.7; the file's REST parts are in `doc/RestApi.md`. Not moved, by design: "Further Reading" (links a frozen file), the image (sub-task 11), and the sentence in Example 1 Notes about `registry_rev` / pinging LOCAL (REST part, covered by `doc/Architecture.md` 7.6).
- `ReadMe.md` MQTT bullets (lines 113-121) contain no fact missing from 8.5 to 8.7 or `doc/RestApi.md`, except that the config keys and state names are only in RestApi.md (as intended).

---

## Sub-task 8 - Move the ReadMe REST quick-start into RestApi.md

**Intent:** the full JSON request and response listings of ReadMe section 6 are reference material.

**Expected outcomes:** `ReadMe.md` section 6 is gone. Its unique content (read-back responses of `GET /api/lora/nodes/sensor` and `GET /api/lora/nodes/control`, staleness advice, live read) lives in "Worked Examples" in `doc/RestApi.md`. The two short curl examples in the ReadMe summary stay.

**Todo list:**
1. Move the read-back examples and the `?live=true` example into "Worked Examples", with original names for now (renamed in sub-task 14).
2. Keep the `online` / `age_ms` staleness advice for IoT devices.
3. Remove ReadMe section 6. The documentation map (sub-task 11) links to the new location.

**Relevant context:** `ReadMe.md` lines 287-443; `doc/RestApi.md`.

**Status:** [x] done

**Note for later sub-tasks:**
- `ReadMe.md` section 6 "REST API Quick-start Examples" is removed. Former "7. Troubleshooting" is now "## 6. Troubleshooting" and former "8. Range & RF Optimization Rules" is now "## 7. Range & RF Optimization Rules" (anchors `#6-troubleshooting`, `#7-range--rf-optimization-rules`; nothing links to them). Sub-task 11 restructures and renumbers anyway.
- `doc/RestApi.md` "Worked Examples" got four more sub-sections at its end: `### Example 3 - Sensor values: POST on REMOTE, read back on LOCAL` (original names `SensorA`, `Voltage`, `Temp`), `### Example 4 - Control setpoint: POST on LOCAL, read back on REMOTE` (original names `Charger`, `VoltageSet`), `### Example 5 - Live sensor read (bypasses cache, queries REMOTE directly)`. The section intro was adjusted ("two" examples became "end-to-end REST examples", plus a sentence on examples 1/2 versus 3 to 5). Sub-task 14 renames the example names in 3 and 4 (and the ReadMe summary examples).
- Address placeholders: `<LOCAL-IP>` became `<Local>` in the moved curl commands (the placeholder table of Worked Examples defines it). The ReadMe `<LOCAL-IP>` / `<REMOTE-IP>` definition block was not moved; the REMOTE SoftAP address `192.168.4.1` is used literally as in the source. `<LOCAL-IP>` / `<REMOTE-IP>` remain in the ReadMe summary (lines ~104, 134, 138, 153) and are not defined there any more; sub-task 11 must define them or rename (the dashboard and WiFi-mode sections use them).
- Corrections in the moved copy (old to new): `registry_rev` in the sensor POST response `1` to `3` and in the sensor read-back `1` to `3` (1 device + 2 metrics); control POST response `1` to `2` and control read-back `1` to `2` (1 device + 1 metric), shown as the repeat call with `"delivery":"ok"`. The sentence "On the next BEACON, LOCAL will see the new `registry_rev`, crawl the definitions" became "REMOTE immediately sends a PING carrying the new `registry_rev` to LOCAL (see Architecture.md section 7.6), LOCAL crawls the definitions" (the BEACON is no longer on the critical path, `RestService.cpp` calls `pingTaskNotifyControlChange`). The sentence "A `delivery` of `"after_describe"` means REMOTE rebooted and does not have the definitions yet - they will be pushed automatically once REMOTE crawls LOCAL" became "The call that first creates the device or metric is answered with `202` and `"delivery":"after_describe"` instead, see Example 2" (`RestService.cpp`: `schemaChanged` or BAD_REQUEST gives 202). The staleness advice links to the existing "Freshness fields" sub-section.
- Not changed: the example responses of `GET /api/lora/nodes/sensor` and `/control` in the endpoint sections (values `registry_rev` 6 and 1) were left; the control one (`1`) is an inconsistent sample, to be checked in sub-task 14.

---

## Sub-task 9 - Delete IoTProtocol.md after a coverage check

**Intent:** nothing may be lost.

**Expected outcomes:** `doc/IoTProtocol.md` is deleted. A table in the review message maps every heading of the file to its new location.

**Todo list:**
1. Produce the heading-to-location table.
2. Confirm that no document links to `IoTProtocol.md` (a grep found none besides the file itself).
3. Delete the file. `doc/images/LoRaIoT-flow.png` and `LoRaIoT-flow.drawio` stay.

**Relevant context:** `doc/IoTProtocol.md`.

**Status:** [x] done

**Note for later sub-tasks:** `doc/IoTProtocol.md` is deleted (re-grep: only `doc-refactoring-plan.md` mentions it; no live document linked to it or to its `#schema` anchor). `doc/images/LoRaIoT-flow.png` and `LoRaIoT-flow.drawio` remain. The image was referenced only by the deleted file, so it is now unreferenced until sub-task 11 places it in `ReadMe.md`. Heading coverage table: see the review message of sub-task 9. `Further Reading` was dropped on purpose (links the frozen `MqttFeasibility.md`; the other two targets are linked from the ReadMe documentation map of sub-task 11).

---

## Sub-task 10 - Remove references to frozen documents from live documents

**Intent:** live documents must not point to frozen ones.

**Expected outcomes:** no live document (`ReadMe.md`, `FAQ.md`, `doc/RestApi.md`, `doc/Architecture.md`, `doc/LoRaProtocol.md`, `doc/Glossary.md`, `doc/OpenAPI.md`, `doc/NanopbGeneration.md`) links to or cites `*Feasibility.md` or `Learnings.md`.

**Todo list:**
1. `doc/Architecture.md` line 475: "over the air according to `doc/Feasibility.md` §4.1" - drop the citation, keep the 30-50 ms statement.
2. `doc/Architecture.md` lines 900-902: "See `doc/Learnings.md` chapter 1 for the evidence..." - remove the reference, keep the statement that priority 0 is a verified workaround.
3. `ReadMe.md` line 123 is handled in sub-task 11; the `doc/Architecture.md` section 8.1 and `doc/RestApi.md` pointers are handled in sub-task 7.
4. Re-grep `Feasibility|Learnings` across the live documents. Report, but do not change, remaining references in frozen files (`AGENTS.md`, `.bob/**`, `doc/Learnings.md`, `doc/TechDebt.md`, `doc/private/**`, `mqtt-plan.md`).

**Relevant context:** `doc/Architecture.md` lines 475, 874, 900-902; grep results for `Feasibility|Learnings`.

**Status:** [x] done

**Note for later sub-tasks:** removed in `doc/Architecture.md`: "over the air according to `doc/Feasibility.md` §4.1" (now "over the air"; the 30-50 ms statement is kept) and "See `doc/Learnings.md` chapter 1 for the evidence, what is still open, and how to settle it." (the sentence "priority 0 is a verified workaround" is kept). Also removed in `doc/LoRaProtocol.md` (line ~463): "The full design is in `TechDebt.md`." (frozen file, not in the original todo list). Remaining live references to frozen documents after the re-grep: `ReadMe.md` line 123 (`MqttFeasibility.md`, sub-task 11) and `doc/LoggingAnalysis.md` line 171 (`doc/TechDebt.md §1`; the user confirmed it is a live document, so the pointer was removed: "the ring buffer itself - documented in `doc/TechDebt.md §1` - which holds" became "the ring buffer itself, which holds"; sub-task 13 must treat `doc/LoggingAnalysis.md` as live). All other hits are in frozen files (`AGENTS.md`, `.bob/**`, `doc/private/**`, `mqtt-plan.md`, `doc/MqttFeasibility.md`, `doc/TechDebt.md`, `doc/Learnings.md`, `doc/Feasibility.md`).

---

## Sub-task 11 - Restructure the ReadMe (introduction, image, collapsible details, documentation map)

**Intent:** a convincing, readable first page.

**Expected outcomes:** `ReadMe.md` is about 200 lines. Compact by default, details in `<details>`.

**Todo list:**
1. Replace the introduction with the plain wording of `FAQ.md` ("What is LoRaIoT and how does it work?") plus a few connecting sentences. Remove "enterprise-grade" and the claim of transparently extending IP networks (the system is a REST and MQTT data bridge). Do not cite `doc/Feasibility.md`.
2. Place `doc/images/LoRaIoT-flow.png` directly below the introduction.
3. Keep the ownership table, the Mermaid topology diagram and the two curl examples. Put the two step-by-step flow lists and the notes into `<details>`.
4. Keep MQTT, Live dashboard and WiFi modes. Put long parts (screenshots, WiFi curl, notes) into `<details>`. The MQTT pointer (line 123) links to the new MQTT sub-sections (8.5 onward) of `doc/Architecture.md`.
5. Keep section 2 (hardware requirements) and the pin mapping table. Use `<details>` where it helps.
6. Keep "Getting started" (Phase 1 and 2) and the antenna rule.
7. Add a "Documentation" section at the end, split into "For users" (`FAQ.md`, `doc/RestApi.md`, `doc/Glossary.md`) and "For developers" (`doc/Architecture.md`, `doc/LoRaProtocol.md`, `doc/OpenAPI.md`, `doc/NanopbGeneration.md`). Do not list any frozen document.
8. Keep the glossary appendix link.
9. Renumber ReadMe headings and fix internal anchors (`#wifi-modes`, `#live-dashboard`, `#appendix-a-glossary-of-abbreviations`).

**Relevant context:** `ReadMe.md`; `FAQ.md` lines 5-90; `doc/images/LoRaIoT-flow.png`.

**Status:** [x] done

**Note for later sub-tasks:** `ReadMe.md` is now about 330 lines (the target of about 200 was not reached because the `<details>` blocks keep all text). New heading numbering: 1 Executive Summary (with `### MQTT telemetry and control`, `### Live dashboard`, `### WiFi modes`), 2 System Hardware Requirements, 3 Hardware Design & Wiring Specification, 4 Boot & Communication Lifecycle, 5 Troubleshooting, 6 Range & RF Optimization Rules, 7 Documentation, Appendix A. The former "4. Firmware Architecture & Threading Model" pointer section was removed (its link to `doc/Architecture.md` is now in "7. Documentation"). Intro: FAQ wording of "What is LoRaIoT and how does it work?" (paragraphs 1 and 2 verbatim) plus connecting sentences; "enterprise-grade" and "transparently extend local IP networks" removed; the former technical sentences are kept in `<details>` "Technical highlights". Flow image `doc/images/LoRaIoT-flow.png` placed below the intro. Mermaid diagram, ownership table and the two curl examples stay visible; the flow lists, notes, dashboard screenshots, WiFi curl + notes and the pin mapping table are in `<details>`. Added text: the `<LOCAL-IP>` / `<REMOTE-IP>` definition (condensed from the removed section 6), a link to the RestApi Worked Examples, the paragraph "LoRaIoT is a data bridge between a REST or MQTT client ... never needs to know that REMOTE nodes exist" (wording from the FAQ), and the "7. Documentation" section (includes `doc/LoggingAnalysis.md` as a developer doc, since the user confirmed it is live). MQTT pointer now links `doc/Architecture.md#85-topic-scheme-and-payloads` and `doc/RestApi.md#get-apimqttconfig`; the claim "validation rules, and configuration defaults" was dropped from the pointer text because 8.5 to 8.7 do not cover configuration defaults. The glossary hint line moved up below the image. Anchors `#wifi-modes` and `#appendix-a-glossary-of-abbreviations` still resolve. The ReadMe summary examples still use `GridPower` / `DcDc` (sub-task 14).

---

## Sub-task 12 - Correct the endpoint labels in the flow diagram

**Intent:** the image shows `GET .../api/lora/sensor/data` and `POST .../api/lora/control`. The real endpoints are `GET /api/lora/nodes/sensor` and `POST /api/lora/nodes/control`. The image is now the first thing a newcomer sees.

**Expected outcomes:** `doc/images/LoRaIoT-flow.drawio` carries the correct labels.

**Todo list:**
1. Correct the labels in `LoRaIoT-flow.drawio` (Draw.io mode).
2. The user re-exports `LoRaIoT-flow.png`. Wait for the user before continuing.

**Relevant context:** `doc/images/LoRaIoT-flow.drawio`, `doc/images/LoRaIoT-flow.png`.

**Status:** [x] done (source corrected, waiting for the user's PNG re-export)

**Note for later sub-tasks:** only the edge label with `id="11"` (LOCAL to Home Assistant) in `doc/images/LoRaIoT-flow.drawio` was changed: `GET http://<Local>/api/lora/sensor/data` became `GET http://<Local>/api/lora/nodes/sensor` and `POST http://<Local>/api/lora/control` became `POST http://<Local>/api/lora/nodes/control`. The other two endpoint labels were already correct (`GET http://<Remote>/api/lora/nodes/control`, `POST http://<Remote>/api/lora/sensor/data`). `doc/images/LoRaIoT-flow.png` is NOT yet re-exported; it still shows the old labels until the user exports it. Sub-task 13 must not be started before the user confirms the export.

---

## Sub-task 13 - Fix references and verify

**Intent:** no dead links, no duplicated content.

**Expected outcomes:** all links and anchors in the touched documents resolve.

**Todo list:**
1. `doc/Architecture.md`: change both `../README.md` links (lines 5 and 310) to `../ReadMe.md`, and adjust the wording if the target text moved.
2. `FAQ.md`: check the `Architecture.md` anchor `#7-describe-crawl---schema-discovery-before-data-is-visible` and its `doc/RestApi.md` links.
3. Check the cross-links added in sub-tasks 5 to 8.
4. Grep all live `.md` files for `README.md`, `IoTProtocol`, `Feasibility`, `Learnings` and moved section names.
5. For each moved block, grep a distinctive sentence to confirm it exists exactly once.
6. Check that images and Mermaid blocks still resolve (paths relative to each document).
7. Report remaining references in frozen files to the user without changing them.

**Relevant context:** all `.md` files.

**Status:** [x] done

**Note for later sub-tasks:** fixes made: `doc/Architecture.md` line 4-5 intro (`README.md` link became `ReadMe.md`, "quick-start examples" became a link to `RestApi.md#worked-examples`, because the quick-start section no longer exists in the ReadMe); line 310 (`README.md` link for the 90 s keep-alive rule became a link to Architecture 9.4); `doc/Glossary.md` line 3 (`doc/LoRaProtocol.md` relative to `doc/` was a dead link, now `LoRaProtocol.md`). Automated check (temporary script in `%TEMP%`, not in the repo) of every Markdown link and anchor in `ReadMe.md`, `FAQ.md` and the 7 files in `doc/` that are live (`RestApi`, `Architecture`, `LoRaProtocol`, `Glossary`, `OpenAPI`, `NanopbGeneration`, `LoggingAnalysis`): all file targets and all `#anchor` targets resolve after the fixes (GitHub slug rules emulated, not verified in a rendered view). Images exist (`dashboard-local.webp`, `dashboard-remote.webp`, `LoRaIoT-flow.png`), `<details>`/`<summary>` tags are balanced. Open: `doc/Glossary.md` line 96 says "mentioned in the feasibility study" (no link, refers to the frozen study); `doc/LoggingAnalysis.md` line 22 has a source reference `src/util/src/LogBuffer.cpp:212` that is not a Markdown link target. Sanitization rule of topic levels exists in both `doc/RestApi.md` ("Sanitization Rule for Topic Levels") and `doc/Architecture.md` 8.5, with mutual links (kept; the two texts describe the rule from the REST and the MQTT side).

---

## Sub-task 14 (last) - Unify the example names and verify the MQTT facts

**Intent:** (a) the same example everywhere, using the names of `IoTProtocol.md`: sensor `Env-Sensor` with `Temperature` and `Humidity`, control `Power-Supply` with `Voltage`, `Current` and `Pwr`. (b) Check whether the live MQTT description (section 8 of `doc/Architecture.md`) misses facts that exist only in the frozen `MqttFeasibility.md`.

**Expected outcomes:** (a) `GridPower`, `SensorA`, `DcDc`, `Charger`, `VoltageSet`, `Temp` no longer appear in examples. (b) A gap list for the user to decide on. Nothing from (b) is copied without the user's decision.

**Todo list (a - example names):**
1. Grep the old names across `ReadMe.md`, `FAQ.md`, `doc/RestApi.md`, `doc/LoRaProtocol.md` (sections 12 and 15 diagrams) and `doc/Architecture.md`.
2. Replace them consistently, including example values (for example `335/100 = 3.35 V`) and derived MQTT topics.
3. Keep names that are real data in screenshots (`doc/images/dashboard-*.webp`), because those are images of a running system.
4. Re-run the duplicate and link checks of sub-task 13.

**Todo list (b - MQTT fact verification, read-only on `MqttFeasibility.md`):**
1. Compare `MqttFeasibility.md` section 4 (topic pattern, payload formatting, example topic tree, availability and LWT, per-peer status hand-off, `/set` flow, limits and known open points) and section 7 (REST endpoints, NVS parameters) against the live documents.
2. Verify each candidate fact against the source code (`MqttService.cpp` and related constants) so that only facts matching the implementation are proposed.
3. Present a gap list (fact, where it is in the frozen file, verified yes or no, suggested target) and let the user decide which facts, if any, to add.

**Relevant context:** all documents listed above; `doc/MqttFeasibility.md` (read only); `src/service/src/MqttService.cpp`.

**Status:** [x] done

**Result:** (a) Examples unified to `Env-Sensor` (Temperature, Humidity, AirPressure text, Firmware with unit "vers.", Daylight bool) and `Power-Supply` (Voltage, Current, Pwr text) in `ReadMe.md`, `FAQ.md`, `doc/RestApi.md`, `doc/Architecture.md`, `doc/LoRaProtocol.md`, `src/LoRaIoT/src/openapi_json.h` (shortened example) and the two body comments in `src/service/src/RestService.cpp`. `registry_rev` is 6 for Env-Sensor and 4 for Power-Supply. Left unchanged on purpose: screenshot data, `doc/NanopbGeneration.md` (real test output), schema comments in `doc/LoRaProtocol.md` lines 279 and 292 (mirror `loraiot.pb.h`). (b) Verified facts added to `doc/Architecture.md`: new sub-section "/set processing and limits" in 8.6 and new 8.8 "Session, availability and hand-off details". `RestApi.md` field row `unit` reworded (approved). `MqttFeasibility.md` untouched.

---

## Open questions

None. Decisions on the 14 sub-tasks and their order are confirmed unless the user objects after reading this file.
