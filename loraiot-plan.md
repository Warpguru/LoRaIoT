# LoRaIoT Implementation Plan

## Overview

Implement a dual-role ESP32 LoRa gateway firmware. The project lives at the repository root
(`LoRaIoT.ino`, `LoRaIoT.cpp`, `platformio.ini`, `src/`). `ESP/` is a symlink to a read-only
reference project - never write to it.
Both ESPs run from a **single shared codebase**. Role (LOCAL or REMOTE) is selected once
in the WiFiManager captive portal and persisted in NVS. The `/reset` endpoint clears both
WiFi credentials and all NVS settings.

**LOCAL node:** WiFi STA client → REST API WebServer → single E220 LoRa radio (TX+RX)
**REMOTE node:** WiFi SoftAP for IoT devices → single E220 LoRa radio (TX+RX)

**Build sequence:** Plain-text ping-pong PoC first (M0, validates hardware), then protocol
research and decisions (M1), then NVS provisioning (M2), then full framed transport (M3-M5),
then application logic (M6), then tuning (M7).

**Reference patterns** (from `ESP/SerialController/` — layout and utility classes only, no protocol reuse):
- Serial2 ownership: FreeRTOS single-owner task with a request/response queue
- Shared state: `ConverterState.h/.cpp` (mutex-protected struct, getters/setters)
- NVS persistence: `LogBuffer.cpp` (`Preferences` namespace + key)
- WiFiManager custom parameter: `Server.cpp` setupServer() log-level/tier inject pattern

---

## Architecture Decisions (resolved in M0/M1 research)

These decisions are settled and must not be re-opened without updating this section and
`doc/LoRaProtocol.md`.

| Topic | Decision |
|---|---|
| Radio topology | Single E220 per node (half-duplex, shared TX+RX) |
| Addressing mode | E220 fixed transmission (REG3=0x40, bit 6); LOCAL=0x01, REMOTEs=0x02-0xFE; 0x00 invalid; 0xFF broadcast (E220 0xFFFF). The E220 never delivers the sender address, so SRC and DST are in the frame header |
| Channel | Single shared channel (default 0x17 = 433.125 MHz); NVS-persisted; portal-configurable |
| E220 registers | REG0=0xE7 (115200 8N1, 62.5 kbps), REG1=0x00, REG3=0x40; CRYPT_H/L = shared key from NVS |
| Message set | DATA, DATA_ACK, REQUEST, RESPONSE, BEACON, PING, ACK (`doc/LoRaProtocol.md` §5); RESPONSE is the implicit ACK of REQUEST |
| Medium access | Either side may transmit at any time (REMOTE may send unsolicited alerts); no scheduler; collisions resolved by ACK + random backoff + retry |
| REMOTE discovery | Beacon-based: REMOTE beacons every 5 s until first frame from LOCAL, then every 60 s |
| Beacon self-healing | REMOTE reverts to 5 s beacon rate if no frame from LOCAL for 2 min |
| LOCAL registry | LRU timestamp per REMOTE address; purge after 180 s without any frame |
| Reboot detection | Random `boot_id` in BEACON and PING; receiver resets per-SRC sequence and cache state when it changes |
| Collision handling | Reliable messages: timeout 200 ms (REQUEST 1000 ms), random backoff 20-200 ms; retries: DATA_ACK 3, PING/REQUEST 1 |
| LBT | Disabled (REG3 bit 4 = 0); software turn management is sufficient |
| Payload encoding | Plain text for M0 PoC; protobuf (nanopb) specified in `doc/LoRaProtocol.md` §10; generated sources are committed in `src/lora/src/nanopb/` but not yet used by firmware code; JSON only on REST |
| Message size | One message = one frame (max 186 B payload); no fragmentation |
| Data model | `Data{device, repeated Measurement{metric_id, sint32 value}, age_ms, alert}`; a `Device{name, repeated Metric{id, name, unit, scale}}` definition is sent once via `Describe` and cached by LOCAL (no compile-time metric list); device 0 = the node itself, 1..N = devices posting via the REMOTE SoftAP |
| Protocol simplifications | PING and BEACON share one `Beacon` message (optional fields absent in a PING); `Read` returns all metrics of one device as `Data`; `Status` is only OK / BAD_REQUEST; no `SetChannel` (channel is changed per node, not over the air) |
| CRC variant | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection), check value 0x29B1 for "123456789"; scope VER..PAYLOAD |
| Security (v1) | None beyond the E220 CRYPT key; SRC is spoofable |

---

## Milestone 0 — PoC: Plain-Text Ping-Pong (Hardware Validation)

**Status:** [x] completed
**Prerequisite:** None

**Intent:**
Validate that both ESPs and both E220 modules are wired correctly and can exchange data
over the air before any framing, CRC, or protocol work begins. Uses the minimum possible
code: raw plain-text strings over Serial2, no frame header, no CRC, no addressing. Both
ESPs run identical firmware; the role and channel are selected from the configuration portal).

LOCAL sends `"Local send: N\n"` (N = incrementing integer) every 2 seconds.
REMOTE receives it, replies `"Remote reply: N\n"` with the same N.
LOCAL logs both the sent string and the received echo to `GET /api/log`.

This milestone is deliberately minimal. No frame codec, no SeqNo, no CRC, no addressing,
no NVS role selection. E220 is configured in transparent mode (REG3=0x00) with both nodes
on the same channel. The only new code is a `LoRaTransport` raw send/receive skeleton
(without framing) and a trivial ping-pong loop in `applicationLoop()`.

