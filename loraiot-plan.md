# LoRaIoT Implementation Plan

## Overview

Implement a dual-role ESP32 LoRa gateway firmware from the existing scaffold in `ESP/LoRaIoT/`.
Both ESPs run from a **single shared codebase**. Role (Gateway A or Gateway B) is selected once
in the WiFiManager captive portal and persisted in NVS. The `/reset` endpoint clears both
WiFi credentials and role assignment.

**Gateway A (Local):** WiFi Station client → REST API WebServer → LoRa TX/RX
**Gateway B (Remote):** WiFi SoftAP for IoT devices → LoRa TX/RX

**Build sequence:** Echo test first (validates LoRa link + software handshake), then application logic.

**Reference patterns** (already implemented in `ESP/SerialController/`):
- Serial2 ownership: `ModbusTransport.h/.cpp` (FreeRTOS request/response queue, pinned Core 0) — **used as a FreeRTOS/Serial2 ownership template only; the protocol written to Serial2 in LoRaIoT is the E220 UART frame, not Modbus**
- Shared state: `ConverterState.h/.cpp` (mutex-protected struct, getters/setters)
- NVS persistence: `LogBuffer.cpp` (`Preferences` namespace + key)
- WiFiManager custom parameter: `Server.cpp` setupServer() log-level inject pattern

---

## Milestone 1 — Research: LoRa Half-Duplex Software Handshake Protocol

**Status:** [ ] pending

**Intent:**
Before writing any LoRa transport code, define the exact software-level handshake protocol.
The E220 provides no ACK at the RF layer for transparent mode — the application must implement
its own request/response correlation to achieve reliable, low-latency, no-data-loss operation.
This milestone produces a protocol specification document that all subsequent milestones implement against.

**Open questions to answer (research tasks):**

1. **Turn-taking policy:** Because LoRa is half-duplex, can Gateway A and Gateway B ever
   transmit simultaneously (causing a collision), or is there a natural request/response
   alternation that prevents this? Define the rule: "only the node that last received a frame
   may transmit next" (ping-pong) vs. unsolicited push allowed.

2. **ACK frame type:** The frame protocol (Feasibility.md §6) defines Type=0x04 as
   "Heartbeat / ACK". Define exactly: does every data frame require an ACK, or only
   command frames? What is the ACK payload (empty, or echo of SeqNo)?

3. **Timeout and retry policy:** At 62.5 kbps, RTT min ≈ 50 ms (Feasibility.md Appendix A §A.2.1).
   What is the application-level ACK timeout? How many retries before declaring link down?
   (Not defined anywhere in current docs — must be decided.)

4. **Sequence number rollover:** SeqNo is 2 bytes (0–65535). What happens when it wraps?
   Is the receiver expected to handle it gracefully (modular arithmetic)?

5. **Echo test frame definition:** Define the exact payload for the echo test.
   Proposed: Gateway A sends Type=0x04, payload = ASCII string (e.g. "Hello LoRa!"),
   Gateway B reverses the payload string and replies with Type=0x04, same SeqNo.
   Confirm or revise.

6. **CRC algorithm:** CRC16 is specified but the variant (CRC-16/CCITT, CRC-16/IBM, etc.)
   is not. This must be fixed before both sides can interoperate.

**Expected Outcomes:**
- `doc/LoRaProtocol.md` created, defining:
  - Turn-taking rule
  - ACK semantics (which frames require ACK, ACK payload)
  - Timeout values and retry count
  - SeqNo rollover handling
  - Echo test frame format
  - CRC16 variant (name + polynomial)

**Relevant Context:**
- `doc/Feasibility.md` §3.2 (AUX timing), §6 (frame format), Appendix A §A.2.1 (RTT calculation)
- `ESP/SerialController/src/modbus/src/ModbusTransport.cpp` — FreeRTOS/Serial2 ownership template only (per-call timeout pattern: 1500 ms, no retry in transport layer). The Modbus protocol itself is irrelevant; only the queue/task wiring is reused.

