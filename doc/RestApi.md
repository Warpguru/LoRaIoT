# LoRaIoT REST API

Reference for every HTTP and WebSocket endpoint currently registered in the firmware.
Base URL: `http://<device-ip>/` (port 80).

> **Source of truth:** `src/LoRaIoT/src/openapi_json.h` is now the authoritative API spec,
> served at `GET /openapi` and rendered interactively at `GET /openapi/ui` (Swagger UI).
> This file is kept as a human-readable companion. When they diverge, the OpenAPI spec wins.
> **Rule:** every new endpoint must be added to both files in the same change that registers it.

## Endpoint summary

| Method | Path | Registered in | Node | Purpose |
|---|---|---|---|---|
| GET | `/` | `Server.cpp` | Both | Browser UI (HTML from PROGMEM) |
| GET | `/openapi` | `Server.cpp` | Both | OpenAPI 3.0 specification (JSON, from PROGMEM) |
| GET | `/openapi/ui` | `Server.cpp` | Both | Swagger UI explorer (fetches JS/CSS from unpkg CDN) |
| GET | `/status` | `Server.cpp` | Both | ESP32 hardware diagnostics |
| GET | `/reset` | `Server.cpp` | Both | Clear WiFi and all persisted NVS settings, then reboot |
| GET | `/api/log` | `Server.cpp` | Both | Read (and optionally clear) the log ring buffer |
| PUT | `/api/log/level` | `Server.cpp` | Both | Change the active log level |
| GET | `/api/tier` | `RestService.cpp` | Both | Return the active gateway tier (LOCAL / REMOTE) |
| GET | `/api/diag/e220` | `RestService.cpp` | Both | Request an E220 wiring and register check |
| GET | `/api/lora/channel` | `RestService.cpp` | Both | Get active LoRa channel |
| PUT | `/api/lora/channel` | `RestService.cpp` | Both | Set active LoRa channel |
| GET | `/api/lora/address` | `RestService.cpp` | Both | Get active LoRa address |
| PUT | `/api/lora/address` | `RestService.cpp` | REMOTE | Set active LoRa address |
| GET | `/api/system/cpu` | `RestService.cpp` | Both | Get active CPU frequency |
| PUT | `/api/system/cpu` | `RestService.cpp` | Both | Set active CPU frequency (80, 160, 240 MHz) |
| GET | `/api/lora/nodes` | `RestService.cpp` | LOCAL | List all known remote nodes |
| GET | `/api/lora/nodes/devices` | `RestService.cpp` | LOCAL | List/lookup devices on a remote node (`?addr=`, `?device=`, `?refresh=true`) |
| GET | `/api/lora/nodes/metrics` | `RestService.cpp` | LOCAL | Cached or live metric readings (`?addr=`, `?device=`, `?live=true`, `?timeout=ms`) |
| GET | `/api/lora/stats` | `RestService.cpp` | LOCAL | LoRa transport TX/RX/CRC/ACK counters |
| POST | `/api/lora/nodes/echo` | `RestService.cpp` | LOCAL | Echo test to a remote node (`?addr=`) |
| GET,POST | `/api/lora/nodes/ping` | `RestService.cpp` | LOCAL | Unicast ping to a remote node (`?addr=`) |
| GET | `/api/lora/ping` | `RestService.cpp` | LOCAL | Broadcast ping |
| POST | `/api/lora/beacon` | `RestService.cpp` | LOCAL | Trigger an immediate manual BEACON |
| POST | `/api/lora/nodes/data` | `RestService.cpp` | LOCAL | Send a Data frame (`?addr=`, `?ack=true\|false`) |
| POST | `/api/sensor/data` | `RestService.cpp` | REMOTE | Ingest sensor data from a SoftAP IoT device |
| WS | `/ws/data` | `WebSocketService.cpp` | Both | Live state push (1 s) |

---

## GET /status

Returns ESP32 hardware diagnostics as JSON (fields come from `fillESPInfo()` in `ESPInfo.h`).

**Response:** `200 application/json`

---

## GET /reset