**Expected Outcomes:**
- Both ESPs compile and flash from the same codebase, the LOCAL and REMOTE roles are assumed persisted in NVS
- LOCAL sends incrementing ping every 2 s; REMOTE replies with same N
- LOCAL logs `"Sent: N"` and `"Received reply: N"` visibly in `GET /api/log`
- REMOTE logs `"Received ping: N"` and `"Sent reply: N"`
- 10 consecutive exchanges complete with zero missed replies
- Serial monitors on both ESPs show the exchange in real time

**Todo List:**
- [x] Create `src/lora/library.properties`
- [x] Create `src/lora/src/LoRaTransport.h/.cpp` — minimal skeleton: `begin()`, `sendRaw(uint8_t*, size_t)`, `setReceiveCallback(fn)`, `loraTransportTask` (TASK_CREATE, priority 2, 4096 stack)
- [x] `begin()`: configure E220 to 115200 / 62.5 kbps / channel as retrieved from NVS / 22 dBm / transparent mode (REG3=0x00); set Mode 0 for normal operation
- [x] `loraTransportTask` loop: read Serial2 until `'\n'`; invoke receive callback with received bytes; dequeue and send outgoing sends
- [x] In `Application.cpp`: code conditional on LOCAL or REMOTE role in `applicationLoop()` for ping-pong logic
- [x] LOCAL side: every 2 s, call `loraTransport.sendRaw("Local send: N\n")`; increment N; log sent string
- [x] REMOTE side: receive callback logs received string; calls `loraTransport.sendRaw("Remote reply: N\n")`
- [x] Use `TASK_CREATE` macro for all task creation (never `xTaskCreatePinnedToCore` directly)
- [x] Validate: both serial monitors show bidirectional exchange; `GET /api/log` on LOCAL shows 10+ complete exchanges

**Relevant Context:**
- `doc/Feasibility.md` §3.1 (pin mapping), §3.2 (AUX semantics), §3.3 (config command), §7.1 (sendLoRaFrame code sample)
- GPIO: M0=GPIO4, M1=GPIO2, AUX=GPIO15, RX2=GPIO16, TX2=GPIO17
- `src/util/src/TaskUtils.h` — TASK_CREATE macro

---

## Milestone 1 — Research & Protocol Design

**Status:** [x] completed (technical design artifact; implementation of the protobuf schema deferred)
**Prerequisite:** Milestone 0 (hardware confirmed working)

**Intent:**
With hardware validated, define the complete software-level protocol before writing any
framing or application code. The deliverables are `doc/LoRaProtocol.md` (specification) and
the protobuf schema it contains (§10 of the specification). The schema is a technical design
artifact: it is stored as `src/lora/proto/loraiot.proto`, and the nanopb sources generated from it
(plus the nanopb runtime) are committed in `src/lora/src/nanopb/`; regeneration and tests are described in
`doc/NanopbGeneration.md`. No firmware code uses them yet.

**Resolved decisions (all documented in `doc/LoRaProtocol.md`):**

| # | Topic | Section |
|---|---|---|
| 1 | Frame type set: DATA, DATA_ACK, REQUEST, RESPONSE, BEACON, PING, ACK (RESPONSE is the implicit ACK of REQUEST) | §5 |
| 2 | Frame header with VER, SRC, DST (the E220 never delivers the sender address); max payload 186 B | §3, §4 |
| 3 | E220 configuration facts checked against the manual (REG0=0xE7, REG3=0x40, address rules) | §2, §3 |
| 4 | CRC-16/CCITT-FALSE, scope VER..PAYLOAD, check value 0x29B1, worked hex frame | §4.1, §4.2 |
| 5 | SeqNo rules, duplicate detection, `boot_id` reboot detection | §6 |
| 6 | Timeouts, retries, random backoff, broadcast reply window | §7 |
| 7 | Beacon / discovery state machine and LOCAL registry | §8 |
| 8 | Medium access (no scheduler, unsolicited REMOTE data allowed) | §9 |
| 9 | Protobuf payloads, self-defined metrics (`Describe`) with scaled integers, device identity (0 = node itself, 1..N = SoftAP devices) | §10 to §13 |
| 10 | Channel change: not supported over the air | §14 |
| 11 | REST mapping, including live (near real time) reads | §17 |
| 12 | Security v1: E220 CRYPT key only | §16 |

**Definition of Done:**
- [x] `doc/LoRaProtocol.md` written with all sections above
- [x] CRC check value and one worked example frame in hex included
- [x] Reviewed and accepted by the project owner
- [x] `doc/Feasibility.md` §6 and `AGENTS.md` updated to reference `doc/LoRaProtocol.md` as the frame specification (REG0=0xE7 and payload limit corrected)

**Items deliberately deferred to Milestone 5 (hardware verification, listed in specification §18):**
- Whether the 3 E220 routing bytes count toward the 200 B sub-packet
- Whether a module with its own address receives a frame sent to the broadcast address `0xFFFF`
- Final values of `LORA_ACK_TIMEOUT_MS`, the backoff window and the broadcast reply window

**Relevant Context:**
- `doc/E220-400T22D_UserManual_EN_v1.2.pdf` (authoritative for module behaviour)
- `doc/Feasibility.md` §3.2 (AUX timing), Appendix A §A.2.1 (RTT calculation)
- Architecture Decisions table at top of this plan

---

## Milestone 2 — NVS Provisioning: Role, Channel, Address, Log Level, CRYPT Key

**Status:** [ ] pending
**Prerequisite:** Milestone 0 (compile-time role toggle exists; this replaces it with runtime provisioning)

**Intent:**
Replace the M0 compile-time `POC_ROLE_LOCAL` / `POC_ROLE_REMOTE` toggle with a fully
runtime-configurable system. The WiFiManager captive portal gains selectors
(LoRa channel, node address, E220 CRYPT key) alongside the existing tier and log-level selectors.
All settings are NVS-persisted and updatable at runtime via REST API.

**NVS parameters (all in namespace `loraiot_prefs`):**

