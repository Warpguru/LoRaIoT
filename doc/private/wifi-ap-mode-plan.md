# Plan: Remote Node WiFi Access Point (AP) and Concurrent Mode Support

Source of requirements and feasibility analysis: [`doc/APModeFeasibility.md`](doc/APModeFeasibility.md).

## Top-Level Overview

This plan defines the architecture, implementation steps, and end-to-end verification procedures to add Access Point (AP / SoftAP) mode and concurrent AP+STA mode to LoRaIoT remote nodes. This enables field-deployed remote nodes to broadcast their own WiFi network (`192.168.4.1`) for local IoT sensors and actuators while maintaining LoRa upstream communication to the local gateway.

The implementation introduces `WiFiConfigClass` under `src/service/src/`, adds `GET /api/wifi/mode` and `PUT /api/wifi/mode` REST endpoints (the WiFiManager captive portal is not extended; AP settings are REST-only), persists all configuration in NVS with descriptive keys, executes clean mode transitions via deferred reboot, and validates sensor ingestion and control retrieval over AP mode.

---

## Sub-Tasks

### Sub-Task 1 - WiFi Configuration and Service Model (`WiFiConfigClass`)

- **Intent**: Create the core configuration and NVS persistence singleton for WiFi operation modes (STA, AP, AP+STA), AP credentials, IP settings, and radio channel under `src/service/src/`.
- **Expected Outcomes**:
  - `src/service/src/WiFiConfig.h` and `src/service/src/WiFiConfig.cpp` are created.
  - Supports modes: `STA` (Station only), `AP` (Access Point only), and `AP_STA` (Concurrent AP + STA).
  - NVS keys in namespace `sc_prefs` (strictly adhering to ESP-IDF's 15-character key limit):
    - `remoteWifiMode` (14 chars, uint8_t: 0 = STA, 1 = AP, 2 = AP_STA)
    - `remoteApSsid` (12 chars, string: max 32 chars)
    - `remoteApPass` (12 chars, string: max 64 chars)
    - `remoteApIp` (10 chars, string/uint32_t)
    - `remoteApChannel` (15 chars, uint8_t: 1..13)
  - Default values: Mode `STA`, SSID `LoRaIoT-Node-{n}` (derived from the LoRa node address), Password `LoRaIoT`, IP `192.168.4.1`, Channel 1.
  - Validates WPA2 password constraints (empty or >= 8 chars) and channel range (1..13). Automatically assigns `/24` subnet mask (`255.255.255.0`) when custom IP is set.
- **Todo List**:
  1. Define `WifiOpMode` enum (`STA`, `AP`, `AP_STA`) and constants in `src/service/src/WiFiConfig.h`.
  2. Implement `WiFiConfigClass` with `begin()`, `save()`, getters, setters, and validation methods in `src/service/src/WiFiConfig.cpp`.
  3. Implement automatic default SSID derivation based on `LoRaConfig.getAddress()`.
- **Relevant Context**:
  - [`src/LoRaIoT/src/LoRaConfig.h`](src/LoRaIoT/src/LoRaConfig.h:1), [`src/LoRaIoT/src/Tier.h`](src/LoRaIoT/src/Tier.h:1) (NVS singleton patterns).
- **Status**: `[x] done`

---

### Sub-Task 2 - Server Startup Wiring and Mode-Aware Boot

- **Intent**: Update the WiFi initialization sequence in `Server.cpp` to respect the configured WiFi mode and clear WiFi NVS settings on `/reset`. Configuration is managed exclusively via the REST API (not the captive portal).
- **Expected Outcomes**:
  - `setupServer()` branches based on `WiFiConfig.getOpMode()`:
    - In `STA` mode: runs `wm.autoConnect("LoRaIoT")`.
    - In `AP` mode: configures `WiFi.softAPConfig()` with `/24` subnet, sets `WiFi.mode(WIFI_AP)`, and starts `WiFi.softAP()`.
    - In `AP_STA` mode: starts SoftAP and runs `wm.autoConnect("LoRaIoT")`.
  - Captive portal remains clean and uncluttered (AP settings configured exclusively via REST API).
  - `handleReset()` clears all `remote*` WiFi configuration keys from NVS.
- **Todo List**:
  1. Include `WiFiConfig.h` in `src/LoRaIoT/src/Server.cpp`.
  2. Add conditional boot logic in `setupServer()` for AP, STA, and AP_STA modes.
  3. Update `handleReset()` to clear WiFi NVS settings.
- **Relevant Context**:
  - [`src/LoRaIoT/src/Server.cpp`](src/LoRaIoT/src/Server.cpp:174) (`setupServer` parameter injection).
- **Status**: `[x] done`

---

### Sub-Task 3 - REST API Endpoints (`GET /api/wifi/mode` & `PUT /api/wifi/mode`)

- **Intent**: Provide RESTful endpoints to query current WiFi status and reconfigure WiFi mode, AP credentials, and concurrent STA behavior with auto-reboot.
- **Expected Outcomes**:
  - `GET /api/wifi/mode` returns active mode, active IP(s), AP SSID, AP channel, and connected AP client count.
  - `PUT /api/wifi/mode` accepts JSON `{ "mode": "AP", "ssid": "...", "password": "...", "ip": "192.168.4.1", "channel": 1 }`.
  - Input validation: returns HTTP 400 for invalid mode, invalid channel, or password between 1 and 7 characters.
  - On valid PUT: persists to NVS, returns HTTP 200 with updated config, and triggers a clean restart after 200 ms (`ESP.restart()`).
- **Todo List**:
  1. Add route definitions in `RestService::registerRoutes()` in `src/service/src/RestService.cpp`.
  2. Implement request JSON parsing, validation, NVS persistence, and deferred reboot.
- **Relevant Context**:
  - [`src/service/src/RestService.cpp`](src/service/src/RestService.cpp:145) (PUT handler structure with JSON and validation).
  - [`src/LoRaIoT/src/Server.cpp`](src/LoRaIoT/src/Server.cpp:81) (`handleReboot` delayed restart pattern).
- **Status**: `[x] done`

---

### Sub-Task 4 - Documentation and Web UI Updates

- **Intent**: Maintain complete synchronization of OpenAPI documentation, human-readable REST docs, and the web UI.
- **Expected Outcomes**:
  - Authoritative `OPENAPI_JSON` in `src/LoRaIoT/src/openapi_json.h` contains `GET /api/wifi/mode` and `PUT /api/wifi/mode` schemas.
  - `doc/RestApi.md` documents both endpoints with example requests and responses.
  - `src/LoRaIoT/src/index_html.h` displays the active WiFi mode and link to `/api/wifi/mode`.
- **Todo List**:
  1. Add schema and paths to `src/LoRaIoT/src/openapi_json.h`.
  2. Add endpoint documentation to `doc/RestApi.md`.
  3. Add WiFi status row to `src/LoRaIoT/src/index_html.h`.
- **Relevant Context**:
  - [`src/LoRaIoT/src/openapi_json.h`](src/LoRaIoT/src/openapi_json.h:1), [`doc/RestApi.md`](doc/RestApi.md:1), [`src/LoRaIoT/src/index_html.h`](src/LoRaIoT/src/index_html.h:1).
- **Status**: `[x] done`

---

### Sub-Task 5 - End-to-End Verification and Live Validation

- **Intent**: Verify that the remote node operates as an AP, accepts sensor data from an AP client, forwards it over LoRa to LOCAL, receives control setpoints from LOCAL, and serves control data to AP clients.
- **Expected Outcomes**:
  - REMOTE is switched to AP-only mode via `PUT /api/wifi/mode {"mode":"AP"}` and auto-reboots into AP mode.
  - Sensor ingestion test: User connects client device to REMOTE's AP (`LoRaIoT-Node-1` at `192.168.4.1`) and runs provided `curl` command to post sensor readings.
  - LOCAL receives and caches the sensor readings via LoRa, verified at `GET http://192.168.1.147/api/lora/nodes/sensor?addr=2`.
  - Control push test: Control setpoint posted to LOCAL (`POST http://192.168.1.147/api/lora/nodes/control?addr=2`), pushed to REMOTE over LoRa.
  - Control retrieval test: User executes provided `curl` on REMOTE's AP (`GET http://192.168.4.1/api/lora/nodes/control`) and confirms the setpoint arrived.
  - Health and memory checks pass via `loraiot-ping` and `loraiot-resources`.
- **Todo List**:
  1. Test `GET /api/wifi/mode` on bench STA network.
  2. Reconfigure REMOTE to AP mode and observe boot logs.
  3. Provide sensor `curl` command for AP client execution and verify on LOCAL.
  4. Post control setpoint on LOCAL and provide control retrieval `curl` command for AP client verification.
  5. Run diagnostics regression checks.
- **Relevant Context**:
  - [`.bob/skills/loraiot-sensor/SKILL.md`](.bob/skills/loraiot-sensor/SKILL.md:1)
  - [`.bob/skills/loraiot-control/SKILL.md`](.bob/skills/loraiot-control/SKILL.md:1)
- **Status**: `[ ] pending`