---

## Milestone 2 — Role Selection: NVS-Persisted Gateway Role via WiFiManager Portal

**Status:** [ ] pending

**Intent:**
Add a "Gateway Role" selector to the WiFiManager captive portal alongside the existing
log-level selector. Store the selected role in NVS. Expose it via a `GatewayRole` enum
accessible throughout the firmware. The `/reset` endpoint must clear the role from NVS.

**Expected Outcomes:**
- WiFiManager captive portal shows a "Gateway Role" dropdown: `UNASSIGNED`, `GATEWAY_A`, `GATEWAY_B`
- Selected role persists in NVS (namespace `loraiot_prefs`, key `role`) across reboots
- `GatewayRole` enum and global accessor available to all compilation units
- On boot, if role == `UNASSIGNED`, StatusLed shows NO_DEVICE until portal is completed
- `GET /reset` clears both WiFi credentials and role from NVS before rebooting

**Todo List:**
- [ ] Create `src/LoRaIoT/src/GatewayRole.h` - `enum class GatewayRole { UNASSIGNED, GATEWAY_A, GATEWAY_B }` + `extern GatewayRole gatewayRole`
- [ ] In `Server.cpp` setupServer(): add role `<select>` injection into portal HTML (same pattern as log-level selector)
- [ ] In the save-params callback: read role param, call `Preferences` to persist to NVS (`loraiot_prefs` / `role`)
- [ ] In `Application.cpp` applicationSetup(): load role from NVS before `setupServer()`, set `gatewayRole` global
- [ ] Update `handleReset()` in `Server.cpp`: open `loraiot_prefs` in write mode and clear `role` key before calling `wm.resetSettings()`
- [ ] Add `GET /api/role` endpoint in `RestService::registerRoutes()` returning `{"role":"GATEWAY_A"}`
- [ ] Update StatusLed: if `gatewayRole == UNASSIGNED` after WiFi connects, set `NO_DEVICE` state