| NVS key | Type | Portal | REST | Default | Valid range |
|---|---|---|---|---|---|
| `tier` | string | yes (existing) | yes | `UNSET` | `LOCAL`, `REMOTE` |
| `logLevel` | string | yes (existing) | yes | `INFO` | `TRACE`…`ERROR` |
| `loraChannel` | uint8 | yes (new) | yes | `0x17` (23) | 0-31 (433.0-433.475 MHz) |
| `loraAddr` | uint8 | yes (new, REMOTE only) | yes | `0x02` | 2-254 |
| `loraKey` | uint16 | yes (new) | yes (write-only) | `0x0000` (off) | 0-65535; written to E220 CRYPT_H/CRYPT_L; never returned by `GET` |

Address notes:
- LOCAL address is fixed at 0x01 - not configurable, not shown in portal.
- 0x00 is invalid and 0xFF is the broadcast address - both reserved, not selectable (E220 manual §5.3/5.4: 0x0000 and 0xFFFF disable address filtering).
- The address selector is shown in the portal only when tier == REMOTE.
- Address is configurable post-provisioning via `PUT /api/config {"loraAddr":3}`.

**Expected Outcomes:**
- WiFiManager portal shows: Tier, Log Level, LoRa Channel (dropdown 0-31 labelled "Ch N - 433.NNN MHz"), LoRa key, and for REMOTE only: LoRa Address (numeric input 2-254)
- All settings load from NVS on boot (before `setupServer()`) and survive reboots
- `GET /reset` clears the entire `loraiot_prefs` namespace before calling `wm.resetSettings()`
- `GET /api/config` returns the current values as JSON: `{"tier":"REMOTE","logLevel":"INFO","loraChannel":23,"loraAddr":2}` (the key is never returned)
- `PUT /api/config` accepts any subset (e.g. `{"loraChannel":24}`) — persists to NVS; log level takes effect immediately; channel and address take effect on next reboot (E220 re-init)
- `GET /api/lora/nodes` placeholder registered, returns empty array until M6 populates it

**Todo List:**
- [x] Create `src/lora/src/LoRaConfig.h/.cpp` — `LoRaConfigClass` singleton: `begin()` (NVS load), `save()`, getters/setters for channel (`uint8_t`) and addr (`uint8_t`); mirrors `GatewayClass` / `Tier` pattern exactly
- [x] Add `LoRaConfigClass LoRaConfig` global; `begin()` called from `applicationSetup()` before `setupServer()`
- [x] Add LoRa channel `<select>` to portal in `Server.cpp` (options 0-31, each labelled "Ch N - 433.NNN MHz", current value pre-selected)
- [ ] Add LoRa address `<input type='number' min='2' max='254'>` to portal, rendered only when tier == REMOTE (inject conditionally using `Gateway.getTier()`)
- [ ] Add `loraKey` (NVS, portal, `PUT /api/config`) and `LoRaConfig.getKey()` / `setKey()`; use it for CRYPT_H/CRYPT_L in `LoRaTransport::begin()`
- [ ] Change defaults to the new address map: `LORA_DEFAULT_ADDR_LOCAL = 0x01`, `LORA_DEFAULT_ADDR_REMOTE = 0x02`; reject 0x00, 0x01 (REMOTE) and 0xFF in setters and `PUT` handlers
- [x] Portal save callback: persist channel and addr via `LoRaConfig.save()`
- [x] Register `GET /api/lora/channel` / `GET /api/lora/address` in `RestService::registerRoutes()`
- [x] Register `PUT /api/lora/channel` / `PUT /api/lora/address` body handlers: update NVS and return updated JSON
- [ ] Update `handleReset()` in `Server.cpp`: clear `loraiot_prefs` namespace entirely (not just individual keys) before `wm.resetSettings()`
- [ ] Remove compile-time `#ifdef POC_ROLE_LOCAL` / `POC_ROLE_REMOTE` from `Application.cpp`; replace with `Gateway.getTier()` runtime branch
- [ ] Update `LoRaTransport::begin()` (M3) to read channel and addr from `LoRaConfig` instead of hardcoded constants

**Relevant Context:**
- `src/LoRaIoT/src/Tier.h/.cpp` — NVS-persisted singleton pattern to follow exactly
- `src/LoRaIoT/src/Server.cpp` setupServer() — portal parameter injection pattern (tier + log level)

---

## Milestone 3 — LoRa Transport Layer: Full E220 Init + Serial2 Owner Task

**Status:** [ ] pending
**Prerequisite:** Milestone 0 (LoRaTransport skeleton exists; this replaces it with production implementation), Milestone 2 (LoRaConfig available for channel/addr)

**Intent:**
Upgrade the M0 plain-text `LoRaTransport` skeleton to the full production implementation:
E220 fixed-addressing mode, channel and address read from `LoRaConfig`, AUX-correct send
pattern, and a clean raw-bytes API that the frame codec (M4) builds on top of.

**Expected Outcomes:**
- `LoRaTransport` replaces M0 skeleton; external API (`sendRaw` / `setReceiveCallback`) unchanged
- E220 configured with REG0=0xE7, REG1=0x00, REG3=0x40 (fixed transmission), CRYPT_H/L from `LoRaConfig`; channel and ADDL read from `LoRaConfig`
- `sendRaw(uint8_t destAddr, uint8_t* buf, size_t len)` prepends the 3-byte E220 address header (ADDH=0x00, ADDL=destAddr, channel; broadcast: ADDH=ADDL=0xFF), then writes the payload atomically; maximum `len` is 200 - 3 = 197 B at this layer (the frame codec limits payload to 186 B)
- AUX-correct send sequence per Feasibility.md §7.1: poll AUX HIGH → write atomically → wait AUX LOW → wait AUX HIGH → 50 ms guard delay
- Incoming frames: the E220 delivers only the payload (no sender address); the receive callback gets the raw bytes and the codec (M4) reads SRC from the frame header
- E220 config errors (FF FF FF response) retried up to 3 times; `StatusLed::FAULT` on persistent failure
- Mode 3 entry/exit: switch UART to 9600, set M1=1/M0=1, wait AUX rising edge ≥ 2 ms; reverse on exit

