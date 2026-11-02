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
| GET | `/api/mqtt/config` | `RestService.cpp` | LOCAL | Get MQTT broker configuration (password masked) |
| PUT | `/api/mqtt/config` | `RestService.cpp` | LOCAL | Update MQTT broker configuration (partial update, applied live) |
| GET | `/api/mqtt/status` | `RestService.cpp` | LOCAL | Live MQTT connection state |
| WS | `/ws/data` | `WebSocketService.cpp` | Both | Live state push (1 s, server to client only) |

**Tier-specific endpoints:** they are registered only on the tier that supports them. On the other tier the path does not exist (default `404`). Endpoints marked `Both` whose behaviour differs per tier (`/api/lora/nodes/sensor`, `/api/lora/nodes/control`) validate parameters inside the handler and answer `400` for parameters that do not apply to the running tier.

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
{
  "level": "INFO",
  "log": [
    "[INFO ][Application.cpp:30] Starting LoRaIoT ..."
  ]
}
```

---

## PUT /api/log/level

Sets the active log level. The level is persisted in NVS and survives reboots.

**Request body:** `application/json`

```json
{
  "level": "DEBUG"
}
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
{
  "tier": "LOCAL",
  "configured": true
}
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
{
  "error": "Password must be empty (open) or at least 8 characters (max 64)"
}
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
{
  "status": "requested",
  "result": "see GET /api/log"
}
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
{
  "channel": 23
}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/lora/channel

Sets the active LoRa channel. The new channel is persisted in NVS and survives reboots. This endpoint is available on both LOCAL and REMOTE instances.

**Request body:** `application/json`

```json
{
  "channel": 24
}
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
{
  "airRate": "62.5k"
}
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
{
  "airRate": "9.6k"
}
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
{
  "address": 1
}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/lora/address

Sets the active LoRa address. The new address is persisted in NVS and survives reboots. The E220 module is programmed at boot, so the new address takes effect after the next reboot. This endpoint is only available on REMOTE instances. The route is not registered on LOCAL nodes, which answer `404`.

**Request body:** `application/json`

```json
{
  "address": 2
}
```

Allowed values: integer between 2 and 254 (inclusive). `1` is reserved for the LOCAL node.

| Status | Meaning |
|---|---|
| `200` | Address changed. Body: `{"address":2}` |
| `400` | Body is not valid JSON, `address` is missing, or out of range |

---

## GET /api/system/cpu

Returns the currently active CPU frequency of the ESP32 in MHz.

**Response:** `200 application/json`

```json
{
  "frequency": 240
}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/system/cpu

Dynamically scales the ESP32's CPU clock frequency to optimize power consumption. The chosen frequency is persisted in NVS and automatically applied on reboots.

**Request body:** `application/json`

```json
{
  "frequency": 80
}
```

Supported frequencies (with active WiFi radio):
- `80` - 80 MHz (Minimum required clock speed for the ESP32 WiFi radio PLL)
- `160` - 160 MHz
- `240` - 240 MHz (Default high-performance clock speed)

**Response:** `200 application/json`

```json
{
  "frequency": 80
}
```

| Status | Meaning |
|---|---|
| `200` | CPU frequency updated. |
| `400` | Body is not valid JSON, `frequency` is missing, or is not a supported value (80, 160, or 240). |

---

---

## GET /api/lora/nodes

Returns all remote node addresses that LOCAL has observed via BEACON. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

`online` is `false` when a keep-alive check failed (3 unanswered PINGs after 90 s of silence).
Offline nodes stay in the list with their cached definitions; the next frame from the node
sets `online` back to `true`, and no new Describe crawl happens if `boot_id` and
`registry_rev` are unchanged. A slot is only reused (oldest offline node first) when all
slots are taken and a new node appears.

**Response:** `200 application/json`

```json
{
  "nodes": [
    {
      "addr": 2,
      "boot_id": 305419896,
      "registry_rev": 3,
      "last_seen_ms": 12450,
      "online": true,
      "devices": 2,
      "crawl_pending": false
    }
  ]
}
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
{
  "nodes": [
    {
      "addr": 2,
      "boot_id": 305419896,
      "registry_rev": 6,
      "online": true,
      "peer_last_seen_ms_ago": 4120,
      "devices": [
        {
          "device": 1,
          "name": "Env-Sensor",
          "has_data": true,
          "age_ms": 820,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "Temperature",
              "unit": "C",
              "scale": 10,
              "value": 215
            },
            {
              "id": 2,
              "name": "Humidity",
              "unit": "%",
              "scale": 10,
              "value": 483
            },
            {
              "id": 3,
              "name": "AirPressure",
              "unit": "",
              "scale": 1,
              "value": "incr."
            },
            {
              "id": 4,
              "name": "Firmware",
              "unit": "vers.",
              "scale": 100,
              "value": 110
            },
            {
              "id": 5,
              "name": "Daylight",
              "unit": "",
              "scale": 1,
              "value": null
            }
          ]
        }
      ]
    }
  ]
}
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
{
  "nodes": [
    {
      "addr": 2,
      "boot_id": 305419896,
      "registry_rev": 4,
      "online": true,
      "peer_last_seen_ms_ago": 2100,
      "devices": [
        {
          "device": 1,
          "name": "Power-Supply",
          "has_data": true,
          "age_ms": 120,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "Voltage",
              "unit": "V",
              "scale": 100,
              "value": 1200
            },
            {
              "id": 2,
              "name": "Current",
              "unit": "A",
              "scale": 10,
              "value": 50
            },
            {
              "id": 3,
              "name": "Pwr",
              "unit": "",
              "scale": 1,
              "value": "off"
            }
          ]
        }
      ]
    }
  ]
}
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

LOCAL only: the route is not registered on REMOTE nodes, which answer `404`. Registers control devices and metrics in `ControlRegistry[addr]`, stores the setpoint values, and pushes the updated device data to the remote node via DATA_ACK with retry. The same logic is also reachable via the MQTT `/set` topic (`<prefix>/Local/Remote_Node_<n>/<device>/<metric>/set`) - see [Architecture.md section 8.6](Architecture.md#86-control-via-the-set-topic).

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Target remote node LoRa address (2-254) |

**Request body:** `application/json`

```json
{
  "device": "Power-Supply",
  "metrics": {
    "Voltage": {
      "value": 1200,
      "unit": "V",
      "scale": 100
    },
    "Current": {
      "value": 50,
      "unit": "A",
      "scale": 10
    },
    "Pwr": {
      "value": "off"
    }
  }
}
```

Field types, value types and the hard capacity limits are the same as for sensor data: see [Request body schema and field limits](#request-body-schema-and-field-limits) under `POST /api/lora/sensor/data`.

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
{
  "accepted": [
    {
      "metric": "Voltage",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Current",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "Pwr",
      "device_idx": 1,
      "metric_id": 3
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 4,
  "delivery": "ok"
}
```

`delivery` values: `"ok"` | `"after_describe"` | `"busy"` | `"timeout"`

**Response body** (`400`):

```json
{
  "accepted": [
    
  ],
  "rejected": [
    "BadMetric"
  ],
  "registry_rev": 4
}
```

LOCAL stores the setpoint value regardless of delivery outcome. The remote will receive the latest value once the crawl completes (Step 12).

---

## GET /api/lora/stats

Returns cumulative LoRa transport counters since boot. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

**Response:** `200 application/json`

```json
{
  "tx_frames": 42,
  "rx_frames": 38,
  "crc_errors": 0,
  "ack_loss": 0
}
```

---

## POST /api/lora/nodes/echo

Sends a REQUEST(Echo) to the remote node. REMOTE reverses the text and returns it, proving end-to-end frame processing. Blocks until RESPONSE or timeout. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Node LoRa address (1-254) |

**Request body:** `application/json`

```json
{
  "text": "hello world"
}
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
LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

**Request body:** none

| Status | Meaning |
|---|---|
| `200` | Result available. |
| `404` | No echo has been performed since boot. Body: `{"error":"no echo result yet"}` |

**Response body** (`200`):

```json
{
  "sent": "hello world",
  "received": "dlrow olleh",
  "seqNo": 7,
  "roundTripMs": 87,
  "success": true
}
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
(the ACK frame itself carries no `boot_id`). LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

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

Sends a broadcast PING (addr 0xFF) and returns the first ACK received. Useful for discovering reachable nodes. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

| Status | Meaning |
|---|---|
| `200` | At least one ACK received. Body: `{"addr":2,"rtt_ms":91,"boot_id":305419896}` (`addr` is the node that answered; `boot_id` optional) |
| `503` | LoRa broker busy |
| `504` | No response |

---


## POST /api/lora/sensor/data

REMOTE only: the route is not registered on LOCAL nodes, which answer `404`. Accepts sensor data from a SoftAP-connected IoT device. Auto-assigns stable `device_idx` and `metric_id` for new keys. Increments `registry_rev` on schema changes and immediately sends a PING to LOCAL carrying the new revision, so LOCAL starts the Describe crawl before the DATA frame even arrives. Rejects metrics that would cause the encoded `loraiot_Device` proto to exceed 186 bytes.

