# AGENTS.md

This file provides guidance to agents when working with code in this repository.

## Writing Style Rule

**Never use `—` (em dash) or `–` (en dash). Use `-` (hyphen-minus) instead.**

## Project State

This is an ESP32 embedded firmware project. The Maven/Eclipse Java project shell (`pom.xml`, `.classpath`, `src/main/java/`) is a scaffolding artifact - there is **no Java source code**. All firmware lives under `ESP/`.

Two firmware projects exist under `ESP/`:
- `ESP/SerialController/` - **reference implementation** (complete, working). Reuse its patterns.
- `ESP/LoRaIoT/` - **this project** (scaffold in place, LoRa logic to be implemented).

## Stack

- **Target hardware:** ESP32 + E220-400T22D LoRa module (Semtech LLCC68, 433 MHz)
- **Firmware framework:** Arduino (C++), FreeRTOS tasks on ESP32
- **Build toolchain:** PlatformIO or Arduino IDE (both supported - identical source, different entry-point stubs)

## Libraries (declared in `ESP/LoRaIoT/platformio.ini`)

| Library | Author | Version | Purpose |
|---|---|---|---|
| WiFiManager | tzapu | >= 2.0.17 | First-time WiFi captive-portal setup |
| ArduinoJson | Benoit Blanchon | >= 7.4.3 | JSON (`JsonDocument`, `serializeJson`) |
| ESPAsyncWebServer | ESP32Async | >= 3.12.1 | Non-blocking HTTP + WebSocket server |
| AsyncTCP | ESP32Async | >= 3.5.0 | Required dependency of ESPAsyncWebServer |

PlatformIO: `lib_deps` in `platformio.ini` - downloaded automatically.
Arduino IDE: Sketch -> Include Library -> Manage Libraries, install all four.

## Source Layout (`ESP/LoRaIoT/`)

```
LoRaIoT.ino                    Arduino IDE entry point (stub - delegates to Application)
LoRaIoT.cpp                    PlatformIO entry point (#ifndef ARDUINO guard)
platformio.ini
src/
  LoRaIoT/src/
    Application.h/.cpp         applicationSetup() / applicationLoop() - wiring
    Server.h/.cpp              WiFiManager, AsyncWebServer, route registration
    LogBuffer.h/.cpp           Levelled ring-buffer logger (copied verbatim from SerialController)
    StatusLed.h/.cpp           LED state machine BOOTING/NO_DEVICE/READY/FAULT (copied verbatim)
    ESPInfo.h                  GET /status hardware diagnostics helper (copied verbatim)
    index_html.h               PROGMEM macro for browser UI HTML (placeholder - replace with real UI)
  service/src/
    RestService.h/.cpp         /api/* HTTP handlers - add LoRaIoT endpoints here
    WebSocketService.h/.cpp    1 s broadcast task + command queue
```

`LogBuffer`, `StatusLed`, and `ESPInfo.h` are **identical copies** from `ESP/SerialController/` - update both if the shared components need changes.

## Threading Model (must be followed exactly)

```
Core 0                                   Core 1
─────────────────────────────────────    ─────────────────────────────────────
WiFi / lwIP stack (system)               Arduino main task: setup() -> loop()
ESPAsyncWebServer request callbacks        applicationSetup() wiring
loraTask (priority 2)                      applicationLoop() - WS command drain
  sole owner of Serial2 (UART2)          DeviceService pollingTask (priority 2)
  E220 UART RX/TX/begin/end              WebSocketService broadcastTask (priority 1)
StatusLed ledTask (priority 1, tskNO_AFFINITY)
```

- **ESPAsyncWebServer callbacks run on Core 0** alongside the WiFi stack - never do Serial2/LoRa I/O directly inside an HTTP or WebSocket handler.
- **Serial2 (UART2) must have a single owner task** - use a FreeRTOS request/response queue (same pattern as `ModbusTransport` in `ESP/SerialController/src/modbus/`) so HTTP/WS handlers post requests and block for results.
- **Shared state must be protected by a FreeRTOS mutex** - `volatile` alone is not sufficient on dual-core ESP32. See `ESP/README.md` §"Java volatile vs FreeRTOS Mutex".

## WebServer / REST API Conventions