**Todo List:**
- [ ] Upgrade `configureE220()`: switch UART to 9600, enter Mode 3, send config command with channel from `LoRaConfig`, REG3=0x40 (fixed transmission) and CRYPT key, verify response (not FF FF FF), exit Mode 3, switch UART back to 115200
- [ ] Upgrade `sendRaw(destAddr, buf, len)`: prepend 3-byte address header before writing to Serial2
- [ ] Implement `setMode(m1, m0)` private method: set GPIO M0/M1, wait for AUX rising edge ≥ 2 ms
- [ ] Implement full AUX-correct `sendLoRaFrame()` per Feasibility.md §7.1
- [ ] `loraTransportTask` loop: dequeue outgoing request → `sendLoRaFrame()` (destination address per request, not derived from the own address); read incoming bytes from Serial2 → invoke receive callback with the raw bytes
- [ ] Define named constants per `doc/LoRaProtocol.md` §7: `LORA_ACK_TIMEOUT_MS=200`, `LORA_REQUEST_TIMEOUT_MS=1000`, `LORA_MAX_RETRIES=3`, `LORA_BACKOFF_MIN_MS=20`, `LORA_BACKOFF_MAX_MS=200`, `LORA_BCAST_REPLY_WINDOW_MS=1000`, `LORA_RF_GUARD_DELAY_MS=50`
- [ ] Use `TASK_CREATE` macro; priority 2; 4096 stack
- [ ] Remove the M0 peer-address assumption in `sendRawInternal()` (`targetAddl` toggling LOCAL/REMOTE)
- [ ] In `Application.cpp`: call `loraTransport.begin()` after `LoRaConfig.begin()`

**Relevant Context:**
- `doc/Feasibility.md` §3.1 (pin mapping), §3.2 (AUX semantics), §3.3 (config command), §7.1 (send pattern)
- `doc/LoRaProtocol.md` (M1) §2 (registers), §3 (addresses), §7 (timeouts, retries)

---

## Milestone 4 — Frame Codec: Framing, CRC, and LoRaState Shared State

**Status:** [ ] pending. Note: `loraiot.proto`, `loraiot.options`, the generated `loraiot.pb.h/.c` and the nanopb 0.4.9.1 runtime already exist in `src/lora/` (see `doc/NanopbGeneration.md`). Only the startup self-test in `Diagnostics.cpp` (PING encode/decode) uses them so far; it passes on the ESP32 when built with the Arduino IDE. A PlatformIO build has not been run.
**Prerequisite:** Milestone 1 (CRC variant and frame type table decided), Milestone 3 (transport layer complete)

**Intent:**
Implement the frame encode/decode layer (11-byte header/trailer + payload per
`doc/LoRaProtocol.md` §4), the protobuf payload layer (nanopb) and the mutex-protected
`LoRaState` shared state struct that holds received frame data, link health counters,
the LRU REMOTE registry and the per-SRC sequence/boot_id state.

**Expected Outcomes:**
- `src/lora/src/LoRaFrame.h/.cpp` — `encodeFrame()` and `decodeFrame()`
- Frame: `AA 55 | VER | TYPE | SRC | DST | SEQ(2) | LEN | PAYLOAD | CRC16`; CRC over VER..PAYLOAD (not SYNC)
- CRC unit test: `"123456789"` gives `0x29B1`; the worked frame of `doc/LoRaProtocol.md` §4.2 encodes to exactly the documented bytes and decodes back
- `decodeFrame()` returns false on SYNC/CRC/VER failure, unknown TYPE or foreign DST; all failures logged at DEBUG and counted
- `src/lora/proto/loraiot.proto` (+ nanopb `.options`) created from `doc/LoRaProtocol.md` §10; `nanopb/Nanopb` added to `lib_deps` (verify current release and maintenance status when adding); generated C structs use no heap
- Per-REMOTE definition cache (`Device` / `Metric` from `Describe`) in one place, used to name and scale incoming `Measurement`s and to convert REST JSON to `Data`
- `src/lora/src/LoRaState.h/.cpp` — mutex-protected: per-SRC last SEQ, last boot_id, cached RESPONSE, TX/RX/CRC-error counts, link status, LRU REMOTE registry
- LRU registry: `RemoteEntry { uint8_t addr; uint32_t lastSeenMs; uint32_t bootId; bool online; }` — `updateRemote(addr)` stamps timestamp; `purgeStale()` removes entries unseen for 180 s
- Frame type enum: `PING=0x01`, `BEACON=0x02`, `ACK=0x03`, `DATA=0x04`, `DATA_ACK=0x05`, `REQUEST=0x06`, `RESPONSE=0x07`