**Relevant Context:**
- `ESP/LoRaIoT/src/LoRaIoT/src/Server.cpp` setupServer() — log-level portal inject pattern to copy
- `ESP/SerialController/src/SerialController/src/LogBuffer.cpp` — NVS persist/load pattern (`Preferences`, `prefs.begin(namespace)`, `prefs.getString/putString`)
- NVS namespace for this project: `loraiot_prefs` (distinct from SerialController's `sc_prefs`)

---

## Milestone 3 — LoRa Transport Layer: E220 Init + Serial2 Owner Task

**Status:** [ ] pending
**Prerequisite:** Milestone 1 (protocol spec must define ACK timeout for transport layer)

**Intent:**
Implement the E220 hardware initialization sequence and a dedicated FreeRTOS task that
is the sole owner of Serial2 (UART2). All other code sends/receives LoRa frames by posting
to this task's request queue — never touching Serial2 directly. This is the direct equivalent
of `ModbusTransport`, but only its FreeRTOS/Serial2 ownership wiring. The protocol written to Serial2 is the E220 UART frame defined in Feasibility.md §6 — not Modbus.

**Expected Outcomes:**
- `src/lora/src/LoRaTransport.h/.cpp` created
- On boot, E220 is configured to 115200 baud / 62.5 kbps / channel 0x17 / 22 dBm
- `loraTransportTask` (Core 0, priority 2) is the sole owner of Serial2 (pins 16/17)
- `sendFrame(uint8_t* frame, size_t len, uint16_t& seqNo)` posts request, blocks for ACK
- `setReceiveCallback(fn)` registers a callback invoked by the task when a frame arrives
- E220 config errors (FF FF FF response) logged and retried up to 3 times; FAULT state on failure
- Mode switching (Mode 3 for config, Mode 0 for normal) uses correct AUX timing per Feasibility.md §3.2

**Todo List:**
- [ ] Create `src/lora/library.properties`
- [ ] Create `src/lora/src/LoRaTransport.h` — define `LoRaRequest`, `LoRaResponse` structs; class API
- [ ] Implement `configureE220()` private method: switch UART to 9600, enter Mode 3, write config command (0xC0 0x00 0x08 ...), check response (not FF FF FF), exit Mode 3, switch back to 115200, wait for AUX rising edge
- [ ] Implement `setMode(m1, m0)` private method: set GPIO M0/M1, wait for AUX rising edge ≥ 2 ms
- [ ] Implement `sendLoRaFrame(uint8_t*, size_t)` private method: poll AUX HIGH → write atomically → wait AUX LOW → wait AUX HIGH → 50 ms guard (per Feasibility.md §7.1)
- [ ] Implement `loraTransportTask` loop: dequeue request, execute send, enqueue response; OR receive frame from UART, invoke callback
- [ ] Pin task to Core 0 (`xTaskCreatePinnedToCore`, priority 2, 4096 stack)
- [ ] Define GPIO constants: PIN_M0=4, PIN_M1=2, PIN_AUX=15 (verify against wiring in Feasibility.md §3.1)
- [ ] Add `src/lora/` to `ESP/LoRaIoT/src/LoRaIoT/src/Application.cpp` — construct `LoRaTransport` and call `begin()`
- [ ] Set StatusLed to NO_DEVICE if config fails, READY if succeeds

**Relevant Context:**
- `ESP/SerialController/src/modbus/src/ModbusTransport.h/.cpp` — FreeRTOS/Serial2 ownership template to replicate (task pinning, request/response queue structs, per-call reply queue). The Modbus protocol is not involved; only the concurrency wiring applies.
- `doc/Feasibility.md` §3.1 (pin mapping), §3.2 (AUX semantics), §3.3 (config command), §7.1 (sendLoRaFrame code sample)
- GPIO pin mapping from Feasibility.md §3.1: M0=GPIO4, M1=GPIO2, AUX=GPIO15, RX2=GPIO16, TX2=GPIO17

---

## Milestone 4 — Frame Codec: Framing, CRC, and LoRaState Shared State

**Status:** [ ] pending
**Prerequisite:** Milestone 1 (CRC variant must be decided), Milestone 3 (transport layer exists)

**Intent:**
Implement the frame encode/decode layer (the 8-byte header + payload + CRC16 defined in
Feasibility.md §6) and the mutex-protected `LoRaState` shared state struct that holds
the most recently received frame, link health, and sequence counters.

**Expected Outcomes:**
- `src/lora/src/LoRaFrame.h/.cpp` created — `encodeFrame()` and `decodeFrame()` functions
- CRC16 is computed over Type + SeqNo + PayloadLen + Payload (NOT the 0xAA 0x55 header)
- `decodeFrame()` returns false on header mismatch or CRC failure; all failures logged
- `src/lora/src/LoRaState.h/.cpp` created — mutex-protected struct with: last received payload, last SeqNo, last RSSI (if available), TX count, RX count, error count, link status
- Frame type enum defined: `SENSOR=0x01`, `HTTP_REQUEST=0x02`, `HTTP_RESPONSE=0x03`, `HEARTBEAT_ACK=0x04`

**Todo List:**
- [ ] Decide and document CRC16 variant (from Milestone 1 protocol spec)
- [ ] Create `src/lora/src/LoRaFrame.h` — frame constants, `LoRaFrameType` enum, `encodeFrame()` / `decodeFrame()` signatures
- [ ] Implement `encodeFrame()`: write 0xAA 0x55, type, seqNo (big-endian), payloadLen, payload, CRC16
- [ ] Implement `decodeFrame()`: check header bytes, verify CRC, return parsed fields
- [ ] Create `src/lora/src/LoRaState.h/.cpp` — `LoRaState` class mirroring `ConverterState` pattern (mutex, getters, setters)
- [ ] Add `LoRaState loraState` global (analogous to `converterState` in SerialController)
- [ ] Update `LoRaTransport` receive path to call `decodeFrame()` and update `loraState`

**Relevant Context:**
- `doc/Feasibility.md` §6 (frame layout, CRC scope)
- `ESP/SerialController/src/service/src/ConverterState.h/.cpp` — mutex-protected state pattern
- `ESP/SerialController/src/SerialController/src/ConverterStateGlobal.h` — global declaration pattern

---

## Milestone 5 — Echo Test: End-to-End Validation with Two ESPs on PC

**Status:** [ ] pending
**Prerequisite:** Milestones 1, 2, 3, 4

**Intent:**
With both ESPs connected to a PC via USB, implement and validate the echo test defined
in Milestone 1. Gateway A sends a text message over LoRa; Gateway B receives it, reverses
the payload string, and sends it back; Gateway A receives the reversed echo. This validates:
- E220 hardware wiring on both units
- AUX timing and send/receive sequencing
- Frame encoding, CRC, and sequence number handling
- The software ACK/handshake protocol defined in Milestone 1
- That no frames are dropped under repeated sends

**Expected Outcomes:**
- Gateway A REST endpoint `POST /api/lora/echo` accepts `{"message":"Hello LoRa!"}`, sends it
- Gateway B automatically reverses received payload and replies (no REST call needed on B side)
- Gateway A REST endpoint `GET /api/lora/echo/result` returns `{"sent":"Hello LoRa!","received":"!aro L olleH","seqNo":1,"roundTripMs":…,"success":true}`
- Repeated calls increment SeqNo; no duplicate or dropped frames observed in `GET /api/log`
- Both ESPs' `GET /api/lora/status` show TX count, RX count, error count

**Todo List:**
- [ ] Add echo RX handler in LoRaTransport receive callback: if role == GATEWAY_B and frame type == HEARTBEAT_ACK, reverse payload bytes, encode response frame with same SeqNo, send back
- [ ] Add echo TX logic: POST /api/lora/echo sends frame, blocks (with timeout from protocol spec) for response, stores result in LoRaState
- [ ] Register `POST /api/lora/echo` and `GET /api/lora/echo/result` in `RestService::registerRoutes()`
- [ ] Register `GET /api/lora/status` endpoint returning TX/RX/error counts and last SeqNo from LoRaState
- [ ] Add echo test results and link health to WebSocket 1-second broadcast JSON
- [ ] Manual validation: connect both ESPs to PC, open serial monitors on both, send via REST, verify reversed echo and log output
- [ ] Stress test: send 100 sequential echo frames, verify zero errors in `/api/log` and TX count == RX count on both sides

**Relevant Context:**
- `doc/LoRaProtocol.md` (created in Milestone 1) — ACK timeout, SeqNo rules
- `ESP/LoRaIoT/src/service/src/RestService.cpp` — endpoint registration pattern
- `ESP/SerialController/src/service/src/RestService.cpp` — PUT body handler signature pattern

---

## Milestone 6 — Dual-Role Application Logic: Gateway A and Gateway B Behavior

**Status:** [ ] pending
**Prerequisite:** Milestone 5 (echo test passing = LoRa link confirmed working)

**Intent:**
Implement the full role-specific behavior for both gateways, building on the validated LoRa link.
Gateway A becomes the WebServer/REST API node connected to the local LAN. Gateway B becomes
the SoftAP that IoT devices connect to. Both run from the same firmware binary; behavior
branches on `gatewayRole` at runtime.

**Expected Outcomes:**
- Gateway B starts a WiFi SoftAP (SSID: `LoRaIoT-AP`, IP 192.168.4.1) in addition to WiFi STA
  (note: ESP32 can run STA+AP simultaneously; Gateway B's STA is used to reach the LoRa uplink)
- Gateway A (STA mode) receives LoRa frames from Gateway B and forwards payload to its local REST API state
- Gateway B receives REST calls from connected IoT devices at `192.168.4.1/api/*` and forwards them as LoRa frames to Gateway A
- `LoRaState` is updated on both sides; WebSocket broadcast reflects the latest forwarded state
- StatusLed READY state set once LoRa link is up AND WiFi role is established

**Todo List:**
- [ ] In `applicationSetup()`: branch on `gatewayRole` — Gateway A initializes as WiFi STA only; Gateway B initializes WiFi STA + starts `WiFi.softAP("LoRaIoT-AP")` after WiFiManager connects
- [ ] Create `src/service/src/GatewayAService.h/.cpp` — handles forwarding: LoRa RX → update LoRaState → REST response
- [ ] Create `src/service/src/GatewayBService.h/.cpp` — handles forwarding: REST RX from IoT device → encode LoRa frame → send → wait for ACK
- [ ] Define the forwarded message schema (what IoT device sends to Gateway B's REST API, and what Gateway A exposes) — derive from Feasibility.md §5.2 component roles
- [ ] Register role-specific REST routes: Gateway B exposes IoT-facing `/api/iot/*`; Gateway A exposes LAN-facing `/api/upstream/*`
- [ ] Update WebSocket broadcast: include role, link status, and last forwarded payload in JSON snapshot
- [ ] Update index_html.h placeholder with a real UI (copy and adapt from SerialController's index_html.h pattern — single self-contained HTML with inline JS WebSocket client)

**Relevant Context:**
- `doc/Feasibility.md` §5.1 architecture diagram, §5.2 component roles
- `ESP/SerialController/src/SerialController/src/index_html.h` — PROGMEM HTML pattern
- `ESP/SerialController/src/service/src/WebSocketService.cpp` — broadcast pattern

---

## Milestone 7 — Performance Tuning and Link Quality Measurement

**Status:** [ ] pending
**Prerequisite:** Milestone 6 (full application logic working)

**Intent:**
Measure and optimize throughput, latency, and error rate of the LoRa link under realistic
load. The echo test from Milestone 5 provides the baseline; this milestone runs structured
tests and tunes parameters to hit the "low latency, high throughput, no data loss" goals.

**Open questions / research tasks:**
1. Is 62.5 kbps optimal for the target deployment range, or should the air rate be configurable at runtime via REST?
2. Can the 50 ms RF guard delay in `sendLoRaFrame()` be reduced empirically by measuring actual AUX timing?
3. Should LBT (REG3 bit 4) be enabled for production use to prevent collisions in a dense 433 MHz environment?
4. Is the 400-byte FIFO always sufficient for a single 190-byte payload frame, or does concurrent WiFi activity on the ESP32 cause UART timing glitches?

**Expected Outcomes:**
- REST endpoint `POST /api/lora/benchmark` runs N echo frames and returns: min/avg/max RTT, error count, throughput in bytes/s
- Air rate (REG0 bits 2-0) and TX power (REG1 bits 1-0) configurable at runtime via `PUT /api/lora/config` without reboot (uses Temporary Write Command 0xC2)
- Guard delay constant extracted to a named constant and tuned based on measured AUX timing
- Results documented in `doc/Feasibility.md` §8 matrix update (actual measured values)

**Todo List:**
- [ ] Implement `POST /api/lora/benchmark` endpoint (N configurable, default 100)
- [ ] Implement `PUT /api/lora/config` endpoint — validate params, write via 0xC2 (temporary), update LoRaState
- [ ] Measure round-trip timing at 62.5 kbps for 10B, 50B, 100B, 190B payloads; record in `doc/LoRaProtocol.md`
- [ ] Tune `LORA_RF_GUARD_DELAY_MS` constant based on measurements
- [ ] Decide LBT policy (research task from above); document decision in `doc/Feasibility.md`
- [ ] Update `doc/Feasibility.md` §8 matrix with measured vs. theoretical values

**Relevant Context:**
- `doc/Feasibility.md` §3.3 (REG0/REG1 bit fields, 0xC2 temporary write command)
- `doc/Feasibility.md` §4.1 (throughput table — theoretical values to compare against)
- `doc/LoRaProtocol.md` (created in Milestone 1)