Clears the stored WiFi credentials and every application setting persisted in NVS, then reboots into the
WiFiManager captive portal. The erased settings are the gateway tier, the log level, the LoRa channel,
the LoRa address and the CPU frequency (namespaces `TIER_NVS_NAMESPACE`, `LOG_NVS_NAMESPACE` and
`LORA_NVS_NAMESPACE`, all currently `sc_prefs`). After the reboot the node uses the built-in defaults until
it is provisioned again.

**Response:** `200 text/plain` - `WiFi and application settings cleared. ESP32 rebooting to Configuration Mode...`

The device restarts about 200 ms after the response is sent.

---

## GET /api/log

Returns the in-RAM log ring buffer (last 128 lines).

| Query parameter | Type | Description |
|---|---|---|
| `clear` | `1` | Optional. Clears the buffer before returning it. |

**Response:** `200 application/json`

```json
{"log":["[INFO ][Application.cpp:30] Starting LoRaIoT ..."],"level":"INFO"}
```

---

## PUT /api/log/level

Sets the active log level. The level is persisted in NVS and survives reboots.

**Request body:** `application/json`

```json
{"level":"DEBUG"}
```

Allowed values: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`.

| Status | Meaning |
|---|---|
| `200` | Level changed. Body: `{"level":"DEBUG"}` |
| `400` | Body is not valid JSON, `level` is missing, or the level name is unknown |

---

## GET /api/tier

Returns the currently active gateway tier.

**Response:** `200 application/json`

```json
{"tier":"LOCAL","configured":true}
```

| Field | Type | Description |
|---|---|---|
| `tier` | string | `"LOCAL"`, `"REMOTE"`, or `"UNSET"` if no tier has been selected in the portal yet |
| `configured` | bool | `false` when tier is `"UNSET"` (portal has not been completed) |

**Tier meanings:**
- `LOCAL` - connected to the local LAN / Home Assistant; receives data from the REMOTE instance.
- `REMOTE` - hosts the WiFi SoftAP for IoT sensors; forwards data over LoRa to the LOCAL instance.

The tier is selected in the WiFiManager captive portal and persisted in NVS
(namespace `sc_prefs`, key `tier`). It survives reboots. Clear it with `GET /reset`.

---

## GET /api/diag/e220

Requests a run of the E220 wiring and register check (`src/diagnostics/src/Diagnostics.cpp`).

The handler only sets a flag. The check itself runs on the next pass of `applicationLoop()`,
because HTTP callbacks must not do Serial2 I/O or block. The call returns immediately and
does not contain the result.

**Request body:** none

**Response:** `202 application/json`

```json
{"status":"requested","result":"see GET /api/log"}
```

**Reading the result:** call `GET /api/log` a few seconds later and look for the lines starting
with `E220 diag:`. The sequence ends with either
`RESULT: PASS. UART, M0, M1 and AUX wiring verified` or `RESULT: FAIL`, and each `FAIL` line
names the likely wiring cause.

After the E220 check, the same run also performs a software-only protobuf check: it encodes a
PING payload (`Beacon{boot_id = 0x12345678}`) with nanopb, compares it with the documented bytes
`08 F8 AC D1 91 01` (`doc/LoRaProtocol.md` section 4.2) and decodes it again. Its lines start with
`Protobuf diag:` and end with `RESULT: PASS. nanopb encode and decode verified` or `RESULT: FAIL`.
It needs no radio, so it runs even when the E220 check fails, and it does not change the status LED.

**Side effects:**
- Uses only the temporary write command (`0xC2`), so nothing is stored in the module's flash.
- If the check fails, the status LED switches to `NO_DEVICE`.
- Takes UART2 for the duration of the check and releases it afterwards. Remove this endpoint
  when Milestone 3 (`LoRaTransport`) becomes the sole owner of UART2.

---

## GET /api/lora/channel

Returns the currently configured LoRa channel. This endpoint is available on both LOCAL and REMOTE instances.

**Response:** `200 application/json`

```json
{"channel":23}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/lora/channel

Sets the active LoRa channel. The new channel is persisted in NVS and survives reboots. This endpoint is available on both LOCAL and REMOTE instances.

**Request body:** `application/json`

```json
{"channel":24}
```

Allowed values: integer between 0 and 31 (inclusive).