- **Server:** `ESPAsyncWebServer server(80)` - fully non-blocking; `handleServerRequests()` is an intentional no-op.
- **Browser UI:** serve from PROGMEM via `request->send_P(200, "text/html", INDEX_HTML)` - never load HTML into DRAM.
- **PUT handler with body:** signature is `(AsyncWebServerRequest*, uint8_t* data, size_t len, size_t, size_t)`.
- **JSON:** `JsonDocument doc; serializeJson(doc, json); request->send(200, "application/json", json)`.
- **Route registration:** add all `/api/*` endpoints in `RestService::registerRoutes()`, not scattered in `setupServer()`.
- **WiFi failure:** `statusLed.setState(LedState::FAULT)` then `vTaskSuspend(NULL)` - ledTask drives SOS forever.
- **Diagnostics endpoints** (already wired in `Server.cpp`): `GET /api/log`, `PUT /api/log/level`, `GET /status`, `GET /reset`.
- **Log level** is persisted in NVS (`sc_prefs` / `logLevel`) and survives reboots. Set via captive-portal form or `PUT /api/log/level`.

## Logging

Use the `Log_*` macros - never `Serial.println()` directly in application code:

```cpp
Log_error("fmt", ...);   // level ERROR
Log_warn ("fmt", ...);   // level WARN
Log_info ("fmt", ...);   // level INFO  (default)
Log_debug("fmt", ...);   // level DEBUG
Log_trace("fmt", ...);   // level TRACE
```

Output format: `[INFO ][Application.cpp:52] message text`
Remote retrieval: `GET /api/log` (last 128 lines as JSON).

## LoRa / E220 Architecture (from `doc/Feasibility.md`)

Two ESP32 gateway nodes bridge a remote WiFi SoftAP to a local network via a 433 MHz LoRa P2P link:

- **Remote node:** WiFi SoftAP (192.168.4.1) -> payload compressor -> UART -> E220 LoRa TX
- **Local node:** E220 LoRa RX -> UART -> frame validator -> WiFi STA -> MQTT/REST upstream

Lightweight TCP/IP (DNS, NTP, MQTT, small REST with MTU=190) is viable over the LoRa link. General-purpose IP routing is not. See `doc/Feasibility.md` Appendix A.

## E220 Hardware Wiring (canonical pin mapping)

| E220 Pin | ESP32 GPIO | Note |
|---|---|---|
| M0 (1) | GPIO 4 | Output - mode control |
| M1 (2) | GPIO 2 | Output - mode control |
| RXD (3) | GPIO 17 (TX2) | UART2 TX from ESP32 |
| TXD (4) | GPIO 16 (RX2) | UART2 RX to ESP32 |
| AUX (5) | GPIO 15 | Input - HIGH = module ready (see AUX semantics below) |
| VCC (6) | 3.3V or 5V | |
| GND (7) | GND | |

## Critical E220 Firmware Rules

- **AUX=HIGH does not mean RF transmission is complete** - the last sub-packet may still be in the air. Correct pattern: poll AUX HIGH -> write frame atomically -> wait AUX LOW -> wait AUX HIGH -> add 50ms guard delay.
- **Mode switching:** Set M0/M1, then wait for AUX rising edge >= 2 ms before sending any UART data.
- **Config mode (Mode 3: M1=1, M0=1)** uses fixed **9600 8N1** - switch UART to 9600 before entering, back to 115200 after returning to Mode 0.
- **Write each frame atomically** - never byte-by-byte with delays; the E220 force-flushes after 2 s of UART inactivity.
- **Wrong-format config response is `FF FF FF`** - always check and retry.

## Frame Protocol (spec from `doc/Feasibility.md` §6)

```
[0xAA 0x55][Type 1B][SeqNo 2B][PayloadLen 2B][Payload 1-190B][CRC16 2B]
```
Max frame = 200 bytes. Type values: `0x01` sensor, `0x02` HTTP GET proxy, `0x03` HTTP response, `0x04` heartbeat/ACK.

## Recommended E220 Configuration (max performance)

```cpp
// C0 + StartAddr(00) + Length(08) + ADDH + ADDL + REG0 + REG1 + REG2 + REG3 + CRYPT_H + CRYPT_L
// REG0=0xEF: UART=115200, AirRate=62.5kbps
// REG1=0x00: SubPacket=200B, Power=22dBm
// REG2=0x17: Channel 23 -> 433.125 MHz
// REG3=0x00: Transparent mode, RSSI byte off, LBT off
uint8_t configCmd[] = {0xC0, 0x00, 0x08, 0x00, 0x01, 0xEF, 0x00, 0x17, 0x00, 0x00, 0x00};
```

## Build / Flash Commands

Arduino IDE: Sketch -> Export Compiled Binary -> flash with esptool:
```
esptool.exe --chip esp32 --port COM3 --baud 921600 write-flash 0x0 LoRaIoT.ino.merged.bin
```
PlatformIO (from `ESP/LoRaIoT/`): `pio run` / `pio run --target upload` / `pio device monitor`