**Todo List:**
- [ ] Implement `computeCRC16(const uint8_t* data, size_t len)` — CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no input/output reflection); add the `0x29B1` check
- [ ] Create `src/lora/src/LoRaFrame.h` — frame constants, `LoRaFrameType` enum, `LoRaFrame` struct (ver, type, src, dst, seq, len, payload[186]), function signatures
- [ ] Implement `encodeFrame(const LoRaFrame&, uint8_t* outBuf, size_t& outLen)` per §4
- [ ] Implement `decodeFrame(const uint8_t* inBuf, size_t len, LoRaFrame& out, uint8_t ownAddr)`: resync on SYNC, verify CRC, VER, TYPE, DST; return false on any error
- [ ] Create `src/lora/proto/loraiot.proto` and `.options`; add nanopb to `platformio.ini` `lib_deps` and generate sources
- [ ] Create `src/lora/src/LoRaMetrics.h/.cpp` — definition cache (per REMOTE and device) and JSON <-> `Data` conversion (`value / scale`, key from `Metric.name`)
- [ ] Create `src/lora/src/LoRaState.h/.cpp` — mirrors `ConverterState` pattern; LRU registry with `updateRemote()`, `purgeStale()`; per-SRC duplicate detection and `boot_id` reset (§6.1)
- [ ] Add `LoRaState loraState` global (analogous to `converterState` in SerialController)
- [ ] Update `LoRaTransport` receive callback: call `decodeFrame()`, update `loraState`, call `purgeStale()`

**Relevant Context:**
- `doc/LoRaProtocol.md` (M1) - §4 frame, §5 types, §6 sequence numbers, §10 payloads, §11 naming, §12 definitions
- `ESP/SerialController/src/service/src/ConverterState.h/.cpp` — mutex-protected state pattern
- `ESP/SerialController/src/SerialController/src/ConverterStateGlobal.h` — global declaration pattern

---

## Milestone 5 — Echo Test: End-to-End Validation with Two ESPs

**Status:** [ ] pending
**Prerequisite:** Milestones 1, 2, 3, 4

**Intent:**
With both ESPs connected to a PC via USB, validate the full framed protocol end-to-end.
LOCAL sends a framed `REQUEST(echo)`; REMOTE receives it, reverses the text, and
replies with a `RESPONSE(echo)` whose `request_seq` equals the request SEQ. This validates:
- Fixed addressing (frames reach the correct node) and SRC/DST handling
- Frame encoding, protobuf payloads, CRC, and SeqNo handling end-to-end
- AUX timing and send/receive sequencing under the production transport
- Timeout, retry and backoff logic from `doc/LoRaProtocol.md` §7
- The open hardware questions of `doc/LoRaProtocol.md` §18 (payload limit, broadcast PING)

**Expected Outcomes:**
- `POST /api/lora/echo {"message":"Hello LoRa!"}` on LOCAL sends a framed `REQUEST(echo)` to REMOTE address 0x02
- REMOTE reverses the text and replies with `RESPONSE(echo)`; no REST call needed on REMOTE
- `GET /api/lora/echo/result` returns `{"sent":"Hello LoRa!","received":"!aro L olleH","seqNo":1,"roundTripMs":…,"success":true}`
- `GET /api/lora/nodes/2/ping` returns the PING round trip; a broadcast PING is tried and the result recorded in `doc/LoRaProtocol.md` §18
- Payload size test: find the largest payload accepted unfragmented and update `doc/LoRaProtocol.md` §4 if above 186 B
- Repeated calls increment SeqNo; no dropped frames visible in `GET /api/log`
- Both ESPs' `GET /api/lora/status` show TX count, RX count, error count
- Stress test: 100 sequential REQUEST(echo) frames, zero CRC errors in log, TX count == RX count on both sides

**Todo List:**
- [ ] REMOTE receive callback: PING -> ACK (with boot_id handling); REQUEST(echo) -> reverse text, RESPONSE with `request_seq`, `loraTransport.sendRaw(srcAddr, ...)`
- [ ] LOCAL echo TX in `RestService`: encode `REQUEST(echo)` addressed to REMOTE 0x02, send, block on response queue with `LORA_REQUEST_TIMEOUT_MS`; retry once with random backoff (§7); store result in `loraState`
- [ ] Register `GET /api/lora/nodes/{addr}/ping` (PING/ACK round trip)
- [ ] Register `POST /api/lora/echo` and `GET /api/lora/echo/result` in `RestService::registerRoutes()`
- [ ] Register `GET /api/lora/status` returning TX/RX/error counts and last SeqNo from `loraState`
- [ ] Add echo test results and link health to WebSocket 1-second broadcast JSON
- [ ] Manual validation: both serial monitors show framed exchange; `GET /api/log` shows CRC pass on both sides
- [ ] Stress test: 100 sequential frames; verify zero errors

**Relevant Context:**
- `doc/LoRaProtocol.md` (M1) - §5 types, §6 sequence numbers, §7 timeouts and retries, §18 items to verify

---

## Milestone 6 — Dual-Role Application Logic: Beacon, Requests, and Sensor Data

**Status:** [ ] pending
**Prerequisite:** Milestone 5 (framed LoRa link confirmed working end-to-end)

**Intent:**
Implement the full role-specific behavior of `doc/LoRaProtocol.md`: REMOTE beaconing, unsolicited
DATA/DATA_ACK alerts and answering REQUESTs with sensor data; LOCAL maintaining the REMOTE
registry, optional cyclic reads, live (near real time) reads for REST clients and the cached
values. A power IoT sensor scenario (voltage + current) is the concrete payload used for validation.
Both data sources must work: code on the REMOTE itself (device 0) and external devices that
post through the REMOTE SoftAP (device 1..N).

**Medium access model:**
No scheduler (`doc/LoRaProtocol.md` §9). Either side may transmit at any time; collisions are
resolved by timeout + random backoff + retry. LOCAL may read cyclically (configurable interval,
default 5 s per REMOTE).

**REMOTE beacon state machine (`doc/LoRaProtocol.md` §8):**
```
UNDISCOVERED → beacon every 5 s (+ 0-1 s jitter)
    first frame from LOCAL addressed to this REMOTE
DISCOVERED   → beacon every 60 s
    no frame from LOCAL for 2 min
UNDISCOVERED → beacon every 5 s  (self-healing)
```

