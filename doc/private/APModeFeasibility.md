# Feasibility and Architecture Analysis: Remote Node WiFi Access Point (AP) Mode

## 1. Executive Summary

This document analyzes the feasibility, architecture, change surface, and risks of introducing configurable Access Point (AP / SoftAP) mode to remote LoRaIoT nodes alongside the existing Station (STA) client mode.

The proposed feature is **fully feasible, technically sound, and aligns directly with the original LoRaIoT architecture specification** (as documented in [`doc/LoRaProtocol.md`](doc/LoRaProtocol.md) and [`doc/Architecture.md`](doc/Architecture.md)), which envisioned remote nodes operating as autonomous field SoftAPs (`192.168.4.1`) for battery-powered IoT sensors and actuators.

The execution plan derived from this analysis is documented in [`wifi-ap-mode-plan.md`](wifi-ap-mode-plan.md).

---

## 2. Technical Feasibility Analysis

### 2.1 ESP32 Arduino WiFi Library Capabilities
The ESP32 Arduino WiFi stack (built on ESP-IDF and lwIP) natively supports:
- `WiFi.mode(WIFI_STA)` - Station mode (client connected to existing router).
- `WiFi.mode(WIFI_AP)` - Soft Access Point mode (node hosts its own SSID and DHCP server).
- `WiFi.mode(WIFI_AP_STA)` - Simultaneous concurrent AP and STA mode.
- `WiFi.softAP(ssid, password, channel, hidden, max_connection)` - Starts the SoftAP beacon.
- `WiFi.softAPConfig(local_ip, gateway, subnet)` - Sets custom AP IP subnet (defaults to `192.168.4.1 / 24`).
- `WiFi.softAPdisconnect(wifioff)` / `WiFi.disconnect(wifioff)` - Gracefully stops AP or STA radio interfaces.

### 2.2 Runtime Mode Switching Mechanics
Switching between STA, AP, and AP_STA mode at runtime without rebooting is supported by the ESP-IDF WiFi driver, but an automatic deferred reboot is recommended for robust socket and DHCP cleanup:
1. Reconfiguration request received at `PUT /api/wifi/mode`.
2. Input parameters validated and persisted to NVS.
3. Server returns HTTP 200 JSON response to the client.
4. After 200 ms delay, the node issues `ESP.restart()`, cleanly booting directly into the target mode.

### 2.3 ESPAsyncWebServer and Sockets Resilience
- `ESPAsyncWebServer` binds to `0.0.0.0` (all interfaces) on port 80.
- When booting in AP mode, `AsyncWebServer` is accessible on `192.168.4.1:80`.
- In `AP_STA` mode, it is accessible from both `192.168.1.174` (router LAN) and `192.168.4.1` (SoftAP WLAN).

---

## 3. Architectural Soundness

### 3.1 Alignment with LoRaIoT Architecture
In the LoRaIoT system model:
- **LOCAL Node** (`0x01`): Connected to home router (STA mode). Serves local dashboards, bridges sensor data into LAN, and accepts setpoint commands from home automation.
- **REMOTE Node** (`0x02`..`0xFE`): Deployed in the field, greenhouse, or outbuilding beyond home WiFi range. It acts as an autonomous WiFi base station (SoftAP). Local IoT sensors join its WiFi to POST readings to `POST /api/lora/sensor/data`, which REMOTE buffers and relays over 433 MHz LoRa to LOCAL.

During initial development, both nodes operated in STA mode connected to the development router for easy bench testing. Adding runtime and persistent AP / AP+STA mode completes the architectural intent while preserving development convenience.

### 3.2 RF Coexistence
- **LoRa Frequency**: 410.125 - 441.125 MHz (E220-400T22D via UART2).
- **WiFi Frequency**: 2.412 - 2.484 GHz.
- There is zero RF harmonic or band collision between the 433 MHz LoRa radio and 2.4 GHz WiFi radio.

---

## 4. Change Surface and Implementation Scope