| Status | Meaning |
|---|---|
| `200` | Channel changed. Body: `{"channel":24}` |
| `400` | Body is not valid JSON, `channel` is missing, or out of range. |

---

## GET /api/lora/address

Returns the currently configured LoRa node address. This endpoint is available on all nodes: `1` for the LOCAL node and `2-254` for a REMOTE node. `0` is invalid and `255` is the broadcast address.

**Response:** `200 application/json`

```json
{"address":1}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/lora/address

Sets the active LoRa address. The new address is persisted in NVS and survives reboots. The E220 module is programmed at boot, so the new address takes effect after the next reboot. This endpoint is only available on REMOTE instances. Accessing it on a LOCAL instance returns a bad request.

**Request body:** `application/json`

```json
{"address":2}
```

Allowed values: integer between 2 and 254 (inclusive). `1` is reserved for the LOCAL node.

| Status | Meaning |
|---|---|
| `200` | Address changed. Body: `{"address":2}` |
| `400` | Body is not valid JSON, `address` is missing, out of range, or this is a LOCAL instance. |

---

## GET /api/system/cpu

Returns the currently active CPU frequency of the ESP32 in MHz.

**Response:** `200 application/json`

```json
{"frequency":240}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/system/cpu

Dynamically scales the ESP32's CPU clock frequency to optimize power consumption. The chosen frequency is persisted in NVS and automatically applied on reboots.

**Request body:** `application/json`

```json
{"frequency":80}
```

Supported frequencies (with active WiFi radio):
- `80` - 80 MHz (Minimum required clock speed for the ESP32 WiFi radio PLL)
- `160` - 160 MHz
- `240` - 240 MHz (Default high-performance clock speed)

**Response:** `200 application/json`

```json
{"frequency":80}
```

| Status | Meaning |
|---|---|
| `200` | CPU frequency updated. |
| `400` | Body is not valid JSON, `frequency` is missing, or is not a supported value (80, 160, or 240). |

---

## WebSocket /ws/data

Pushes a JSON state snapshot to all connected clients once per second. The payload is not
defined yet (placeholder in `WebSocketService::broadcastTask`).

---

## GET /api/lora/nodes

Returns all remote node addresses that LOCAL has observed via BEACON. LOCAL only.

`online` is `false` when a keep-alive check failed (3 unanswered PINGs after 90 s of silence).
Offline nodes stay in the list with their cached definitions; the next frame from the node
sets `online` back to `true`, and no new Describe crawl happens if `boot_id` and
`registry_rev` are unchanged. A slot is only reused (oldest offline node first) when all
slots are taken and a new node appears.

**Response:** `200 application/json`

```json
{"nodes":[{"addr":2,"boot_id":305419896,"registry_rev":3,"last_seen_ms":12450,"online":true,"devices":2,"crawl_pending":false}]}
```

---

## GET /api/lora/nodes/devices

Returns cached device definitions for a remote node. LOCAL only.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Remote node LoRa address (2-254) |
| `device` | No | Filter to a single device by index (integer) or name |
| `refresh` | No | `true` to schedule a fresh Describe crawl before responding |

| Status | Meaning |
|---|---|
| `200` | OK. Body: `DeviceListResponse` |
| `400` | `addr` missing or out of range |
| `404` | Node not found in registry |

```json
{"addr":2,"registry_rev":3,"crawl_pending":false,"devices":[{"device":0,"name":"RemoteNode","metrics":[{"id":1,"name":"Uptime","unit":"s","scale":1}]},{"device":1,"name":"SensorA","metrics":[{"id":1,"name":"Voltage","unit":"V","scale":100}]}]}
```

---

## GET /api/lora/nodes/metrics

Returns the most recent measurement values for a device. LOCAL only.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Remote node LoRa address (2-254) |
| `device` | Yes | Device index |
| `live` | No | `true` to send a REQUEST(Read) and wait for a fresh response |
| `timeout` | No | Timeout in ms for live read (200-10000, default 3000) |

| Status | Meaning |
|---|---|
| `200` | OK. Body: `MetricsResponse` |
| `400` | Missing or invalid parameters |
| `404` | Device not found |
| `503` | LoRa broker busy (another request in flight) |
| `504` | No response from remote node within timeout |

