# LoRaIoT REST API

Reference for every HTTP and WebSocket endpoint currently registered in the firmware.
Base URL: `http://<device-ip>/` (port 80).

> **Source of truth:** `src/LoRaIoT/src/openapi_json.h` is now the authoritative API spec,
> served at `GET /openapi` and rendered interactively at `GET /openapi/ui` (Swagger UI).
> This file is kept as a human-readable companion. When they diverge, the OpenAPI spec wins.
> **Rule:** every new endpoint must be added to both files in the same change that registers it.

## Endpoint summary

| Method | Path | Registered in | Purpose |
|---|---|---|---|
| GET | `/` | `Server.cpp` | Browser UI (HTML from PROGMEM) |
| GET | `/openapi` | `Server.cpp` | OpenAPI 3.0 specification (JSON, from PROGMEM) |
| GET | `/openapi/ui` | `Server.cpp` | Swagger UI explorer (fetches JS/CSS from unpkg CDN) |
| GET | `/status` | `Server.cpp` | ESP32 hardware diagnostics |
| GET | `/reset` | `Server.cpp` | Clear WiFi and all persisted NVS settings, then reboot |
| GET | `/api/log` | `Server.cpp` | Read (and optionally clear) the log ring buffer |
| PUT | `/api/log/level` | `Server.cpp` | Change the active log level |
| GET | `/api/tier` | `RestService.cpp` | Return the active gateway tier (LOCAL / REMOTE) |
| GET | `/api/diag/e220` | `RestService.cpp` | Request an E220 wiring and register check |
| GET | `/api/lora/channel` | `RestService.cpp` | Get active LoRa channel |
| PUT | `/api/lora/channel` | `RestService.cpp` | Set active LoRa channel |
| GET | `/api/lora/address` | `RestService.cpp` | Get active LoRa address |
| PUT | `/api/lora/address` | `RestService.cpp` | Set active LoRa address (REMOTE only) |
| GET | `/api/system/cpu` | `RestService.cpp` | Get active CPU frequency |
| PUT | `/api/system/cpu` | `RestService.cpp` | Set active CPU frequency (80, 160, 240 MHz) |
| WS | `/ws/data` | `WebSocketService.cpp` | Live state push (1 s) |

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
