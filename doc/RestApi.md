# LoRaIoT REST API

Reference for every HTTP and WebSocket endpoint currently registered in the firmware.
Base URL: `http://<device-ip>/` (port 80).
`<device-ip>` depends on the node's WiFi mode (see [`GET /api/wifi/mode`](#get-apiwifimode)): the LAN address
in `STA` and `AP_STA` mode, or the SoftAP address (`apIp`, default `192.168.4.1`, after joining the node's SSID)
in `AP` and `AP_STA` mode.

> **Source of truth:** `src/LoRaIoT/src/openapi_json.h` is now the authoritative API spec,
> served at `GET /openapi` and rendered interactively at `GET /openapi/ui` (Swagger UI).
> This file is kept as a human-readable companion. When they diverge, the OpenAPI spec wins.
> **Rule:** every new endpoint must be added to both files in the same change that registers it.

## Endpoint summary

| Method | Path | Registered in | Node | Purpose |
|---|---|---|---|---|
| GET | `/` | `Server.cpp` | Both | Live dashboard (HTML from PROGMEM, fed by `/ws/data`) |
| GET | `/openapi` | `Server.cpp` | Both | OpenAPI 3.0 specification (JSON, from PROGMEM) |
| GET | `/openapi/ui` | `Server.cpp` | Both | Swagger UI explorer (fetches JS/CSS from unpkg CDN) |
| GET | `/status` | `Server.cpp` | Both | ESP32 hardware diagnostics |
| GET | `/reboot` | `Server.cpp` | Both | Reboot the ESP32, all settings preserved (use this to restart a node) |
| GET | `/reset` | `Server.cpp` | Both | DESTRUCTIVE: clear WiFi credentials, WiFi mode / SoftAP settings and all persisted NVS settings, then reboot. Never call from automation or agents |
| GET | `/api/log` | `Server.cpp` | Both | Read (and optionally clear) the log ring buffer |
| PUT | `/api/log/level` | `Server.cpp` | Both | Change the active log level |
| GET | `/api/tier` | `RestService.cpp` | Both | Return the active gateway tier (LOCAL / REMOTE) |
| GET | `/api/wifi/mode` | `RestService.cpp` | Both | Get active WiFi mode (`STA`, `AP`, `AP_STA`), IP addresses, SoftAP settings and client count |
| PUT | `/api/wifi/mode` | `RestService.cpp` | Both | Set WiFi mode (`STA`, `AP`, `AP_STA`) and SoftAP settings (auto-reboots). Intended for REMOTE nodes |
| GET | `/api/diag/e220` | `RestService.cpp` | Both | Request an E220 wiring and register check |
| GET | `/api/lora/channel` | `RestService.cpp` | Both | Get active LoRa channel |
| PUT | `/api/lora/channel` | `RestService.cpp` | Both | Set active LoRa channel |
| GET | `/api/lora/airrate` | `RestService.cpp` | Both | Get active LoRa air data rate |
| PUT | `/api/lora/airrate` | `RestService.cpp` | Both | Set LoRa air data rate (applied immediately) |
| GET | `/api/lora/address` | `RestService.cpp` | Both | Get active LoRa address |
| PUT | `/api/lora/address` | `RestService.cpp` | REMOTE | Set active LoRa address |
| GET | `/api/system/cpu` | `RestService.cpp` | Both | Get active CPU frequency |
| PUT | `/api/system/cpu` | `RestService.cpp` | Both | Set active CPU frequency (80, 160, 240 MHz) |
| GET | `/api/lora/nodes` | `RestService.cpp` | LOCAL | List all known remote nodes |
| GET | `/api/lora/nodes/sensor` | `RestService.cpp` | Both | Unified device + metric sensor view (`?addr=`, `?device=`, `?live=true`, `?timeout=ms`, `?refresh=true`) |
| GET | `/api/lora/nodes/control` | `RestService.cpp` | Both | Unified device + metric control view (`?addr=`, `?device=`; REMOTE also `?live=true`, `?timeout=ms`, `?refresh=true`) |
| POST | `/api/lora/nodes/control` | `RestService.cpp` | LOCAL | Register control devices and store setpoint values (`?addr=`) |
| GET | `/api/lora/stats` | `RestService.cpp` | LOCAL | LoRa transport TX/RX/CRC/ACK counters |
| POST | `/api/lora/nodes/echo` | `RestService.cpp` | LOCAL | Echo test to a remote node (`?addr=`) |
| GET | `/api/lora/echo/result` | `RestService.cpp` | LOCAL | Return the result of the last echo test (non-blocking) |
| GET,POST | `/api/lora/nodes/ping` | `RestService.cpp` | LOCAL | Unicast ping to a remote node (`?addr=`) |
| GET | `/api/lora/ping` | `RestService.cpp` | LOCAL | Broadcast ping |
| POST | `/api/lora/sensor/data` | `RestService.cpp` | REMOTE | Ingest sensor data from an IoT device (via the SoftAP in `AP` / `AP_STA` mode, or via the LAN address in `STA` / `AP_STA` mode) |
| WS | `/ws/data` | `WebSocketService.cpp` | Both | Live state push (1 s, server to client only) |

---

## GET /status

Returns ESP32 hardware diagnostics as JSON (fields come from `fillESPInfo()` in `ESPInfo.h`).

**Response:** `200 application/json`

---

## GET /reboot

Reboots the ESP32 about 200 ms after the response is sent. WiFi credentials and all NVS settings (tier, log
level, LoRa channel, air rate, address, CPU frequency) are preserved, so the node comes back with the same
configuration within a few seconds. **This is the endpoint to use whenever a node has to be restarted.**

**Response:** `200 text/plain`

---

## GET /reset

> **WARNING - DESTRUCTIVE. Never call this endpoint from scripts, tests, skills or agents.** It wipes the
> node's WiFi credentials and configuration, and the developer must re-provision the node by hand through
> the captive portal. To restart a node use [`GET /reboot`](#get-reboot) instead.

Clears the stored WiFi credentials and every application setting persisted in NVS, then reboots into the
WiFiManager captive portal. The erased settings are the gateway tier, the log level, the LoRa channel,
the LoRa air rate, the LoRa address, the CPU frequency and the WiFi mode with its SoftAP settings (namespaces
`TIER_NVS_NAMESPACE`, `LOG_NVS_NAMESPACE`, `LORA_NVS_NAMESPACE` and `WIFI_NVS_NAMESPACE`, all currently `sc_prefs`).
After the reboot the node uses the built-in defaults (WiFi mode `STA`) until it is provisioned again. This is also
the last-resort recovery if a node was switched to `AP` mode and its SSID or password is lost. While the SoftAP
credentials are known, `PUT /api/wifi/mode` with `{"mode":"STA"}` is the normal way back.

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
| `tier` | string | `"LOCAL"`, `"REMOTE (Node 1)"`, `"REMOTE (Node 2)"`, `"REMOTE (Node 3)"`, or `"UNSET"` if no tier has been selected in the portal yet |
| `configured` | bool | `false` when tier is `"UNSET"` (portal has not been completed) |

**Tier meanings:**
- `LOCAL` - connected to the local WLAN (`STA` mode); receives data from REMOTE nodes (default addr `0x01`).
- `REMOTE (Node 1)` - forwards data over LoRa to LOCAL (default addr `0x02`).
- `REMOTE (Node 2)` - forwards data over LoRa to LOCAL (default addr `0x03`).
- `REMOTE (Node 3)` - forwards data over LoRa to LOCAL (default addr `0x04`).

A REMOTE node is typically run in `AP` or `AP_STA` mode so it hosts a WiFi SoftAP for IoT sensors, but it can also
stay in `STA` mode (the default after provisioning). The tier says nothing about the WiFi mode; read the active mode
from [`GET /api/wifi/mode`](#get-apiwifimode).

The tier is selected in the WiFiManager captive portal and persisted in NVS
(namespace `sc_prefs`, key `tier`). It survives reboots. Clear it with `GET /reset`. The portal only runs in `STA`
and `AP_STA` mode; in `AP` mode WiFiManager is bypassed, so the tier and LoRa settings must be provisioned
(in `STA` mode) before switching a node to `AP`.

---

## GET /api/wifi/mode

Returns the active WiFi operational mode, assigned IP addresses, SoftAP configuration, and connected client count. Available on both tiers.

The three modes:

| Mode | Behaviour | Reachable at |
|---|---|---|
| `STA` | WiFi client of an existing router (default, original behaviour). WiFiManager captive portal runs when the node is unprovisioned. | `staIp` (LAN address assigned by the router) |
| `AP` | Node hosts its own SoftAP and DHCP server and does not join any router. WiFiManager is bypassed at boot. | `apIp` (default `192.168.4.1`) after joining the node's SSID |
| `AP_STA` | Concurrent: the SoftAP is started and the node also joins the router. If the router connection fails the node keeps running as SoftAP only. | `apIp` and, when connected, `staIp` |

**Request body:** none

**Response:** `200 application/json`

`STA` mode:

```json
{
  "mode": "STA",
  "apSsid": "LoRaIoT-Node-1",
  "apIp": "192.168.4.1",
  "apChannel": 1,
  "apClients": 0,
  "staIp": "192.168.1.174"
}
```

`AP` mode (no router, `staIp` is `null`):

```json
{
  "mode": "AP",
  "apSsid": "LoRaIoT-Node-1",
  "apIp": "192.168.4.1",
  "apChannel": 1,
  "apClients": 2,
  "staIp": null
}
```

`AP_STA` mode (both interfaces up):

```json
{
  "mode": "AP_STA",
  "apSsid": "LoRaIoT-Node-1",
  "apIp": "192.168.4.1",
  "apChannel": 1,
  "apClients": 1,
  "staIp": "192.168.1.174"
}
```

| Field | Type | Description |
|---|---|---|
| `mode` | string | Active operational mode: `"STA"`, `"AP"`, or `"AP_STA"` |
| `apSsid` | string | Configured SoftAP SSID. Always reported, also in `STA` mode (the SoftAP is then not running) |
| `apIp` | string | Configured SoftAP host IP address (default `192.168.4.1`). Always reported, also in `STA` mode |
| `apChannel` | integer | Configured SoftAP 2.4 GHz channel (1-13). Always reported, also in `STA` mode |
| `apClients` | integer | Number of stations connected to the SoftAP. Always `0` in `STA` mode |
| `staIp` | string or null | Station IP address when connected to an external router. `null` in `AP` mode, or in `STA` / `AP_STA` mode while not connected |

---

## PUT /api/wifi/mode

Reconfigures the WiFi operational mode (`"STA"`, `"AP"`, or `"AP_STA"`) and SoftAP parameters. All settings are validated and persisted to NVS (namespace `sc_prefs`). Upon successful validation, returns HTTP 200 and automatically restarts the ESP32 after 200 ms so the device boots cleanly into the new mode.

> **Reachability warning.** After the reboot the node is no longer reachable at its old address when the mode
> changes. Switching `STA` to `AP` drops the LAN address: join the node's SSID (`ssid`, default `LoRaIoT-Node-{n}`)
> and use `http://<ip>/` (default `192.168.4.1`). Switching to `AP_STA` keeps the LAN address and adds the SoftAP
> address. Switching back to `STA` is done the same way with `{"mode":"STA"}` through the SoftAP address.
> Provision the tier and LoRa settings in `STA` mode first: in `AP` mode the captive portal is not available.

The node number `{n}` in the default SSID is derived from the LoRa address (`0x02` gives `LoRaIoT-Node-1`, `0x03` gives
`LoRaIoT-Node-2`). The default password is `LoRaIoT`; change it for any field deployment.

**Request body:** `application/json`

```json
{
  "mode": "AP",
  "ssid": "LoRaIoT-Node-1",
  "password": "MySecretPassword",
  "ip": "192.168.4.1",
  "channel": 1
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `mode` | string | Yes | Target operational mode: `"STA"`, `"AP"`, or `"AP_STA"` |
| `ssid` | string | No | SoftAP SSID (1-32 characters). Defaults to `LoRaIoT-Node-{n}` |
| `password` | string | No | SoftAP WPA2 password (empty string for open network, or 8-64 characters). Defaults to `"LoRaIoT"` |
| `ip` | string | No | Custom host IP address for SoftAP (e.g. `"192.168.4.1"`). Automatically derives a `/24` subnet mask |
| `channel` | integer | No | SoftAP 2.4 GHz channel (1-13). Default `1`. In `AP_STA` mode the router's channel typically takes precedence while the node is connected |

Omitted optional fields keep their stored values. Only `mode` is mandatory.

**Response (Success):** `200 application/json`

```json
{
  "status": "rebooting",
  "mode": "AP",
  "apSsid": "LoRaIoT-Node-1",
  "apIp": "192.168.4.1",
  "apChannel": 1
}
```

**Response (Validation Error):** `400 application/json`

```json
{"error":"Password must be empty (open) or at least 8 characters (max 64)"}
```

| `error` text | Cause |
|---|---|
| `Expected JSON with mandatory 'mode' ('STA', 'AP', or 'AP_STA')` | Body is not valid JSON or `mode` is missing |
| `Invalid mode. Expected 'STA', 'AP', or 'AP_STA'` | Unknown `mode` value |
| `SSID exceeds maximum length of 32 characters` | `ssid` longer than 32 characters |
| `Password must be empty (open) or at least 8 characters (max 64)` | `password` is 1-7 or more than 64 characters |
| `Invalid IP address format. Expected e.g. '192.168.4.1'` | `ip` is not a valid dotted IPv4 address |
| `Channel must be between 1 and 13` | `channel` out of range |

**Example** - switch REMOTE to concurrent mode:

```bash
curl -s -X PUT http://192.168.1.174/api/wifi/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"AP_STA","ssid":"LoRaIoT-Node-1","password":"MySecretPassword","channel":1}'
```

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
- Takes UART2 for the duration of the check via `LoRaTransport.pause()` / `resume()` and releases it afterwards.

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

## GET /api/lora/airrate

Returns the currently configured LoRa air data rate. This endpoint is available on both LOCAL and REMOTE instances.

**Response:** `200 application/json`

```json
{"airRate":"62.5k"}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/lora/airrate

Sets the LoRa air data rate. The rate is persisted in NVS (as the `LoRaTransportClass::AirRate` enum ordinal, key `loraAirRate`)
and applied immediately: `loraTransportTask` rewrites the E220 registers (Mode 3, about 100 ms), so no reboot is needed.
Frames sent or received during that moment are lost. This endpoint is available on both LOCAL and REMOTE instances.
The default is `62.5k`; lower rates give more range but a longer air time.

Both nodes must use the same rate, otherwise they cannot hear each other. Change REMOTE first, then LOCAL: the link
re-establishes with the next BEACON/PING after both match. If the order is reversed, REMOTE is unreachable over LoRa and must be changed through its own WiFi address (its LAN address in `STA` / `AP_STA` mode, or its SoftAP address, default `192.168.4.1`, in `AP` / `AP_STA` mode).
The existing timeouts (for example `LORA_REQUEST_TIMEOUT_MS`) are not scaled with the rate; at the slow rates they may need manual tuning.

**Request body:** `application/json`

```json
{"airRate":"9.6k"}
```

Allowed values: `"2.4k"`, `"4.8k"`, `"9.6k"`, `"19.2k"`, `"38.4k"`, `"62.5k"`.

| Status | Meaning |
|---|---|
| `200` | Air rate changed. Body: `{"airRate":"9.6k"}` |
| `400` | Body is not valid JSON, `airRate` is missing, or not one of the allowed values. |

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

## GET /api/lora/nodes/sensor

Returns device definitions and sensor metric values. Available on all nodes.
- **LOCAL (cache view):** returns cached sensor definitions and values received from REMOTE nodes. Supports `?addr=`, `?device=`, `?live=true`, `?timeout=ms`, `?refresh=true`.
- **REMOTE (owner view):** returns own sensor definitions and values from `SensorRegistry`. `addr`, `live`, and `refresh` return 400.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | No | Node LoRa address (1-254). Omit for all known nodes (LOCAL only) |
| `device` | No | Filter to one device index |
| `live` | No | `true` to send a REQUEST(Read) before responding. Requires both `addr` and `device` (LOCAL only) |
| `timeout` | No | Live read timeout in ms (200-4000, default 3000). Only used with `live=true` |
| `refresh` | No | `true` to schedule a Describe crawl of the node and return 202 at once. Requires `addr` (LOCAL only) |

| Status | Meaning |
|---|---|
| `200` | OK. Body: `NodeSnapshotResponse` |
| `202` | `refresh=true`: crawl scheduled. Body: `{"status":"crawl_scheduled","addr":2}` |
| `400` | Invalid parameter combination (e.g. `device` without `addr` on LOCAL, `refresh` without `addr`, or `addr`/`live`/`refresh` on REMOTE) |
| `404` | `addr` specified but node not found |
| `503` | LoRa broker busy (only when `live=true`) |
| `504` | No response from remote node within timeout (only when `live=true`) |

```json
{"nodes":[{"addr":2,"boot_id":305419896,"registry_rev":6,"online":true,"peer_last_seen_ms_ago":4120,"devices":[{"device":1,"name":"PMon A","has_data":true,"age_ms":820,"alert":false,"metrics":[{"id":1,"name":"Voltage","unit":"V","scale":10,"value":2311},{"id":2,"name":"Current","unit":"A","scale":100,"value":380}]},{"device":2,"name":"EnvMon B","has_data":false,"age_ms":0,"alert":false,"metrics":[{"id":1,"name":"Temperature","unit":"C","scale":10,"value":null},{"id":2,"name":"Humidity","unit":"%","scale":10,"value":null}]}]}]}
```

---

## GET /api/lora/nodes/control

Returns device definitions and control metric values. Available on all nodes.
- **LOCAL (owner view):** returns own control device definitions and setpoints from `ControlRegistry`. Supports `?addr=` and `?device=`. `live` and `refresh` return 400.
- **REMOTE (cache view):** returns cached control definitions and setpoints received from LOCAL. `addr` returns 400.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | No | Target node LoRa address (1-254). Omit for all nodes (LOCAL only) |
| `device` | No | Filter to one device index |
| `live` | No | `true` to send a REQUEST(Read) to LOCAL before responding. Requires `device` (REMOTE only; LOCAL returns 400) |
| `timeout` | No | Live read timeout in ms (200-4000, default 3000). Only used with `live=true` |
| `refresh` | No | `true` to schedule a Describe crawl of LOCAL and return 202 at once (REMOTE only; LOCAL returns 400) |

| Status | Meaning |
|---|---|
| `200` | OK. Body: `NodeSnapshotResponse` |
| `202` | `refresh=true`: crawl scheduled. Body: `{"status":"crawl_scheduled"}` |
| `400` | Invalid parameter combination (e.g. `device` without `addr` on LOCAL, `live` without `device`, `addr` on REMOTE, or `live`/`refresh` on LOCAL) |
| `404` | `addr` specified but node not found, or `live=true` and LOCAL returned an error |
| `503` | LoRa broker busy (only when `live=true`) |
| `504` | No response from LOCAL within timeout (only when `live=true`) |

```json
{"nodes":[{"addr":2,"boot_id":305419896,"registry_rev":1,"online":true,"peer_last_seen_ms_ago":2100,"devices":[{"device":1,"name":"Charger","has_data":true,"age_ms":120,"alert":false,"metrics":[{"id":1,"name":"VoltageSet","unit":"V","scale":100,"value":333}]}]}]}
```

### Freshness fields: `age_ms` and `peer_last_seen_ms_ago`

Both views on both nodes carry the same two freshness signals, so a consumer (for example
a REMOTE that applies a LOCAL setpoint) can build its own fail-safe:

| Field | Level | Meaning |
|---|---|---|
| `age_ms` | device | Milliseconds since the oldest metric value of the device was stored. `0` with `has_data:false` means no value yet. |
| `online` | node | `false` after 3 unanswered keep-alive PINGs, or when the peer is not yet known. |
| `peer_last_seen_ms_ago` | node | Milliseconds since the last valid frame from the peer (any frame type). `0` when the peer is not yet known (then `online` is `false`). |

Owner views report the liveness of the consumer peer as tracked by the other registry on the
same node (LOCAL control owner view: `SensorRegistry`; REMOTE sensor owner view: `ControlRegistry`).

Recommended consumer rule: treat a value as trustworthy only when `online` is `true`,
`has_data` is `true`, `age_ms` is below your own staleness limit, and `peer_last_seen_ms_ago`
is below your link-loss limit. `age_ms` says how old the value is; `peer_last_seen_ms_ago`
says whether the source is still reachable. A small `age_ms` with a large
`peer_last_seen_ms_ago` is a recent value from a peer that has since gone quiet. Otherwise
fall back to your safe state, optionally after `?refresh=true` or `?live=true&device=N`.

---

## POST /api/lora/nodes/control

LOCAL only. Registers control devices and metrics in `ControlRegistry[addr]`, stores the setpoint values, and pushes the updated device data to the remote node via DATA_ACK with retry.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Target remote node LoRa address (2-254) |

**Request body:** `application/json`

```json
{"device":"Charger","metrics":{"VoltageSet":{"value":333,"unit":"V","scale":100},"Mode":{"value":"auto"},"Active":{"value":true}}}
```

Value types:
- **Numeric:** integer or float (e.g. `333`), with optional `scale` (divisor, default 1).
- **String:** opaque text up to 12 bytes UTF-8 (e.g. `"auto"`, `"enabled"`, `"30.10.2026"`). JSON type is the discriminator (even if digits only, a JSON string is treated as text). `scale` is ignored for strings.
- **Boolean:** JSON `true`/`false` is stored as text `"true"`/`"false"`.
- Strings longer than 12 bytes, `null`, arrays, or objects are rejected (listed in `rejected`, HTTP 400).

| Status | Meaning |
|---|---|
| `200` | Metrics accepted and remote acknowledged (`delivery: "ok"`) |
| `202` | Metrics accepted but delivery deferred: schema changed (PING sent, REMOTE may not have crawled yet) or REMOTE answered BAD_REQUEST (crawl scheduled). `delivery: "after_describe"` |
| `400` | Invalid JSON, missing `addr`/`device`/`metrics`, or all metrics rejected. No delivery attempted. |
| `404` | Target node `addr` not found |
| `503` | LoRa broker busy (`delivery: "busy"`) |
| `504` | No ACK from remote node after all retries (`delivery: "timeout"`) |

**Response body** (`200`, `202`, `503`, `504`):

```json
{"accepted":[{"metric":"VoltageSet","device_idx":1,"metric_id":1}],"rejected":[],"registry_rev":1,"delivery":"ok"}
```

`delivery` values: `"ok"` | `"after_describe"` | `"busy"` | `"timeout"`

**Response body** (`400`):

```json
{"accepted":[],"rejected":["BadMetric"],"registry_rev":1}
```

LOCAL stores the setpoint value regardless of delivery outcome. The remote will receive the latest value once the crawl completes (Step 12).

---

## GET /api/lora/stats

Returns cumulative LoRa transport counters since boot.

**Response:** `200 application/json`

```json
{"tx_frames":42,"rx_frames":38,"crc_errors":0,"ack_loss":0}
```

---

## POST /api/lora/nodes/echo

Sends a REQUEST(Echo) to the remote node. REMOTE reverses the text and returns it, proving end-to-end frame processing. Blocks until RESPONSE or timeout. LOCAL only.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Node LoRa address (1-254) |

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

## GET /api/lora/echo/result

Returns the result of the last `POST /api/lora/nodes/echo` call without blocking. The result is
stored in RAM by the echo handler and survives until the next echo call replaces it or the node reboots.
LOCAL only.

**Request body:** none

| Status | Meaning |
|---|---|
| `200` | Result available. |
| `404` | No echo has been performed since boot. Body: `{"error":"no echo result yet"}` |

**Response body** (`200`):

```json
{"sent":"hello world","received":"dlrow olleh","seqNo":7,"roundTripMs":87,"success":true}
```

| Field | Type | Description |
|---|---|---|
| `sent` | string | The text that was sent |
| `received` | string | The text returned by the remote node (empty string on failure) |
| `seqNo` | uint32 | Frame sequence number of the echo request |
| `roundTripMs` | uint32 | Round-trip time in milliseconds |
| `success` | bool | `true` when a valid echo response was received |

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


## POST /api/lora/sensor/data

REMOTE only. Accepts sensor data from a SoftAP-connected IoT device. Auto-assigns stable `device_idx` and `metric_id` for new keys. Increments `registry_rev` on schema changes. Rejects metrics that would cause the encoded `loraiot_Device` proto to exceed 186 bytes.

**Request body:** `application/json`

```json
{"device":"SensorA","metrics":{"Voltage":{"value":335,"unit":"V","scale":100},"Date":{"value":"30.10.2026"},"Enabled":{"value":true}}}
```

Value types:
- **Numeric:** integer or float (e.g. `335`), with optional `scale` (divisor, default 1).
- **String:** opaque text up to 12 bytes UTF-8 (e.g. `"30.10.2026"`, `"10:45:00"`, `"enabled"`). JSON type is the discriminator (even if digits only, a JSON string is treated as text). `scale` is ignored for strings.
- **Boolean:** JSON `true`/`false` is stored as text `"true"`/`"false"`.
- Strings longer than 12 bytes, `null`, arrays, or objects are rejected (listed in `rejected`, HTTP 400).

| Status | Meaning |
|---|---|
| `200` | All metrics accepted |
| `400` | Some metrics rejected, or not a REMOTE instance |

**Response body** (both `200` and `400`):

```json
{"accepted":[{"metric":"Voltage","device_idx":1,"metric_id":1},{"metric":"Temp","device_idx":1,"metric_id":2}],"rejected":[],"registry_rev":2}
```

---

## WebSocket /ws/data

Pushes a JSON state snapshot to all connected clients once per second. It drives the browser
dashboard served at `GET /`. The endpoint is server-push only: messages sent by a client are
ignored. Nothing is sent while no client is connected.

`nodes[]` has the same shape as `GET /api/lora/nodes/sensor` on LOCAL and
`GET /api/lora/nodes/control` on REMOTE (without `boot_id` and `registry_rev`), and is `[]` when
no peer is known.

```json
{
  "uptime_s": 312,
  "tier": "LOCAL",
  "lora": {"tx_frames": 20, "rx_frames": 23, "crc_errors": 0, "ack_loss": 0},
  "nodes": [
    {
      "addr": 3, "online": true, "peer_last_seen_ms_ago": 53000,
      "devices": [
        {
          "device": 1, "name": "Lenovo W510", "has_data": true, "age_ms": 247000, "alert": false,
          "metrics": [{"id": 1, "name": "CPU Temp.", "unit": "C", "scale": 100, "value": 7146}]
        }
      ]
    }
  ],
  "echo": null
}
```

`echo` is `null` until an echo test has run, then
`{"sent":"...","received":"...","seq_no":n,"round_trip_ms":n,"success":true}`.
`value` is `null` for a metric that has not received data yet; the real value is `value / scale`.