**Request body:** `application/json`

```json
{
  "device": "Env-Sensor",
  "metrics": {
    "Temperature": {
      "value": 215,
      "unit": "C",
      "scale": 10
    },
    "Humidity": {
      "value": 483,
      "unit": "%",
      "scale": 10
    },
    "AirPressure": {
      "value": "incr."
    },
    "Firmware": {
      "value": 110,
      "unit": "vers.",
      "scale": 100
    },
    "Daylight": {
      "value": true
    }
  }
}
```

Field types, value types and the hard capacity limits are described in [Request body schema and field limits](#request-body-schema-and-field-limits) below.

| Status | Meaning |
|---|---|
| `200` | All metrics accepted |
| `400` | Invalid JSON, missing fields, or some metrics rejected |

**Response body** (both `200` and `400`):

```json
{
  "accepted": [
    {
      "metric": "Temperature",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Humidity",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "AirPressure",
      "device_idx": 1,
      "metric_id": 3
    },
    {
      "metric": "Firmware",
      "device_idx": 1,
      "metric_id": 4
    },
    {
      "metric": "Daylight",
      "device_idx": 1,
      "metric_id": 5
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 6
}
```

### Request body schema and field limits

Both `POST /api/lora/sensor/data` (REMOTE) and `POST /api/lora/nodes/control` (LOCAL) share
the same JSON body shape. The annotated schema below shows every field, its JSON type, and the
hard limits enforced by `DeviceTable.h`. These limits also apply to the MQTT `/set` payload
(which carries one metric object) and to the topic levels derived from `device` and metric key
names.

```
{
  "device":  <string, max 12 chars>,    // device name; also the MQTT topic level
                                        // max 4 distinct devices per REMOTE node (node-wide hard cap)
  "metrics": {
    "<metric-key>": {                   // metric name, max 12 chars; also the MQTT topic level
                                        // max 8 metrics per device (node-wide hard cap)
      "value":  <integer|string|bool>,  // see value types below
      "unit":   <string, max 5 chars>,  // optional, default ""; stored as metadata only
      "scale":  <integer >= 1>          // optional, default 1; 0 treated as 1
    }
  }
}
```

> **Hard caps (node-wide, not per-request):** a REMOTE node holds at most **4 devices** and
> each device holds at most **8 metrics**. These slots are allocated on first use and never
> freed until the node reboots. A POST that would exceed either cap is rejected with HTTP 400
> and the metric appears in the `rejected` array of the response. (On `POST /api/lora/nodes/control`
> HTTP 400 is returned only when no metric at all is accepted; otherwise the status follows the
> delivery result and the metric still appears in `rejected`.)

#### Field Reference

| Field | JSON type | Max length / range | Default | Notes |
|---|---|---|---|---|
| `device` | string | 12 chars | - | Required. Name stored in registry and used as MQTT topic level (sanitized). |
| metric key (object key inside `"metrics"`) | string | 12 chars | - | Required. Name stored in registry and used as MQTT topic level (sanitized). |
| `value` (numeric) | integer or float | int32 range (-2 147 483 648 to 2 147 483 647) | - | Stored as `int32`; floats are truncated. Pass the pre-scaled integer, e.g. `215` for 21.5 with scale 10. |
| `value` (text) | string | 12 bytes UTF-8 | - | Stored verbatim. Strings over 12 bytes are rejected (HTTP 400). Use for state labels such as `"on"`, `"off"`, `"auto"`. |
| `value` (boolean) | `true` / `false` | - | - | Stored as the text strings `"true"` / `"false"`. |
| `unit` | string | 5 chars | `""` | Optional. Stored as metadata. In an MQTT `/set` payload it is used only when the metric is first registered. Never published and never in the topic path. |
| `scale` | integer | >= 1; 0 treated as 1 | `1` | Optional. Divisor: real value = `value / scale`. MQTT payload is the formatted decimal, e.g. `"21.5"`. |

Further rules for `value`:

- **String:** opaque text, for example `"30.10.2026"`, `"10:45:00"`, `"enabled"`. JSON type is the discriminator (even if digits only, a JSON string is treated as text). `scale` is ignored for strings.
- A `value` that is `null`, an array or an object is rejected (listed in `rejected`, HTTP 400), like a string over 12 bytes.

#### Sanitization Rule for Topic Levels

`device` and metric key names are stored as-is in the registry. When building an MQTT topic
level the firmware replaces every character outside `[A-Z a-z 0-9 _ -]` with `_`. The `/set`
parser matches incoming topic levels back to registry names via their sanitized form, so
`Power_Supply` in a topic matches a registered device `"Power Supply"`. The topic scheme itself
is described in [Architecture.md section 8.5](Architecture.md#85-topic-scheme-and-payloads).

<details>
<summary>Full capacity limits with source constant names (click to expand)</summary>

| Resource | Limit | Source constant in `DeviceTable.h` |
|---|---|---|
| Devices per REMOTE node | 4 | `DEVICE_TABLE_MAX_DEVICES` |
| Metrics per device | 8 | `DEVICE_TABLE_MAX_METRICS` |
| Device / metric name length | 12 chars | `DEVICE_TABLE_NAME_LEN - 1` |
| Unit string length | 5 chars | `DEVICE_TABLE_UNIT_LEN - 1` |
| Text value length | 12 bytes UTF-8 | `DEVICE_TABLE_TEXT_MAX_LEN` |

These limits apply equally to sensor data (REMOTE) and control setpoints (LOCAL).

</details>

---

## WebSocket /ws/data

Pushes a JSON state snapshot to all connected clients once per second. It drives the browser
dashboard served at `GET /`. The endpoint is server-push only: messages sent by a client are
ignored. Nothing is sent while no client is connected.

`mqtt` is the MQTT connection state (`idle`, `disconnected`, `connected` or `error`, same values as
`state` in `GET /api/mqtt/status`). It is present on LOCAL only and absent on REMOTE. The dashboard
shows a globe icon (U+1F310) in front of the spider icon only when the key is present: full colour
when `connected`, grey when `disconnected` or `error`, faint when `idle`.

`nodes[]` has the same shape as `GET /api/lora/nodes/sensor` on LOCAL and
`GET /api/lora/nodes/control` on REMOTE (without `boot_id` and `registry_rev`), and is `[]` when
no peer is known.

`crawl_pending` is `true` while a Describe crawl is in progress for that peer (schema is being
fetched). The dashboard spider icon is lit while any node has `crawl_pending: true`.

```json
{
  "uptime_s": 312,
  "tier": "LOCAL",
  "lora": {
    "tx_frames": 20,
    "rx_frames": 23,
    "crc_errors": 0,
    "ack_loss": 0
  },
  "mqtt": "connected",
  "nodes": [
    {
      "addr": 3,
      "online": true,
      "peer_last_seen_ms_ago": 53000,
      "crawl_pending": false,
      "devices": [
        {
          "device": 1,
          "name": "Lenovo W510",
          "has_data": true,
          "age_ms": 247000,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "CPU Temp.",
              "unit": "C",
              "scale": 100,
              "value": 7146
            }
          ]
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

---

## GET /api/mqtt/config

Returns the current MQTT broker configuration. JSON keys match the NVS key names exactly. The password is never returned; `mqttPassSet` indicates whether one is stored. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

**Response:** `200 application/json`

```json
{
  "mqttBroker": "192.168.1.10",
  "mqttPort": 1883,
  "mqttUser": "mqttuser",
  "mqttPassSet": true,
  "mqttPrefix": "LoRaIoT",
  "mqttRetain": true,
  "mqttDiscovery": false,
  "mqttEnabled": true
}
```

---

## PUT /api/mqtt/config

Updates MQTT broker settings. Partial update: any omitted field keeps its current stored value. Persisted to NVS and applied live - the broker connection is re-established immediately without a reboot. JSON keys match the NVS key names. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

**Request body:** `application/json` (all fields optional)

```json
{
  "mqttBroker": "192.168.1.10",
  "mqttPort": 1883,
  "mqttUser": "mqttuser",
  "mqttPass": "change-me",
  "mqttPrefix": "LoRaIoT",
  "mqttRetain": true,
  "mqttDiscovery": false,
  "mqttEnabled": true
}
```

| Key | Type | Limits | Default | Meaning |
|---|---|---|---|---|
| `mqttBroker` | string | max 63 chars | `""` | Broker hostname or IPv4 address |
| `mqttPort` | integer | 1-65535 | `1883` | Broker TCP port |
| `mqttUser` | string | max 31 chars | `""` | Optional username |
| `mqttPass` | string | max 31 chars | `""` | Optional password. Omit to keep current; set to `""` to clear. Never returned. |
| `mqttPrefix` | string | 1-31 chars | `"LoRaIoT"` | Root topic prefix; no `+`, `#`, spaces, or leading/trailing `/` |
| `mqttRetain` | boolean | - | `true` | Publish telemetry and status topics with retain=true |
| `mqttDiscovery` | boolean | - | `false` | Home Assistant MQTT auto-discovery (Sub-Task 6, currently unused) |
| `mqttEnabled` | boolean | - | `false` | Master enable switch for the MQTT service |

| Status | Meaning |
|---|---|
| `200` | Settings accepted, persisted, and applied |
| `400` | Validation error (message in `error` field) |

**Response body (`200`):** same format as `GET /api/mqtt/config`.

---

## GET /api/mqtt/status

Returns the live MQTT connection state and diagnostics. Fields are conditional - only present when relevant. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

| Field | Presence | Meaning |
|---|---|---|
| `enabled` | always | The configured `mqttEnabled` switch |
| `state` | always | Live connection: `idle`, `disconnected`, `connected`, `error` |
| `reason` | only when `state` is `idle` | `"disabled"` (switch off) or `"no broker"` (enabled but broker empty) |
| `broker`, `port` | only when a broker is configured | As configured |
| `lastError` | only when `state` is `error` | lwmqtt error code from the last failed connect (`client.lastError()`) |
| `returnCode` | only when `state` is `error` and non-zero | MQTT CONNACK return code from the broker |
| `retryInSec` | only when `state` is `error` or `disconnected` | Seconds until the next reconnect attempt |
| `since` | always | Uptime seconds (`millis()/1000`) when `state` last changed |

`state` values:
- `"idle"` - not active; see `reason` for why.
- `"disconnected"` - enabled and broker set, currently attempting to reconnect.
- `"connected"` - broker link is up, publishing telemetry.
- `"error"` - last connect attempt failed; retrying with exponential backoff (2 s to 64 s).

`lastError` is the raw `int` returned by `arduino-mqtt` / lwmqtt `client.lastError()`. Common values are library-specific (negative = lwmqtt internal errors, positive = CONNACK codes). Check the arduino-mqtt source under `.pio/libdeps/*/MQTT/` for the full `lwmqtt_err_t` enum.

**Examples:**

```json
{
  "enabled": true,
  "state": "connected",
  "broker": "192.168.1.10",
  "port": 1883,
  "since": 412
}
```

```json
{
  "enabled": false,
  "state": "idle",
  "reason": "disabled",
  "since": 35
}
```

```json
{
  "enabled": true,
  "state": "error",
  "broker": "192.168.1.10",
  "port": 1883,
  "lastError": -3,
  "retryInSec": 16,
  "since": 120
}
```

For the full MQTT topic scheme (telemetry, availability, control `/set`, LWT) see [Architecture.md sections 8.5 to 8.7](Architecture.md#85-topic-scheme-and-payloads).

---

## Worked Examples

This section shows end-to-end REST examples derived directly from the running firmware
code. Each example gives the curl command that an IoT device or home-automation script sends
and the expected HTTP response from the handler. All values travel as scaled integers; the
real value is `value / scale`. Examples 1 and 2 are the detailed walk-throughs; examples 3 to
5 add the read-back calls and a live read.

### Address placeholders

The commands below use placeholders instead of hard-coded IP addresses because every
installation is different:

<details>
<summary>Address placeholder reference (click to expand)</summary>

| Placeholder | What to substitute |
|---|---|
| `<Local>` | Hostname or IP address of the LOCAL node (e.g. `192.168.1.147`). |
| `<Remote-Node-x>` | Hostname or IP address of the target REMOTE node (x = 1, 2, or 3). Each REMOTE has its own address; its IP address is reported by `GET /api/wifi/mode` on the node itself (`staIp` / `apIp`), and its LoRa address (the `addr` value) is listed by `GET /api/lora/nodes` on LOCAL or returned by `GET /api/lora/address` on the REMOTE node itself. In AP / AP_STA mode the SoftAP address is `192.168.4.1` by default. |
| `<broker>` | Hostname or IP address of the MQTT broker (e.g. `192.168.1.117`). |

</details>

---

### Example 1 - Environmental Sensor (REMOTE, sensor data POST)

#### Scenario

An IoT sensor board running a small HTTP client joins the REMOTE node SoftAP and POSTs
temperature, humidity, air pressure trend, firmware version and daylight readings. LOCAL
receives the DATA_ACK frame over LoRa.

**Assumptions:**
- REMOTE node address: `<Remote-Node-1>` (substitute the actual hostname or IP).

#### curl Command

**Bash / sh:**

```bash
curl -s -X POST http://<Remote-Node-1>/api/lora/sensor/data \
  -H "Content-Type: application/json" \
  -d '{"device":"Env-Sensor",
       "metrics":{"Temperature":{"value":215,"unit":"C","scale":10},
                  "Humidity":   {"value":483,"unit":"%","scale":10},
                  "AirPressure":{"value":"incr."},
                  "Firmware":   {"value":110,"unit":"vers.","scale":100},
                  "Daylight":   {"value":true}}}'
```

**PowerShell note:** PowerShell's `curl` is an alias for `Invoke-WebRequest`. Use `curl.exe`
explicitly and escape the inner double quotes with a backslash, or wrap the JSON body in a
here-string. Recommended approach:

```powershell
$body = '{"device":"Env-Sensor","metrics":{"Temperature":{"value":215,"unit":"C","scale":10},"Humidity":{"value":483,"unit":"%","scale":10},"AirPressure":{"value":"incr."},"Firmware":{"value":110,"unit":"vers.","scale":100},"Daylight":{"value":true}}}'
curl.exe -s -X POST http://<Remote-Node-1>/api/lora/sensor/data `
  -H "Content-Type: application/json" `
  -d $body
```

#### Request Body Explained

The `value` field of a numeric metric is the **scaled integer** - the physical reading multiplied by `scale`:
- Temperature 21.5 C: `21.5 * 10 = 215` (integer).
- Humidity 48.3 %: `48.3 * 10 = 483` (integer).
- Firmware version 1.10: `1.10 * 100 = 110` (integer), with the unit `"vers."`.

The other two metrics are not numbers:
- `AirPressure`: the JSON string `"incr."` (rising trend; `"decr."` for falling). Text metric, no `unit` or `scale` needed (ignored for strings).
- `Daylight`: the JSON boolean `true`. It is stored as the text `"true"` (see [value types in the field reference](#request-body-schema-and-field-limits)).

The `unit` field (`"C"`, `"%"`, `"vers."`) is stored as metadata and forwarded verbatim. It does not
appear in the MQTT topic name; only the metric name (`"Temperature"`, `"Humidity"`, ...) is used
to build the topic level (see [Architecture.md section 8.5](Architecture.md#85-topic-scheme-and-payloads)).

#### Expected HTTP Response

**`200 OK`** (all metrics accepted):

```json
{
  "accepted": [
    {
      "metric": "Temperature",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Humidity",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "AirPressure",
      "device_idx": 1,
      "metric_id": 3
    },
    {
      "metric": "Firmware",
      "device_idx": 1,
      "metric_id": 4
    },
    {
      "metric": "Daylight",
      "device_idx": 1,
      "metric_id": 5
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 6
}
```

`registry_rev` increments only on schema changes (new device or new metric name), once per new
device and once per new metric name. The first POST above on a freshly booted node therefore
returns `6` (1 device + 5 metrics). A repeat POST with the same device/metric names returns the
same `registry_rev` value and updates only the stored value. See the status code table of
[`POST /api/lora/sensor/data`](#post-apilorasensordata) for the full list of status codes.

#### Notes

- `POST /api/lora/sensor/data` is a **REMOTE-only** endpoint. Sending it to the LOCAL node
  returns `404`.
- Numeric values are truncated to `int32` by `valVar.as<int32_t>()` in the request handler
  (`controlApplyMetric` for control setpoints). Do not pass floating-point JSON values expecting
  implicit scaling - pass the pre-scaled integer.
- LOCAL publishes the stored metrics to the MQTT broker; the matching topics and a
  `mosquitto_sub` session are in
  [Architecture.md section 8.7, Example 1](Architecture.md#example-1---environmental-sensor-subscribe-and-see-the-topics).

---

### Example 2 - Power Supply Control (LOCAL, control setpoint POST)

#### Scenario

A home-automation script or Node-RED flow sets the output voltage, current limit, and power
state of a DC power supply connected to a REMOTE node. The device is named `"Power-Supply"`.
LOCAL stores the setpoints in `ControlRegistry` and pushes them over LoRa to the REMOTE node
as a DATA_ACK frame.

**Assumptions:**
- LOCAL node address: `<Local>` (substitute the actual hostname or IP).
- Target REMOTE node LoRa address: `0x02` (assumption). `addr=2` in the query.
  Use `addr=3` if the REMOTE is at address `0x03`, etc.

#### curl Command - REST POST

**Bash / sh:**

```bash
curl -s -X POST "http://<Local>/api/lora/nodes/control?addr=2" \
  -H "Content-Type: application/json" \
  -d '{"device":"Power-Supply",
       "metrics":{"Voltage":{"value":1200,"unit":"V","scale":100},
                  "Current":{"value":50,  "unit":"A","scale":10},
                  "Pwr":    {"value":"off"}}}'
```

**PowerShell note:**

```powershell
$body = '{"device":"Power-Supply","metrics":{"Voltage":{"value":1200,"unit":"V","scale":100},"Current":{"value":50,"unit":"A","scale":10},"Pwr":{"value":"off"}}}'
curl.exe -s -X POST "http://<Local>/api/lora/nodes/control?addr=2" `
  -H "Content-Type: application/json" `
  -d $body
```

#### Request Body Explained

- `Voltage`: output voltage 12.00 V - stored as `1200` (scale 100). In an MQTT payload this value is formatted as `12.00` (see [Architecture.md section 8.5](Architecture.md#85-topic-scheme-and-payloads)); setpoints themselves are not published (see [8.6](Architecture.md#86-control-via-the-set-topic)).
- `Current`: current limit 5.0 A - stored as `50` (scale 10), formatted as `5.0` in an MQTT payload.
- `Pwr`: power state - JSON string `"off"`. Text metric. No `unit` or `scale` needed (ignored
  for strings). To turn on, send `"value":"on"`.

**Note on the `Pwr` text metric:** there is no enum type and no compile-time list of allowed
values (see [value types in the field reference](#request-body-schema-and-field-limits)). The firmware stores whatever string
the caller sends up to 12 bytes. `"on"`, `"off"`, `"standby"`, etc. are all valid. A JSON
boolean `true` would be stored as the text `"true"`, not `"on"`. All names in this example
(`Power-Supply`, `Voltage`, `Current`, `Pwr`) pass the sanitizer unchanged.

#### Expected HTTP Response

**`200 OK`** (all metrics accepted and REMOTE acknowledged; the schema is unchanged, i.e. a repeat call with the same device and metric names):

```json
{
  "accepted": [
    {
      "metric": "Voltage",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Current",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "Pwr",
      "device_idx": 1,
      "metric_id": 3
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 4,
  "delivery": "ok"
}
```

The call that first creates the device or a metric changes the schema (1 device + 3 metrics =
`registry_rev` `4` on a fresh registry). That call is answered with `202` and
`"delivery":"after_describe"` with an otherwise identical body, because REMOTE has not yet
fetched the new schema. `delivery` can also be `"after_describe"` (`202`) when REMOTE answers
BAD_REQUEST (crawl scheduled), `"timeout"` (`504`) if REMOTE does not respond, or `"busy"`
(`503`) if the LoRa broker is occupied. See the status code table of
[`POST /api/lora/nodes/control`](#post-apiloranodescontrol) for the full list of status codes.

#### Notes

- `POST /api/lora/nodes/control` is a **LOCAL-only** endpoint. Sending it to a REMOTE node
  returns `404`.
- The `addr` query parameter is mandatory. Omitting it returns `400`.
- The same setpoints can be set from an MQTT client via the `/set` topic without calling the
  REST API; the matching `mosquitto_pub` commands are in
  [Architecture.md section 8.7, Example 2](Architecture.md#example-2---power-supply-control-set-values-with-mosquitto_pub).

---

### Example 3 - Sensor values: POST on REMOTE, read back on LOCAL

An IoT sensor connected to REMOTE's SoftAP (`192.168.4.1`) pushes its environment readings:

```bash
curl -s -X POST http://192.168.4.1/api/lora/sensor/data \
  -H "Content-Type: application/json" \
  -d '{"device":"Env-Sensor",
       "metrics":{"Temperature":{"value":215,
                                 "unit":"C",
                                 "scale":10},
                  "Humidity":{"value":483,
                              "unit":"%",
                              "scale":10},
                  "AirPressure":{"value":"incr."},
                  "Firmware":{"value":110,
                              "unit":"vers.",
                              "scale":100},
                  "Daylight":{"value":true}
                 }
      }'
```

**Response (first post - schema registered, `registry_rev` incremented):**
```json
{
  "accepted": [
    {
      "metric": "Temperature",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Humidity",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "AirPressure",
      "device_idx": 1,
      "metric_id": 3
    },
    {
      "metric": "Firmware",
      "device_idx": 1,
      "metric_id": 4
    },
    {
      "metric": "Daylight",
      "device_idx": 1,
      "metric_id": 5
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 6
}
```

The values are now in REMOTE's `SensorRegistry`. REMOTE immediately sends a PING carrying the new `registry_rev` to LOCAL (see [Architecture.md section 7.6](Architecture.md#76-visibility-delay-for-new-data)), LOCAL crawls the definitions, and REMOTE's `PushTask` will deliver the values over LoRa. A LAN client can then read them from LOCAL:

```bash
curl -s "http://<Local>/api/lora/nodes/sensor?addr=2&device=1"
```

**Response:**
```json
{
  "nodes": [
    {
      "addr": 2,
      "boot_id": 305419896,
      "registry_rev": 6,
      "online": true,
      "peer_last_seen_ms_ago": 1200,
      "devices": [
        {
          "device": 1,
          "name": "Env-Sensor",
          "has_data": true,
          "age_ms": 4500,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "Temperature",
              "unit": "C",
              "scale": 10,
              "value": 215
            },
            {
              "id": 2,
              "name": "Humidity",
              "unit": "%",
              "scale": 10,
              "value": 483
            },
            {
              "id": 3,
              "name": "AirPressure",
              "unit": "",
              "scale": 1,
              "value": "incr."
            },
            {
              "id": 4,
              "name": "Firmware",
              "unit": "vers.",
              "scale": 100,
              "value": 110
            },
            {
              "id": 5,
              "name": "Daylight",
              "unit": "",
              "scale": 1,
              "value": "true"
            }
          ]
        }
      ]
    }
  ]
}
```

`value / scale` gives the real number: `215 / 10 = 21.5 °C`, `483 / 10 = 48.3 %`, `110 / 100 = 1.10` (firmware version). `AirPressure` and `Daylight` are text metrics and are returned as text; the boolean `true` is stored and returned as the text `"true"`.

The response always lists `unit` and `scale` for every metric. For text metrics they carry the defaults (`""` and `1`); the request does not need to send them.

---

### Example 4 - Control setpoint: POST on LOCAL, read back on REMOTE

A home automation controller on the LAN sets the output voltage and current limit of a power supply attached to REMOTE and switches it off:

```bash
curl -s -X POST "http://<Local>/api/lora/nodes/control?addr=2" \
  -H "Content-Type: application/json" \
  -d '{"device":"Power-Supply",
       "metrics":{"Voltage":{"value":1200,
                             "unit":"V",
                             "scale":100},
                  "Current":{"value":50,
                             "unit":"A",
                             "scale":10},
                  "Pwr":{"value":"off"}
                 }
      }'
```

**Response (repeat call, schema unchanged, REMOTE acknowledged):**
```json
{
  "accepted": [
    {
      "metric": "Voltage",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Current",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "Pwr",
      "device_idx": 1,
      "metric_id": 3
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 4,
  "delivery": "ok"
}
```

LOCAL stores the setpoint and delivers it to REMOTE as a `DATA_ACK` frame. The call that first creates the device or metric is answered with `202` and `"delivery":"after_describe"` instead, see [Example 2](#example-2---power-supply-control-local-control-setpoint-post).

The IoT device on REMOTE's SoftAP retrieves the cached setpoint:

```bash
curl -s http://192.168.4.1/api/lora/nodes/control
```

**Response:**
```json
{
  "nodes": [
    {
      "addr": 1,
      "boot_id": 987654321,
      "registry_rev": 4,
      "online": true,
      "peer_last_seen_ms_ago": 800,
      "devices": [
        {
          "device": 1,
          "name": "Power-Supply",
          "has_data": true,
          "age_ms": 120,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "Voltage",
              "unit": "V",
              "scale": 100,
              "value": 1200
            },
            {
              "id": 2,
              "name": "Current",
              "unit": "A",
              "scale": 10,
              "value": 50
            },
            {
              "id": 3,
              "name": "Pwr",
              "unit": "",
              "scale": 1,
              "value": "off"
            }
          ]
        }
      ]
    }
  ]
}
```

`1200 / 100 = 12.00 V` and `50 / 10 = 5.0 A`; `Pwr` is a text metric and is returned as it was sent. Check `online` and `age_ms` before applying the values - if `online` is `false` or `age_ms` exceeds your staleness limit, fall back to a safe state (see [Freshness fields](#freshness-fields-age_ms-and-peer_last_seen_ms_ago)).

---

### Example 5 - Live sensor read (bypasses cache, queries REMOTE directly)

Force a fresh reading from REMOTE's device 1 without waiting for the next push:

```bash
curl -s "http://<Local>/api/lora/nodes/sensor?addr=2&device=1&live=true"
```

LOCAL sends a live-read request to REMOTE over LoRa ([`REQUEST(Read)`](LoRaProtocol.md) in the protocol) and waits up to 3 s for the reply. Returns `504` on timeout.
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
| GET | `/api/mqtt/config` | `RestService.cpp` | LOCAL | Get MQTT broker configuration (password masked) |
| PUT | `/api/mqtt/config` | `RestService.cpp` | LOCAL | Update MQTT broker configuration (partial update, applied live) |
| GET | `/api/mqtt/status` | `RestService.cpp` | LOCAL | Live MQTT connection state |
| WS | `/ws/data` | `WebSocketService.cpp` | Both | Live state push (1 s, server to client only) |

**Tier-specific endpoints:** they are registered only on the tier that supports them. On the other tier the path does not exist (default `404`). Endpoints marked `Both` whose behaviour differs per tier (`/api/lora/nodes/sensor`, `/api/lora/nodes/control`) validate parameters inside the handler and answer `400` for parameters that do not apply to the running tier.

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
{
  "level": "INFO",
  "log": [
    "[INFO ][Application.cpp:30] Starting LoRaIoT ..."
  ]
}
```

---

## PUT /api/log/level

Sets the active log level. The level is persisted in NVS and survives reboots.

**Request body:** `application/json`

```json
{
  "level": "DEBUG"
}
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
{
  "tier": "LOCAL",
  "configured": true
}
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
{
  "error": "Password must be empty (open) or at least 8 characters (max 64)"
}
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
{
  "status": "requested",
  "result": "see GET /api/log"
}
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
{
  "channel": 23
}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/lora/channel

Sets the active LoRa channel. The new channel is persisted in NVS and survives reboots. This endpoint is available on both LOCAL and REMOTE instances.

**Request body:** `application/json`

```json
{
  "channel": 24
}
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
{
  "airRate": "62.5k"
}
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
{
  "airRate": "9.6k"
}
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
{
  "address": 1
}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/lora/address

Sets the active LoRa address. The new address is persisted in NVS and survives reboots. The E220 module is programmed at boot, so the new address takes effect after the next reboot. This endpoint is only available on REMOTE instances. The route is not registered on LOCAL nodes, which answer `404`.

**Request body:** `application/json`

```json
{
  "address": 2
}
```

Allowed values: integer between 2 and 254 (inclusive). `1` is reserved for the LOCAL node.

| Status | Meaning |
|---|---|
| `200` | Address changed. Body: `{"address":2}` |
| `400` | Body is not valid JSON, `address` is missing, or out of range |

---

## GET /api/system/cpu

Returns the currently active CPU frequency of the ESP32 in MHz.

**Response:** `200 application/json`

```json
{
  "frequency": 240
}
```

| Status | Meaning |
|---|---|
| `200` | OK. |

---

## PUT /api/system/cpu

Dynamically scales the ESP32's CPU clock frequency to optimize power consumption. The chosen frequency is persisted in NVS and automatically applied on reboots.

**Request body:** `application/json`

```json
{
  "frequency": 80
}
```

Supported frequencies (with active WiFi radio):
- `80` - 80 MHz (Minimum required clock speed for the ESP32 WiFi radio PLL)
- `160` - 160 MHz
- `240` - 240 MHz (Default high-performance clock speed)

**Response:** `200 application/json`

```json
{
  "frequency": 80
}
```

| Status | Meaning |
|---|---|
| `200` | CPU frequency updated. |
| `400` | Body is not valid JSON, `frequency` is missing, or is not a supported value (80, 160, or 240). |

---

---

## GET /api/lora/nodes

Returns all remote node addresses that LOCAL has observed via BEACON. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

`online` is `false` when a keep-alive check failed (3 unanswered PINGs after 90 s of silence).
Offline nodes stay in the list with their cached definitions; the next frame from the node
sets `online` back to `true`, and no new Describe crawl happens if `boot_id` and
`registry_rev` are unchanged. A slot is only reused (oldest offline node first) when all
slots are taken and a new node appears.

**Response:** `200 application/json`

```json
{
  "nodes": [
    {
      "addr": 2,
      "boot_id": 305419896,
      "registry_rev": 3,
      "last_seen_ms": 12450,
      "online": true,
      "devices": 2,
      "crawl_pending": false
    }
  ]
}
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
{
  "nodes": [
    {
      "addr": 2,
      "boot_id": 305419896,
      "registry_rev": 6,
      "online": true,
      "peer_last_seen_ms_ago": 4120,
      "devices": [
        {
          "device": 1,
          "name": "Env-Sensor",
          "has_data": true,
          "age_ms": 820,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "Temperature",
              "unit": "C",
              "scale": 10,
              "value": 215
            },
            {
              "id": 2,
              "name": "Humidity",
              "unit": "%",
              "scale": 10,
              "value": 483
            },
            {
              "id": 3,
              "name": "AirPressure",
              "unit": "",
              "scale": 1,
              "value": "incr."
            },
            {
              "id": 4,
              "name": "Firmware",
              "unit": "vers.",
              "scale": 100,
              "value": 110
            },
            {
              "id": 5,
              "name": "Daylight",
              "unit": "",
              "scale": 1,
              "value": null
            }
          ]
        }
      ]
    }
  ]
}
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
{
  "nodes": [
    {
      "addr": 2,
      "boot_id": 305419896,
      "registry_rev": 4,
      "online": true,
      "peer_last_seen_ms_ago": 2100,
      "devices": [
        {
          "device": 1,
          "name": "Power-Supply",
          "has_data": true,
          "age_ms": 120,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "Voltage",
              "unit": "V",
              "scale": 100,
              "value": 1200
            },
            {
              "id": 2,
              "name": "Current",
              "unit": "A",
              "scale": 10,
              "value": 50
            },
            {
              "id": 3,
              "name": "Pwr",
              "unit": "",
              "scale": 1,
              "value": "off"
            }
          ]
        }
      ]
    }
  ]
}
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

LOCAL only: the route is not registered on REMOTE nodes, which answer `404`. Registers control devices and metrics in `ControlRegistry[addr]`, stores the setpoint values, and pushes the updated device data to the remote node via DATA_ACK with retry. The same logic is also reachable via the MQTT `/set` topic (`<prefix>/Local/Remote_Node_<n>/<device>/<metric>/set`) - see [Architecture.md section 8.6](Architecture.md#86-control-via-the-set-topic).

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Target remote node LoRa address (2-254) |

**Request body:** `application/json`

```json
{
  "device": "Power-Supply",
  "metrics": {
    "Voltage": {
      "value": 1200,
      "unit": "V",
      "scale": 100
    },
    "Current": {
      "value": 50,
      "unit": "A",
      "scale": 10
    },
    "Pwr": {
      "value": "off"
    }
  }
}
```

Field types, value types and the hard capacity limits are the same as for sensor data: see [Request body schema and field limits](#request-body-schema-and-field-limits) under `POST /api/lora/sensor/data`.

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
{
  "accepted": [
    {
      "metric": "Voltage",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Current",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "Pwr",
      "device_idx": 1,
      "metric_id": 3
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 4,
  "delivery": "ok"
}
```

`delivery` values: `"ok"` | `"after_describe"` | `"busy"` | `"timeout"`

**Response body** (`400`):

```json
{
  "accepted": [
    
  ],
  "rejected": [
    "BadMetric"
  ],
  "registry_rev": 4
}
```

LOCAL stores the setpoint value regardless of delivery outcome. The remote will receive the latest value once the crawl completes (Step 12).

---

## GET /api/lora/stats

Returns cumulative LoRa transport counters since boot. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

**Response:** `200 application/json`

```json
{
  "tx_frames": 42,
  "rx_frames": 38,
  "crc_errors": 0,
  "ack_loss": 0
}
```

---

## POST /api/lora/nodes/echo

Sends a REQUEST(Echo) to the remote node. REMOTE reverses the text and returns it, proving end-to-end frame processing. Blocks until RESPONSE or timeout. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

| Query parameter | Required | Description |
|---|---|---|
| `addr` | Yes | Node LoRa address (1-254) |

**Request body:** `application/json`

```json
{
  "text": "hello world"
}
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
LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

**Request body:** none

| Status | Meaning |
|---|---|
| `200` | Result available. |
| `404` | No echo has been performed since boot. Body: `{"error":"no echo result yet"}` |

**Response body** (`200`):

```json
{
  "sent": "hello world",
  "received": "dlrow olleh",
  "seqNo": 7,
  "roundTripMs": 87,
  "success": true
}
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
(the ACK frame itself carries no `boot_id`). LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

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

Sends a broadcast PING (addr 0xFF) and returns the first ACK received. Useful for discovering reachable nodes. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

| Status | Meaning |
|---|---|
| `200` | At least one ACK received. Body: `{"addr":2,"rtt_ms":91,"boot_id":305419896}` (`addr` is the node that answered; `boot_id` optional) |
| `503` | LoRa broker busy |
| `504` | No response |

---


## POST /api/lora/sensor/data

REMOTE only: the route is not registered on LOCAL nodes, which answer `404`. Accepts sensor data from a SoftAP-connected IoT device. Auto-assigns stable `device_idx` and `metric_id` for new keys. Increments `registry_rev` on schema changes and immediately sends a PING to LOCAL carrying the new revision, so LOCAL starts the Describe crawl before the DATA frame even arrives. Rejects metrics that would cause the encoded `loraiot_Device` proto to exceed 186 bytes.

**Request body:** `application/json`

```json
{
  "device": "Env-Sensor",
  "metrics": {
    "Temperature": {
      "value": 215,
      "unit": "C",
      "scale": 10
    },
    "Humidity": {
      "value": 483,
      "unit": "%",
      "scale": 10
    },
    "AirPressure": {
      "value": "incr."
    },
    "Firmware": {
      "value": 110,
      "unit": "vers.",
      "scale": 100
    },
    "Daylight": {
      "value": true
    }
  }
}
```

Field types, value types and the hard capacity limits are described in [Request body schema and field limits](#request-body-schema-and-field-limits) below.

| Status | Meaning |
|---|---|
| `200` | All metrics accepted |
| `400` | Invalid JSON, missing fields, or some metrics rejected |

**Response body** (both `200` and `400`):

```json
{
  "accepted": [
    {
      "metric": "Temperature",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Humidity",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "AirPressure",
      "device_idx": 1,
      "metric_id": 3
    },
    {
      "metric": "Firmware",
      "device_idx": 1,
      "metric_id": 4
    },
    {
      "metric": "Daylight",
      "device_idx": 1,
      "metric_id": 5
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 6
}
```

### Request body schema and field limits

Both `POST /api/lora/sensor/data` (REMOTE) and `POST /api/lora/nodes/control` (LOCAL) share
the same JSON body shape. The annotated schema below shows every field, its JSON type, and the
hard limits enforced by `DeviceTable.h`. These limits also apply to the MQTT `/set` payload
(which carries one metric object) and to the topic levels derived from `device` and metric key
names.

```
{
  "device":  <string, max 12 chars>,    // device name; also the MQTT topic level
                                        // max 4 distinct devices per REMOTE node (node-wide hard cap)
  "metrics": {
    "<metric-key>": {                   // metric name, max 12 chars; also the MQTT topic level
                                        // max 8 metrics per device (node-wide hard cap)
      "value":  <integer|string|bool>,  // see value types below
      "unit":   <string, max 5 chars>,  // optional, default ""; stored as metadata only
      "scale":  <integer >= 1>          // optional, default 1; 0 treated as 1
    }
  }
}
```

> **Hard caps (node-wide, not per-request):** a REMOTE node holds at most **4 devices** and
> each device holds at most **8 metrics**. These slots are allocated on first use and never
> freed until the node reboots. A POST that would exceed either cap is rejected with HTTP 400
> and the metric appears in the `rejected` array of the response. (On `POST /api/lora/nodes/control`
> HTTP 400 is returned only when no metric at all is accepted; otherwise the status follows the
> delivery result and the metric still appears in `rejected`.)

#### Field Reference

| Field | JSON type | Max length / range | Default | Notes |
|---|---|---|---|---|
| `device` | string | 12 chars | - | Required. Name stored in registry and used as MQTT topic level (sanitized). |
| metric key (object key inside `"metrics"`) | string | 12 chars | - | Required. Name stored in registry and used as MQTT topic level (sanitized). |
| `value` (numeric) | integer or float | int32 range (-2 147 483 648 to 2 147 483 647) | - | Stored as `int32`; floats are truncated. Pass the pre-scaled integer, e.g. `215` for 21.5 with scale 10. |
| `value` (text) | string | 12 bytes UTF-8 | - | Stored verbatim. Strings over 12 bytes are rejected (HTTP 400). Use for state labels such as `"on"`, `"off"`, `"auto"`. |
| `value` (boolean) | `true` / `false` | - | - | Stored as the text strings `"true"` / `"false"`. |
| `unit` | string | 5 chars | `""` | Optional. Stored as metadata. In an MQTT `/set` payload it is used only when the metric is first registered. Never published and never in the topic path. |
| `scale` | integer | >= 1; 0 treated as 1 | `1` | Optional. Divisor: real value = `value / scale`. MQTT payload is the formatted decimal, e.g. `"21.5"`. |

Further rules for `value`:

- **String:** opaque text, for example `"30.10.2026"`, `"10:45:00"`, `"enabled"`. JSON type is the discriminator (even if digits only, a JSON string is treated as text). `scale` is ignored for strings.
- A `value` that is `null`, an array or an object is rejected (listed in `rejected`, HTTP 400), like a string over 12 bytes.

#### Sanitization Rule for Topic Levels

`device` and metric key names are stored as-is in the registry. When building an MQTT topic
level the firmware replaces every character outside `[A-Z a-z 0-9 _ -]` with `_`. The `/set`
parser matches incoming topic levels back to registry names via their sanitized form, so
`Power_Supply` in a topic matches a registered device `"Power Supply"`. The topic scheme itself
is described in [Architecture.md section 8.5](Architecture.md#85-topic-scheme-and-payloads).

<details>
<summary>Full capacity limits with source constant names (click to expand)</summary>

| Resource | Limit | Source constant in `DeviceTable.h` |
|---|---|---|
| Devices per REMOTE node | 4 | `DEVICE_TABLE_MAX_DEVICES` |
| Metrics per device | 8 | `DEVICE_TABLE_MAX_METRICS` |
| Device / metric name length | 12 chars | `DEVICE_TABLE_NAME_LEN - 1` |
| Unit string length | 5 chars | `DEVICE_TABLE_UNIT_LEN - 1` |
| Text value length | 12 bytes UTF-8 | `DEVICE_TABLE_TEXT_MAX_LEN` |

These limits apply equally to sensor data (REMOTE) and control setpoints (LOCAL).

</details>

---

## WebSocket /ws/data

Pushes a JSON state snapshot to all connected clients once per second. It drives the browser
dashboard served at `GET /`. The endpoint is server-push only: messages sent by a client are
ignored. Nothing is sent while no client is connected.

`mqtt` is the MQTT connection state (`idle`, `disconnected`, `connected` or `error`, same values as
`state` in `GET /api/mqtt/status`). It is present on LOCAL only and absent on REMOTE. The dashboard
shows a globe icon (U+1F310) in front of the spider icon only when the key is present: full colour
when `connected`, grey when `disconnected` or `error`, faint when `idle`.

`nodes[]` has the same shape as `GET /api/lora/nodes/sensor` on LOCAL and
`GET /api/lora/nodes/control` on REMOTE (without `boot_id` and `registry_rev`), and is `[]` when
no peer is known.

`crawl_pending` is `true` while a Describe crawl is in progress for that peer (schema is being
fetched). The dashboard spider icon is lit while any node has `crawl_pending: true`.

```json
{
  "uptime_s": 312,
  "tier": "LOCAL",
  "lora": {
    "tx_frames": 20,
    "rx_frames": 23,
    "crc_errors": 0,
    "ack_loss": 0
  },
  "mqtt": "connected",
  "nodes": [
    {
      "addr": 3,
      "online": true,
      "peer_last_seen_ms_ago": 53000,
      "crawl_pending": false,
      "devices": [
        {
          "device": 1,
          "name": "Lenovo W510",
          "has_data": true,
          "age_ms": 247000,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "CPU Temp.",
              "unit": "C",
              "scale": 100,
              "value": 7146
            }
          ]
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

---

## GET /api/mqtt/config

Returns the current MQTT broker configuration. JSON keys match the NVS key names exactly. The password is never returned; `mqttPassSet` indicates whether one is stored. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

**Response:** `200 application/json`

```json
{
  "mqttBroker": "192.168.1.10",
  "mqttPort": 1883,
  "mqttUser": "mqttuser",
  "mqttPassSet": true,
  "mqttPrefix": "LoRaIoT",
  "mqttRetain": true,
  "mqttDiscovery": false,
  "mqttEnabled": true
}
```

---

## PUT /api/mqtt/config

Updates MQTT broker settings. Partial update: any omitted field keeps its current stored value. Persisted to NVS and applied live - the broker connection is re-established immediately without a reboot. JSON keys match the NVS key names. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

**Request body:** `application/json` (all fields optional)

```json
{
  "mqttBroker": "192.168.1.10",
  "mqttPort": 1883,
  "mqttUser": "mqttuser",
  "mqttPass": "change-me",
  "mqttPrefix": "LoRaIoT",
  "mqttRetain": true,
  "mqttDiscovery": false,
  "mqttEnabled": true
}
```

| Key | Type | Limits | Default | Meaning |
|---|---|---|---|---|
| `mqttBroker` | string | max 63 chars | `""` | Broker hostname or IPv4 address |
| `mqttPort` | integer | 1-65535 | `1883` | Broker TCP port |
| `mqttUser` | string | max 31 chars | `""` | Optional username |
| `mqttPass` | string | max 31 chars | `""` | Optional password. Omit to keep current; set to `""` to clear. Never returned. |
| `mqttPrefix` | string | 1-31 chars | `"LoRaIoT"` | Root topic prefix; no `+`, `#`, spaces, or leading/trailing `/` |
| `mqttRetain` | boolean | - | `true` | Publish telemetry and status topics with retain=true |
| `mqttDiscovery` | boolean | - | `false` | Home Assistant MQTT auto-discovery (Sub-Task 6, currently unused) |
| `mqttEnabled` | boolean | - | `false` | Master enable switch for the MQTT service |

| Status | Meaning |
|---|---|
| `200` | Settings accepted, persisted, and applied |
| `400` | Validation error (message in `error` field) |

**Response body (`200`):** same format as `GET /api/mqtt/config`.

---

## GET /api/mqtt/status

Returns the live MQTT connection state and diagnostics. Fields are conditional - only present when relevant. LOCAL only: the route is not registered on REMOTE nodes, which answer `404`.

| Field | Presence | Meaning |
|---|---|---|
| `enabled` | always | The configured `mqttEnabled` switch |
| `state` | always | Live connection: `idle`, `disconnected`, `connected`, `error` |
| `reason` | only when `state` is `idle` | `"disabled"` (switch off) or `"no broker"` (enabled but broker empty) |
| `broker`, `port` | only when a broker is configured | As configured |
| `lastError` | only when `state` is `error` | lwmqtt error code from the last failed connect (`client.lastError()`) |
| `returnCode` | only when `state` is `error` and non-zero | MQTT CONNACK return code from the broker |
| `retryInSec` | only when `state` is `error` or `disconnected` | Seconds until the next reconnect attempt |
| `since` | always | Uptime seconds (`millis()/1000`) when `state` last changed |

`state` values:
- `"idle"` - not active; see `reason` for why.
- `"disconnected"` - enabled and broker set, currently attempting to reconnect.
- `"connected"` - broker link is up, publishing telemetry.
- `"error"` - last connect attempt failed; retrying with exponential backoff (2 s to 64 s).

`lastError` is the raw `int` returned by `arduino-mqtt` / lwmqtt `client.lastError()`. Common values are library-specific (negative = lwmqtt internal errors, positive = CONNACK codes). Check the arduino-mqtt source under `.pio/libdeps/*/MQTT/` for the full `lwmqtt_err_t` enum.

**Examples:**

```json
{
  "enabled": true,
  "state": "connected",
  "broker": "192.168.1.10",
  "port": 1883,
  "since": 412
}
```

```json
{
  "enabled": false,
  "state": "idle",
  "reason": "disabled",
  "since": 35
}
```

```json
{
  "enabled": true,
  "state": "error",
  "broker": "192.168.1.10",
  "port": 1883,
  "lastError": -3,
  "retryInSec": 16,
  "since": 120
}
```

For the full MQTT topic scheme (telemetry, availability, control `/set`, LWT) see [Architecture.md sections 8.5 to 8.7](Architecture.md#85-topic-scheme-and-payloads).

---

## Worked Examples

This section shows end-to-end REST examples derived directly from the running firmware
code. Each example gives the curl command that an IoT device or home-automation script sends
and the expected HTTP response from the handler. All values travel as scaled integers; the
real value is `value / scale`. Examples 1 and 2 are the detailed walk-throughs; examples 3 to
5 add the read-back calls and a live read.

### Address placeholders

The commands below use placeholders instead of hard-coded IP addresses because every
installation is different:

<details>
<summary>Address placeholder reference (click to expand)</summary>

| Placeholder | What to substitute |
|---|---|
| `<Local>` | Hostname or IP address of the LOCAL node (e.g. `192.168.1.147`). |
| `<Remote-Node-x>` | Hostname or IP address of the target REMOTE node (x = 1, 2, or 3). Each REMOTE has its own address; its IP address is reported by `GET /api/wifi/mode` on the node itself (`staIp` / `apIp`), and its LoRa address (the `addr` value) is listed by `GET /api/lora/nodes` on LOCAL or returned by `GET /api/lora/address` on the REMOTE node itself. In AP / AP_STA mode the SoftAP address is `192.168.4.1` by default. |
| `<broker>` | Hostname or IP address of the MQTT broker (e.g. `192.168.1.117`). |

</details>

---

### Example 1 - Environmental Sensor (REMOTE, sensor data POST)

#### Scenario

An IoT sensor board running a small HTTP client joins the REMOTE node SoftAP and POSTs
temperature, humidity, air pressure trend, firmware version and daylight readings. LOCAL
receives the DATA_ACK frame over LoRa.

**Assumptions:**
- REMOTE node address: `<Remote-Node-1>` (substitute the actual hostname or IP).

#### curl Command

**Bash / sh:**

```bash
curl -s -X POST http://<Remote-Node-1>/api/lora/sensor/data \
  -H "Content-Type: application/json" \
  -d '{"device":"Env-Sensor",
       "metrics":{"Temperature":{"value":215,"unit":"C","scale":10},
                  "Humidity":   {"value":483,"unit":"%","scale":10},
                  "AirPressure":{"value":"incr."},
                  "Firmware":   {"value":110,"unit":"vers.","scale":100},
                  "Daylight":   {"value":true}}}'
```

**PowerShell note:** PowerShell's `curl` is an alias for `Invoke-WebRequest`. Use `curl.exe`
explicitly and escape the inner double quotes with a backslash, or wrap the JSON body in a
here-string. Recommended approach:

```powershell
$body = '{"device":"Env-Sensor","metrics":{"Temperature":{"value":215,"unit":"C","scale":10},"Humidity":{"value":483,"unit":"%","scale":10},"AirPressure":{"value":"incr."},"Firmware":{"value":110,"unit":"vers.","scale":100},"Daylight":{"value":true}}}'
curl.exe -s -X POST http://<Remote-Node-1>/api/lora/sensor/data `
  -H "Content-Type: application/json" `
  -d $body
```

#### Request Body Explained

The `value` field of a numeric metric is the **scaled integer** - the physical reading multiplied by `scale`:
- Temperature 21.5 C: `21.5 * 10 = 215` (integer).
- Humidity 48.3 %: `48.3 * 10 = 483` (integer).
- Firmware version 1.10: `1.10 * 100 = 110` (integer), with the unit `"vers."`.

The other two metrics are not numbers:
- `AirPressure`: the JSON string `"incr."` (rising trend; `"decr."` for falling). Text metric, no `unit` or `scale` needed (ignored for strings).
- `Daylight`: the JSON boolean `true`. It is stored as the text `"true"` (see [value types in the field reference](#request-body-schema-and-field-limits)).

The `unit` field (`"C"`, `"%"`, `"vers."`) is stored as metadata and forwarded verbatim. It does not
appear in the MQTT topic name; only the metric name (`"Temperature"`, `"Humidity"`, ...) is used
to build the topic level (see [Architecture.md section 8.5](Architecture.md#85-topic-scheme-and-payloads)).

#### Expected HTTP Response

**`200 OK`** (all metrics accepted):

```json
{
  "accepted": [
    {
      "metric": "Temperature",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Humidity",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "AirPressure",
      "device_idx": 1,
      "metric_id": 3
    },
    {
      "metric": "Firmware",
      "device_idx": 1,
      "metric_id": 4
    },
    {
      "metric": "Daylight",
      "device_idx": 1,
      "metric_id": 5
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 6
}
```

`registry_rev` increments only on schema changes (new device or new metric name), once per new
device and once per new metric name. The first POST above on a freshly booted node therefore
returns `6` (1 device + 5 metrics). A repeat POST with the same device/metric names returns the
same `registry_rev` value and updates only the stored value. See the status code table of
[`POST /api/lora/sensor/data`](#post-apilorasensordata) for the full list of status codes.

#### Notes

- `POST /api/lora/sensor/data` is a **REMOTE-only** endpoint. Sending it to the LOCAL node
  returns `404`.
- Numeric values are truncated to `int32` by `valVar.as<int32_t>()` in the request handler
  (`controlApplyMetric` for control setpoints). Do not pass floating-point JSON values expecting
  implicit scaling - pass the pre-scaled integer.
- LOCAL publishes the stored metrics to the MQTT broker; the matching topics and a
  `mosquitto_sub` session are in
  [Architecture.md section 8.7, Example 1](Architecture.md#example-1---environmental-sensor-subscribe-and-see-the-topics).

---

### Example 2 - Power Supply Control (LOCAL, control setpoint POST)

#### Scenario

A home-automation script or Node-RED flow sets the output voltage, current limit, and power
state of a DC power supply connected to a REMOTE node. The device is named `"Power-Supply"`.
LOCAL stores the setpoints in `ControlRegistry` and pushes them over LoRa to the REMOTE node
as a DATA_ACK frame.

**Assumptions:**
- LOCAL node address: `<Local>` (substitute the actual hostname or IP).
- Target REMOTE node LoRa address: `0x02` (assumption). `addr=2` in the query.
  Use `addr=3` if the REMOTE is at address `0x03`, etc.

#### curl Command - REST POST

**Bash / sh:**

```bash
curl -s -X POST "http://<Local>/api/lora/nodes/control?addr=2" \
  -H "Content-Type: application/json" \
  -d '{"device":"Power-Supply",
       "metrics":{"Voltage":{"value":1200,"unit":"V","scale":100},
                  "Current":{"value":50,  "unit":"A","scale":10},
                  "Pwr":    {"value":"off"}}}'
```

**PowerShell note:**

```powershell
$body = '{"device":"Power-Supply","metrics":{"Voltage":{"value":1200,"unit":"V","scale":100},"Current":{"value":50,"unit":"A","scale":10},"Pwr":{"value":"off"}}}'
curl.exe -s -X POST "http://<Local>/api/lora/nodes/control?addr=2" `
  -H "Content-Type: application/json" `
  -d $body
```

#### Request Body Explained

- `Voltage`: output voltage 12.00 V - stored as `1200` (scale 100). In an MQTT payload this value is formatted as `12.00` (see [Architecture.md section 8.5](Architecture.md#85-topic-scheme-and-payloads)); setpoints themselves are not published (see [8.6](Architecture.md#86-control-via-the-set-topic)).
- `Current`: current limit 5.0 A - stored as `50` (scale 10), formatted as `5.0` in an MQTT payload.
- `Pwr`: power state - JSON string `"off"`. Text metric. No `unit` or `scale` needed (ignored
  for strings). To turn on, send `"value":"on"`.

**Note on the `Pwr` text metric:** there is no enum type and no compile-time list of allowed
values (see [value types in the field reference](#request-body-schema-and-field-limits)). The firmware stores whatever string
the caller sends up to 12 bytes. `"on"`, `"off"`, `"standby"`, etc. are all valid. A JSON
boolean `true` would be stored as the text `"true"`, not `"on"`. All names in this example
(`Power-Supply`, `Voltage`, `Current`, `Pwr`) pass the sanitizer unchanged.

#### Expected HTTP Response

**`200 OK`** (all metrics accepted and REMOTE acknowledged; the schema is unchanged, i.e. a repeat call with the same device and metric names):

```json
{
  "accepted": [
    {
      "metric": "Voltage",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Current",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "Pwr",
      "device_idx": 1,
      "metric_id": 3
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 4,
  "delivery": "ok"
}
```

The call that first creates the device or a metric changes the schema (1 device + 3 metrics =
`registry_rev` `4` on a fresh registry). That call is answered with `202` and
`"delivery":"after_describe"` with an otherwise identical body, because REMOTE has not yet
fetched the new schema. `delivery` can also be `"after_describe"` (`202`) when REMOTE answers
BAD_REQUEST (crawl scheduled), `"timeout"` (`504`) if REMOTE does not respond, or `"busy"`
(`503`) if the LoRa broker is occupied. See the status code table of
[`POST /api/lora/nodes/control`](#post-apiloranodescontrol) for the full list of status codes.

#### Notes

- `POST /api/lora/nodes/control` is a **LOCAL-only** endpoint. Sending it to a REMOTE node
  returns `404`.
- The `addr` query parameter is mandatory. Omitting it returns `400`.
- The same setpoints can be set from an MQTT client via the `/set` topic without calling the
  REST API; the matching `mosquitto_pub` commands are in
  [Architecture.md section 8.7, Example 2](Architecture.md#example-2---power-supply-control-set-values-with-mosquitto_pub).

---

### Example 3 - Sensor values: POST on REMOTE, read back on LOCAL

An IoT sensor connected to REMOTE's SoftAP (`192.168.4.1`) pushes its environment readings:

```bash
curl -s -X POST http://192.168.4.1/api/lora/sensor/data \
  -H "Content-Type: application/json" \
  -d '{"device":"Env-Sensor",
       "metrics":{"Temperature":{"value":215,
                                 "unit":"C",
                                 "scale":10},
                  "Humidity":{"value":483,
                              "unit":"%",
                              "scale":10},
                  "AirPressure":{"value":"incr."},
                  "Firmware":{"value":110,
                              "unit":"vers.",
                              "scale":100},
                  "Daylight":{"value":true}
                 }
      }'
```

**Response (first post - schema registered, `registry_rev` incremented):**
```json
{
  "accepted": [
    {
      "metric": "Temperature",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Humidity",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "AirPressure",
      "device_idx": 1,
      "metric_id": 3
    },
    {
      "metric": "Firmware",
      "device_idx": 1,
      "metric_id": 4
    },
    {
      "metric": "Daylight",
      "device_idx": 1,
      "metric_id": 5
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 6
}
```

The values are now in REMOTE's `SensorRegistry`. REMOTE immediately sends a PING carrying the new `registry_rev` to LOCAL (see [Architecture.md section 7.6](Architecture.md#76-visibility-delay-for-new-data)), LOCAL crawls the definitions, and REMOTE's `PushTask` will deliver the values over LoRa. A LAN client can then read them from LOCAL:

```bash
curl -s "http://<Local>/api/lora/nodes/sensor?addr=2&device=1"
```

**Response:**
```json
{
  "nodes": [
    {
      "addr": 2,
      "boot_id": 305419896,
      "registry_rev": 6,
      "online": true,
      "peer_last_seen_ms_ago": 1200,
      "devices": [
        {
          "device": 1,
          "name": "Env-Sensor",
          "has_data": true,
          "age_ms": 4500,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "Temperature",
              "unit": "C",
              "scale": 10,
              "value": 215
            },
            {
              "id": 2,
              "name": "Humidity",
              "unit": "%",
              "scale": 10,
              "value": 483
            },
            {
              "id": 3,
              "name": "AirPressure",
              "unit": "",
              "scale": 1,
              "value": "incr."
            },
            {
              "id": 4,
              "name": "Firmware",
              "unit": "vers.",
              "scale": 100,
              "value": 110
            },
            {
              "id": 5,
              "name": "Daylight",
              "unit": "",
              "scale": 1,
              "value": "true"
            }
          ]
        }
      ]
    }
  ]
}
```

`value / scale` gives the real number: `215 / 10 = 21.5 °C`, `483 / 10 = 48.3 %`, `110 / 100 = 1.10` (firmware version). `AirPressure` and `Daylight` are text metrics and are returned as text; the boolean `true` is stored and returned as the text `"true"`.

The response always lists `unit` and `scale` for every metric. For text metrics they carry the defaults (`""` and `1`); the request does not need to send them.

---

### Example 4 - Control setpoint: POST on LOCAL, read back on REMOTE

A home automation controller on the LAN sets the output voltage and current limit of a power supply attached to REMOTE and switches it off:

```bash
curl -s -X POST "http://<Local>/api/lora/nodes/control?addr=2" \
  -H "Content-Type: application/json" \
  -d '{"device":"Power-Supply",
       "metrics":{"Voltage":{"value":1200,
                             "unit":"V",
                             "scale":100},
                  "Current":{"value":50,
                             "unit":"A",
                             "scale":10},
                  "Pwr":{"value":"off"}
                 }
      }'
```

**Response (repeat call, schema unchanged, REMOTE acknowledged):**
```json
{
  "accepted": [
    {
      "metric": "Voltage",
      "device_idx": 1,
      "metric_id": 1
    },
    {
      "metric": "Current",
      "device_idx": 1,
      "metric_id": 2
    },
    {
      "metric": "Pwr",
      "device_idx": 1,
      "metric_id": 3
    }
  ],
  "rejected": [
    
  ],
  "registry_rev": 4,
  "delivery": "ok"
}
```

LOCAL stores the setpoint and delivers it to REMOTE as a `DATA_ACK` frame. The call that first creates the device or metric is answered with `202` and `"delivery":"after_describe"` instead, see [Example 2](#example-2---power-supply-control-local-control-setpoint-post).

The IoT device on REMOTE's SoftAP retrieves the cached setpoint:

```bash
curl -s http://192.168.4.1/api/lora/nodes/control
```

**Response:**
```json
{
  "nodes": [
    {
      "addr": 1,
      "boot_id": 987654321,
      "registry_rev": 4,
      "online": true,
      "peer_last_seen_ms_ago": 800,
      "devices": [
        {
          "device": 1,
          "name": "Power-Supply",
          "has_data": true,
          "age_ms": 120,
          "alert": false,
          "metrics": [
            {
              "id": 1,
              "name": "Voltage",
              "unit": "V",
              "scale": 100,
              "value": 1200
            },
            {
              "id": 2,
              "name": "Current",
              "unit": "A",
              "scale": 10,
              "value": 50
            },
            {
              "id": 3,
              "name": "Pwr",
              "unit": "",
              "scale": 1,
              "value": "off"
            }
          ]
        }
      ]
    }
  ]
}
```

`1200 / 100 = 12.00 V` and `50 / 10 = 5.0 A`; `Pwr` is a text metric and is returned as it was sent. Check `online` and `age_ms` before applying the values - if `online` is `false` or `age_ms` exceeds your staleness limit, fall back to a safe state (see [Freshness fields](#freshness-fields-age_ms-and-peer_last_seen_ms_ago)).

---

### Example 5 - Live sensor read (bypasses cache, queries REMOTE directly)

Force a fresh reading from REMOTE's device 1 without waiting for the next push:

```bash
curl -s "http://<Local>/api/lora/nodes/sensor?addr=2&device=1&live=true"
```

LOCAL sends a live-read request to REMOTE over LoRa ([`REQUEST(Read)`](LoRaProtocol.md) in the protocol) and waits up to 3 s for the reply. Returns `504` on timeout.