```json
{"addr":2,"device":1,"name":"SensorA","has_data":true,"age_ms":450,"alert":false,"live":false,"values":[{"metric_id":1,"value":335,"name":"Voltage","unit":"V","scale":100}]}
```

---

## GET /api/lora/stats

Returns cumulative LoRa transport counters since boot. CRC error and ACK loss counters are wired in Phase 3.

**Response:** `200 application/json`

```json
{"tx_frames":42,"rx_frames":38,"crc_errors":0,"ack_loss":0}
```

---

## POST /api/lora/nodes/echo

Sends a REQUEST(Echo) to the remote node. REMOTE reverses the text and returns it, proving end-to-end frame processing. Blocks until RESPONSE or timeout. LOCAL only.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Remote node LoRa address (2-254) |

**Request body:** `application/json`

```json
{"text":"hello world"}
```

| Status | Meaning |
|---|---|
| `200` | Echo response received. Body: `{"addr":2,"seq":7,"status":"ok","echo":"dlrow olleh"}` |
| `400` | Missing `addr`, invalid body, or text exceeds 175 characters |
| `503` | LoRa broker busy |
| `504` | No response from remote node |

---

## GET /api/lora/nodes/ping  /  POST /api/lora/nodes/ping

Sends a PING to the specified remote node and waits for an ACK. Returns the responder address and RTT.
`boot_id` is taken from the last BEACON of that node and is omitted if none has been received yet
(the ACK frame itself carries no `boot_id`). LOCAL only.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Remote node LoRa address (2-254) |

| Status | Meaning |
|---|---|
| `200` | ACK received. Body: `{"addr":2,"rtt_ms":87,"boot_id":305419896}` (`boot_id` optional) |
| `400` | Missing or invalid `addr` |
| `503` | LoRa broker busy |
| `504` | No ACK received |

---

## GET /api/lora/ping

Sends a broadcast PING (addr 0xFF) and returns the first ACK received. Useful for discovering reachable nodes. LOCAL only.

| Status | Meaning |
|---|---|
| `200` | At least one ACK received. Body: `{"addr":2,"rtt_ms":91,"boot_id":305419896}` (`addr` is the node that answered; `boot_id` optional) |
| `503` | LoRa broker busy |
| `504` | No response |

---

## POST /api/lora/beacon

Queues an immediate manual BEACON frame for broadcast transmission. Returns 202 immediately; the frame is sent asynchronously by `loraTransportTask`. LOCAL only.

**Response:** `202 application/json`

```json
{"status":"queued"}
```

---

## POST /api/lora/nodes/data

Encodes a `loraiot_Data` payload and queues it for transmission to a remote node. LOCAL only.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Remote node LoRa address (2-254) |
| `ack` | No | `true` = DATA_ACK (reliable), `false` = DATA (unreliable). Default `false` |

**Request body:** `application/json`

```json
{"device":1,"values":[{"metric_id":1,"value":335},{"metric_id":2,"value":215}]}
```

| Status | Meaning |
|---|---|
| `202` | Frame queued. Body: `{"addr":2,"ack":false,"bytes":12}` |
| `400` | Missing `addr`, invalid body, or nanopb encode failed |

---

## POST /api/sensor/data

REMOTE only. Accepts sensor data from a SoftAP-connected IoT device. Auto-assigns stable `device_idx` and `metric_id` for new keys. Increments `registry_rev` on schema changes. Rejects metrics that would cause the encoded `loraiot_Device` proto to exceed 186 bytes.

**Request body:** `application/json`

```json
{"device":"SensorA","metrics":{"Voltage":{"value":335,"unit":"V","scale":100},"Temp":{"value":215,"unit":"C","scale":10}}}
```

| Status | Meaning |
|---|---|
| `200` | All metrics accepted |
| `400` | Some metrics rejected, or not a REMOTE instance |

**Response body** (both `200` and `400`):

```json
{"accepted":[{"metric":"Voltage","device_idx":1,"metric_id":1},{"metric":"Temp","device_idx":1,"metric_id":2}],"rejected":[],"registry_rev":2}
```