**LOCAL LRU registry:**
- `updateRemote(addr)` on every received frame from a REMOTE
- On a BEACON from an unknown REMOTE or with a changed `boot_id`: reply with PING
- Keep-alive PING to every REMOTE idle for 60 s
- `purgeStale()` removes entries with `now - lastSeen > 180 s`; purged REMOTEs re-enter via fast beacon

**Expected Outcomes:**
- REMOTE `beaconTask`: beacons at 5 s rate until the first frame from LOCAL; then 60 s; reverts to 5 s after 2 min of silence
- LOCAL `GET /api/lora/nodes` returns the live registry: `[{"addr":2,"lastSeen":1234567,"online":true,"bootId":…}]`
- Live read: `GET /api/lora/nodes/{addr}/devices/{device}/metrics?live=true&timeout=ms` sends `REQUEST(Read)` and returns 200, 404 (unknown/purged node) or 504 (LoRa timeout); the cached variant (without `live`) returns the last values with their age
- LOCAL fills its definition cache with the `Describe` crawl (device 0, 1, 2, ... until `STATUS_BAD_REQUEST`) when a BEACON shows a new `boot_id` or `registry_rev`
- REMOTE WiFi SoftAP (`LoRaIoT-Remote`, 192.168.4.1) starts after provisioning; IoT devices POST sensor data to `POST /api/sensor` on REMOTE, which keeps the latest values per device name (device and metric tables, `Beacon.registry_rev`); a registration that would make a `Device` exceed one frame is rejected with HTTP 400
- REMOTE may send an unsolicited `DATA_ACK` alert when a configured threshold is exceeded; LOCAL ACKs it and updates its cache
- StatusLed READY on LOCAL once ≥ 1 REMOTE is online; READY on REMOTE once it has received its first frame from LOCAL
- WebSocket broadcast on LOCAL includes: tier, node registry, last sensor values per REMOTE, link stats (TX/RX/error counts)
- `openapi_json.h` and `doc/RestApi.md` describe every endpoint added in this milestone

**Todo List:**
- [ ] Implement REMOTE `beaconTask`: sends BEACON frame (all `Beacon` fields: `boot_id`, `fw_version`, `uptime_s`, `registry_rev`) at current rate; tracks UNDISCOVERED/DISCOVERED state; uses `TASK_CREATE`
- [ ] REMOTE: handle PING -> ACK; REQUEST(Read) -> RESPONSE with `Data` (device 0 sampled on demand, AP devices from the last posted values with `age_ms`); REQUEST(Describe) -> RESPONSE(Device) or `STATUS_BAD_REQUEST` for an unknown device
- [ ] REMOTE: register `POST /api/sensor` in `RestService::registerRoutes()` — IoT device posts `{"device":"Sensor A","data":{"Voltage":3.35,"Current":0.12}}`; unknown device names and keys get the next free device index / metric id (`registry_rev` increments); values are scaled to `sint32` and stored per device
- [ ] LOCAL: implement the `Describe` crawl and the definition cache; drop the cache when `boot_id` changes
- [ ] REMOTE: optional alert threshold -> unsolicited `DATA_ACK` with `alert=true` (retry with random backoff)
- [ ] LOCAL: handle BEACON (registry, PING reply), DATA, DATA_ACK (ACK reply, cache update)
- [ ] LOCAL: implement optional cyclic read task (`readTask`): iterate registry, send `REQUEST(Read)`, update `loraState`; uses `TASK_CREATE`
- [ ] LOCAL: implement the request/response queue towards `loraTask` (REST handlers post and block with timeout; never touch Serial2)
- [ ] LOCAL: register `GET /api/lora/nodes`, `.../nodes/{addr}/devices`, `.../devices/{device}/metrics` (cached and `live=true`), `POST .../nodes/{addr}/data`
- [ ] Update `openapi_json.h` and `doc/RestApi.md` for every endpoint above (same change)
- [ ] In `applicationSetup()`: branch on `Gateway.getTier()` — LOCAL: WiFi STA only; REMOTE: call `WiFi.softAP("LoRaIoT-Remote")` after WiFiManager provisioning completes; disconnect STA after provisioning (AP mode only, frees WiFi resources)
- [ ] Update WebSocket broadcast JSON: tier, node registry, last sensor values, link stats
- [ ] Update `index_html.h` placeholder with a real UI: tier badge, node registry table, per-REMOTE sensor values, link stats panel; adapt SerialController HTML pattern (single self-contained HTML with inline JS WebSocket client)

**Relevant Context:**
- `doc/LoRaProtocol.md` (M1) - §8 beacon state machine, §5 frame types, §10 to §13 payloads, definitions and mapping, §17 REST mapping
- `doc/Feasibility.md` §5.1 architecture diagram, §5.2 component roles
- `ESP/SerialController/src/SerialController/src/index_html.h` — PROGMEM self-contained HTML pattern
- `ESP/SerialController/src/service/src/WebSocketService.cpp` — broadcast pattern

---

## Milestone 7 — Performance Tuning and Multi-REMOTE

**Status:** [ ] pending
**Prerequisite:** Milestone 6 (full application logic working with 1 REMOTE)

**Intent:**
Extend to multiple REMOTE nodes, measure and optimize
throughput and latency, and tune `LORA_RF_GUARD_DELAY_MS` based on measured AUX timing.

**Open questions to answer during this milestone:**
1. Is 62.5 kbps optimal for the target deployment range, or should air rate be configurable at runtime without reboot?
2. Can `LORA_RF_GUARD_DELAY_MS` (currently 50 ms) be reduced based on measured AUX timing?
3. Is the 400-byte E220 FIFO always sufficient, or does concurrent WiFi activity on the ESP32 cause UART timing glitches under load?
4. With N REMOTEs each read every 5 s, what is the practical maximum N before channel saturation? (Each request/response round-trip ≈ 100 ms at 62.5 kbps → theoretical max ~50 per second, practical limit much lower; unsolicited alerts add collisions.)