### 4.1 New / Modified Files
1. **`src/service/src/WiFiConfig.h` & `WiFiConfig.cpp` (New)**:
   - Encapsulates WiFi mode (`STA`, `AP`, `AP_STA`), AP SSID, AP password, AP IP, and AP channel.
   - Managed as an active networking service component under `src/service/src/`.
   - Manages NVS persistence under namespace `sc_prefs` (consistent with `LoRaConfig`, `Gateway`, `LogBuffer`).
   - Descriptive NVS Keys (strictly adhering to ESP-IDF's 15-character key limit):
     - `remoteWifiMode` (14 chars: 0 = STA, 1 = AP, 2 = AP_STA)
     - `remoteApSsid` (12 chars)
     - `remoteApPass` (12 chars)
     - `remoteApIp` (10 chars)
     - `remoteApChannel` (15 chars)
   - Default values:
     - `mode`: `STA` (client mode, preserving backward compatibility). `AP_STA` (concurrent) is selected through `mode`, there is no separate flag.
     - `apSsid`: `LoRaIoT-Node-{n}` (where `{n}` is derived from the LoRa address, e.g. `LoRaIoT-Node-1` for address `0x02`).
     - `apPassword`: `LoRaIoT` (min 8 chars for WPA2, or empty for open).
     - `apIp`: `192.168.4.1` (automatically derives `/24` subnet mask `255.255.255.0`).
     - `apChannel`: `1` (recommended non-overlapping 2.4 GHz channel).
   - Validates parameters before saving.

2. **`src/LoRaIoT/src/Server.h` & `Server.cpp` (Modified)**:
   - Updates `setupServer()`:
     - If `WiFiConfig.getOpMode() == WifiOpMode::AP`: Initializes SoftAP directly with persisted settings without calling `wm.autoConnect()`.
     - If `WiFiConfig.getOpMode() == WifiOpMode::STA`: Calls `wm.autoConnect("LoRaIoT")`.
     - If `WiFiConfig.getOpMode() == WifiOpMode::AP_STA`: Initializes SoftAP and calls `wm.autoConnect("LoRaIoT")`; if the router connection fails the node continues as SoftAP only.
     - The WiFiManager captive portal is not extended: WiFi mode and SoftAP settings are configured exclusively through `PUT /api/wifi/mode`.
   - Updates `handleReset()` to clear all `remote*` WiFi configuration keys from NVS.

3. **`src/service/src/RestService.h` & `RestService.cpp` (Modified)**:
   - Registers new endpoints:
     - `GET /api/wifi/mode`: Returns current mode (`STA`, `AP`, `AP_STA`), active IP(s), AP SSID, AP channel, and connected AP client count.
     - `PUT /api/wifi/mode`: Accepts JSON payload `{"mode":"AP", "ssid":"...", "password":"...", "ip":"...", "channel":1}`. Validates mandatory `mode` (`"STA"`, `"AP"`, `"AP_STA"`), optional fields, persists to NVS, and triggers deferred reboot.

4. **`src/LoRaIoT/src/openapi_json.h` & `doc/RestApi.md` (Modified)**:
   - Documents `GET /api/wifi/mode` and `PUT /api/wifi/mode` in OpenAPI 3.0 specification and Markdown companion.

5. **`src/LoRaIoT/src/index_html.h` (Modified)**:
   - Adds WiFi Mode display and link to `/api/wifi/mode` in the web status UI.

---

## 5. Risks, Limitations, and Mitigation Strategies

| Risk / Limitation | Impact | Mitigation Strategy |
| :--- | :--- | :--- |
| **Client Disconnection on Mode Switch** | Caller loses HTTP connection immediately when radio switches | Return HTTP 200 JSON first, then execute deferred reboot after 200 ms. |
| **WPA2 Password Length Requirement** | ESP32 WPA2 requires >= 8 chars or empty (open). Passwords with 1-7 chars fail silently. | Enforce strict validation: return HTTP 400 if `password.length() > 0 && password.length() < 8`. |
| **IP Reachability Shift** | Switching from STA (`192.168.1.174`) to AP (`192.168.4.1`) makes the node unreachable from the LAN | Document clearly that clients must connect to the node's SSID (`LoRaIoT-Node-1`) once in AP mode. |
| **Captive Portal Stall in AP Mode** | If `wm.autoConnect()` was called in AP mode, boot would block waiting for a router | Bypass `wm.autoConnect()` when configured in AP-only mode; start `WiFi.softAP()` immediately. |
| **Channel Selection Conflicts** | User selects invalid WiFi channel (e.g. > 13 or < 1) | Restrict channel range to 1..13 with default = 1. |

---

## 6. Concrete Recommendation

1. **Adopt a dedicated `WiFiConfigClass` singleton** under `src/service/src/`.
2. **Implement `GET /api/wifi/mode` and `PUT /api/wifi/mode`** with deferred reboot.
3. **Use speaking, descriptive NVS keys**: `remoteWifiMode`, `remoteApSsid`, `remoteApPass`, `remoteApIp`, `remoteApChannel`.
4. **Support concurrent `AP_STA` mode** via the `mode` parameter for seamless developer testing.
5. **Execute complete end-to-end testing** with sensor ingestion and control retrieval over AP mode.