**Expected Outcomes:**
- No over-the-air channel change (`doc/LoRaProtocol.md` §14); the channel is configured on each node
- `PUT /api/lora/config {"airRate":7,"txPower":0}` — temporary E220 write (0xC2) for runtime tuning without NVS change; reverts on reboot (values are the raw REG0 bits 2-0 / REG1 bits 1-0 per the manual: air rate 7 = 62.5 kbps, power 0 = 22 dBm)
- `POST /api/lora/benchmark {"count":100}` runs N `REQUEST(echo)`/`RESPONSE(echo)` pairs and returns: min/avg/max RTT ms, error count, throughput bytes/s
- `LORA_RF_GUARD_DELAY_MS` tuned from measurement; updated constant documented in `doc/LoRaProtocol.md`
- Multi-REMOTE validated: 2 REMOTEs beaconing and responding to polls simultaneously; verify no frame loss
- Results documented in `doc/Feasibility.md` §8 matrix (measured vs. theoretical values)

**Todo List:**
- [ ] Implement `POST /api/lora/benchmark` endpoint (N configurable, default 100)
- [ ] Implement `PUT /api/lora/config` for airRate/txPower using 0xC2 temporary write command
- [ ] Measure actual AUX timing on hardware; tune `LORA_RF_GUARD_DELAY_MS`; document in `doc/LoRaProtocol.md`
- [ ] Test with 2 REMOTE nodes simultaneously; verify zero frame loss under sustained polling
- [ ] Update `doc/Feasibility.md` §8 with measured vs. theoretical values

**Relevant Context:**
- `doc/Feasibility.md` §3.3 (REG0/REG1 bit fields, 0xC2 temporary write command)
- `doc/Feasibility.md` §4.1 (throughput table — theoretical values to compare against)
- `doc/LoRaProtocol.md` (M1)

---

## Appendix A — Single-Core ESP Support (ESP32-C3, C6, S2, H2)

**Status:** [x] complete

### Background

The ESP32-C6 (and C3, S2, H2) are single-core chips. FreeRTOS is built with
`CONFIG_FREERTOS_UNICORE=1`, which means core 1 does not exist. Calling
`xTaskCreatePinnedToCore(..., 1)` on these chips causes an immediate kernel panic
at boot. There is **no fundamental architectural restriction** that requires two cores —
the entire design (FreeRTOS task isolation, Serial2 single-owner queue, mutex-protected
shared state, ESPAsyncWebServer) works identically on a single-core target. Tasks that
would have run in true parallel on dual-core hardware are instead time-sliced by the
FreeRTOS scheduler on the one core.

### Required Code Changes

#### 1. Replace `xTaskCreatePinnedToCore` with `TASK_CREATE` everywhere - DONE

**Implemented:** `src/util/src/TaskUtils.h` defines `TASK_CREATE(func, name, stack, param, prio, handle)`.

- When `LORAIOT_DUAL_CORE` is defined **and** `CONFIG_FREERTOS_UNICORE` is not defined: expands to `xTaskCreatePinnedToCore(..., 1)`.
- Otherwise (all single-core targets, or dual-core without the flag): expands to `xTaskCreate`.
- `CONFIG_FREERTOS_UNICORE` is set automatically by ESP-IDF for all single-core targets (C3, C6, S2, H2) - no per-board detection needed.

`LORAIOT_DUAL_CORE` is the opt-in toggle for dual-core pin affinity:
- Arduino IDE: `#define LORAIOT_DUAL_CORE` in `LoRaIoT.ino` / `LoRaIoT.cpp` (commented out by default).
- PlatformIO: set via `build_flags = -D LORAIOT_DUAL_CORE` on dual-core envs (`esp32dev`, `esp32s3`). Single-core envs omit it entirely.

Fixed call sites:
- `WebSocketService::broadcastTask` - `src/service/src/WebSocketService.cpp` (was `xTaskCreatePinnedToCore(..., 1)`)
- `StatusLed::ledTask` - already used `xTaskCreate` - no change needed
- `loraTransportTask`, `beaconTask`, `pollTask` (M0/M3/M6) - must use `TASK_CREATE`

#### 2. Conditional compilation - DONE

The `TASK_CREATE` macro in `src/util/src/TaskUtils.h` is the sole mechanism. All new task
creation must use it. Direct calls to `xTaskCreatePinnedToCore` are prohibited.

#### 3. Shared state: `volatile` vs. mutex

On a dual-core chip, a mutex (or `std::atomic`) is required for single-word shared
variables because each core has its own store buffer. On a single-core chip, context
switches only occur at FreeRTOS tick boundaries or explicit yields, so `volatile` alone
is technically sufficient for single-word reads/writes.

**Decision:** keep the mutex-based `LoRaState` pattern from Milestone 4 regardless of
target. It is correct on both architectures and avoids bugs if the firmware is later
ported back to a dual-core board. Uncontested mutex acquisition overhead is negligible.

### Considerations (No Code Change Required)

| Concern | Dual-core | Single-core | Impact |
|---|---|---|---|
| WiFi stack + LoRa task true parallelism | Yes | No - time-sliced | Possible increase in LoRa TX latency under heavy HTTP load; only measurable at high REST request rates |
| `delay()` inside tasks | Yields to other tasks on same core | Same behaviour | No change |
| Serial2 UART timing | Runs in gaps between WiFi bursts on Core 0 | Shares the single core with WiFi and HTTP | AUX guard delay of 50 ms remains sufficient; WiFi preemption cannot corrupt a completed `Serial.write()` |
| ESPAsyncWebServer | Callbacks on Core 0 | Callbacks on Core 0 (the only core) | No change |
| FreeRTOS queues and semaphores | Fully supported | Fully supported | No change |

### Board Support Matrix

| Board | Chip | Cores | PlatformIO env | Status |
|---|---|---|---|---|
| ESP32 DevKit | ESP32 (Xtensa LX6) | 2 | `esp32dev` | Supported - `LORAIOT_DUAL_CORE` set via build_flags |
| ESP32-S3 | ESP32-S3 (Xtensa LX7) | 2 | `esp32s3` | Supported - `LORAIOT_DUAL_CORE` set via build_flags |
| ESP32-S2 | ESP32-S2 (Xtensa LX7) | 1 | `esp32s2` | Supported - no flag needed, `TASK_CREATE` uses `xTaskCreate` |
| ESP32-C3 | ESP32-C3 (RISC-V) | 1 | `esp32c3` | Supported - no flag needed, `TASK_CREATE` uses `xTaskCreate` |
| ESP32-C6 | ESP32-C6 (RISC-V) | 1 | `esp32c6` | Supported - no flag needed, `TASK_CREATE` uses `xTaskCreate` |
| ESP32-H2 | ESP32-H2 (RISC-V) | 1 | `esp32h2` | Supported - no flag needed, `TASK_CREATE` uses `xTaskCreate` |

### Todo List

- [x] Audit all `xTaskCreatePinnedToCore` calls across the codebase and replace with `TASK_CREATE` macro
- [x] Create `src/util/src/TaskUtils.h` with `TASK_CREATE` macro to prevent future regressions
- [x] Add single-core PlatformIO envs (`esp32c3`, `esp32c6`, `esp32s2`, `esp32h2`) to `platformio.ini`
- [x] Add note to `AGENTS.md` that `xTaskCreatePinnedToCore` is prohibited; use `TASK_CREATE` for all new task creation
- [ ] Test boot sequence on a single-core board: confirm no kernel panic, confirm WiFiManager captive portal appears, confirm `/status` endpoint responds

---

## Appendix B — Power-Saving Feasibility & Clock Optimization Analysis

**Status:** [x] complete

### 1. Architectural Energy Constraints & Viability

A core objective of the LoRaIoT P2P bridge is to maximize energy efficiency on battery-powered or solar-powered deployments. However, the viability of deep sleep states differs fundamentally between the two nodes due to physical and network-layer constraints.

#### A. The LOCAL (Master) Node
- **Constraint:** Must maintain a continuous TCP/IP socket with the local home network (Station mode) and listen continuously for asynchronous LoRa frames from the `REMOTE` client.
- **Deep/Light Sleep Viability:** **No.** Shutting down the CPU or the WiFi radio terminates the STA connection. Re-connecting to the router and completing a DHCP handshake on wake-up takes 2-5 seconds and consumes up to $250\text{ mA}$, yielding a net negative energy return. Furthermore, sleeping would cause the master to miss asynchronous LoRa transmissions.
- **Power Optimization:** Use automatic **WiFi Modem-Sleep** (`WIFI_PS_MIN_MODEM`). The ESP32 duty-cycles its receiver between router DTIM beacons (sleeping for ~100-300 ms, waking for ~5 ms), dropping active current from 110 mA to **~15-20 mA** with zero packet loss or latency impact.

#### B. The REMOTE (Client) Node
- **Constraint:** Must act as an always-on WiFi Access Point (SoftAP) for third-party WiFi-enabled sensors to report data on demand.
- **Deep/Light Sleep Viability:** **No.** If the ESP32 shuts down its SoftAP, sensors waking up will fail to find the SSID, causing immediate sensor-side packet loss and severe sensor battery drain.
- **Power Optimization:** Because the SoftAP transmitter must remain fully active to broadcast beacon frames every 100 ms, the ESP32 CPU cannot enter deep sleep. Instead, we must target **"limited" power-saving techniques** (such as clock-frequency scaling).

---

### 2. Clock Optimization & Dynamic Frequency Scaling (DFS)

Because both the `LOCAL` and `REMOTE` nodes are completely I/O-bound (polling registers, copying UART buffers, and routing small data frames), running the ESP32 CPU at its maximum clock speed of **240 MHz** is highly inefficient.

#### A. The 80 MHz Threshold
The ESP32's hardware WiFi/Bluetooth radio PLL (Phase-Locked Loop) is derived from the main CPU clock. The **absolute minimum clock frequency required to keep the WiFi hardware active is 80 MHz**. Dropping the clock below 80 MHz (e.g. to 40, 20, or 10 MHz) immediately disables the WiFi radio.

#### B. Clock Frequency Power Metrics
By scaling the CPU frequency down, we dramatically reduce the core dynamic switching current:

| CPU Clock (MHz) | WiFi State | Average Current (mA) | Power Reduction vs. 240 MHz |
|:---:|:---:|:---:|:---:|
| **240 MHz** | SoftAP / STA Active | ~110 - 130 mA | Baseline |
| **160 MHz** | SoftAP / STA Active | ~90 - 105 mA | ~20% |
| **80 MHz**  | SoftAP / STA Active | ~70 - 85 mA  | ~35% |

By running the `REMOTE` node at **80 MHz** permanently, we immediately shave off **~40 mA of continuous current**, extending battery/solar bank life by 35% without impacting SoftAP beacon timing, FreeRTOS task scheduling, or LoRa throughput.

---

### 3. Integrated Control REST API

To allow users and automation systems to dynamically manage and monitor the energy footprint of each node, we implemented a dedicated system clock API:

- **`GET /api/system/cpu`**: Returns the current clock speed.
- **`PUT /api/system/cpu`**: Validates and dynamically sets the clock speed to `80`, `160`, or `240` MHz, writing the choice persistently to NVS (`sc_prefs` / `cpuFreq`) so the optimized frequency is automatically applied at boot time.
