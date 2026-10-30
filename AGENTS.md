# AGENTS.md

This file provides guidance to agents when working with code in this repository.

## Writing Style Rule

**Never use `—` (em dash) or `–` (en dash). Use `-` (hyphen-minus) instead.**

## Project State

This is an ESP32 embedded firmware project (Arduino C++, FreeRTOS). The Maven/Eclipse Java project shell (`pom.xml`, `.classpath`, `src/main/java/`) is a scaffolding artifact - there is **no Java source code**.

- **`ESP/`** is a **symlink to a read-only reference project** - never write to anything under `ESP/`. Use it only to copy proven patterns (FreeRTOS task wiring, logging, NVS persistence).
- All firmware for this project lives at the repository root (`LoRaIoT.ino`, `LoRaIoT.cpp`, `platformio.ini`, `src/`).
- Project overview, source layout, libraries, and wiring are documented in `README.md`, `platformio.ini`, and `doc/Feasibility.md` - do not duplicate them here.
- Dependency direction (one-way only): `service` and `LoRaIoT` → `registry` → `lora` → `util`; `service` and `LoRaIoT` → `diagnostics` → `util`. The `lora` library must never include upward into `LoRaIoT` or `registry`.
- `LogBuffer`, `StatusLed`, and `ESPInfo.h` originate from `ESP/SerialController/` - update both if the shared components need changes.

## Build and Tooling Constraints

- **Never build automatically.** Neither the Arduino IDE build nor the PlatformIO build can be triggered automatically - the developer runs both manually. The agent must never run `pio run`, `arduino-cli`, or any other build/flash command, and must never add build commands to a hook or any automated step.
- **Python scripts:** the Python environment is not on the global PATH. Always source the environment setup first:
  ```
  cmd /c D:\Development\SetupEnvPython3.cmd && python script.py
  ```
- **GIT:** the Git environment is not on the global PATH. Always source the environment setup first:
  ```
  cmd /c D:\Development\SetupEnvGit.cmd && git <command>
  ```

## Single-core / Dual-core Portability

**`TASK_CREATE` macro** (in `src/util/src/TaskUtils.h`) - use this instead of `xTaskCreate` or `xTaskCreatePinnedToCore` everywhere:

```cpp
TASK_CREATE(taskFunc, "TaskName", stackSize, param, priority, &handle);
```

- When `LORAIOT_DUAL_CORE` is defined **and** `CONFIG_FREERTOS_UNICORE` is **not** defined: pins the task to Core 1.
- Otherwise (all single-core targets, or dual-core without the flag): calls `xTaskCreate` with no affinity.
- `CONFIG_FREERTOS_UNICORE` is set automatically by ESP-IDF for **every** single-core chip (C3, C6, S2, H2) - no per-board `#ifdef` needed.
- Calling `xTaskCreatePinnedToCore(..., 1)` on a single-core build causes an **immediate kernel panic** - always use `TASK_CREATE`.

`LORAIOT_DUAL_CORE` is the single toggle:
- Arduino IDE: uncomment `#define LORAIOT_DUAL_CORE` in `LoRaIoT.ino` / `LoRaIoT.cpp`.
- PlatformIO: set automatically via `build_flags = -D LORAIOT_DUAL_CORE` on `[env:esp32dev]` and `[env:esp32s3]`. Single-core envs (`esp32c3`, `esp32c6`, `esp32s2`, `esp32h2`) deliberately omit this flag.

## Task file conventions

There are two valid patterns for FreeRTOS tasks in this codebase:

- **Standalone `*Task.cpp/.h`** - use when the task is an autonomous named actor in the protocol state machine with no owning class (e.g. `BeaconTask`, `PingTask`). The file exposes a `taskBegin()` function and any notification helpers; the task function itself is `static`.
- **Private `static` member of a class** - use when the task is the internal engine of a class that has its own broader public API (e.g. `loraTransportTask` inside `LoRaTransportClass`, `ledTask` inside `StatusLed`, `broadcastTask` inside `WebSocketService`). The task is an encapsulated implementation detail; callers never interact with it directly.

Do **not** extract a class-owned task into its own `*Task.cpp` - that breaks encapsulation and fragments a cohesive class. Do **not** inline a protocol-level standalone task into an unrelated class.

After every `TASK_CREATE`, immediately call `TaskRegistry.add(name, handle, stackSize)` so `GET /status` reports the stack high-water mark for that task.

## Threading Model (must be followed exactly)

```
Core 0  (dual-core only)                 Core 1  (or the only core on single-core builds)
─────────────────────────────────────    ─────────────────────────────────────
WiFi / lwIP stack (system)               Arduino main task: setup() -> loop()
ESPAsyncWebServer request callbacks        applicationSetup() wiring
loraTask (priority 2)                      applicationLoop()
  sole owner of Serial2 (UART2)          DeviceService pollingTask (priority 2)
  E220 UART RX/TX/begin/end              WebSocketService broadcastTask (priority 1)
StatusLed ledTask (priority 1, tskNO_AFFINITY)
```

- **ESPAsyncWebServer callbacks run on Core 0** (dual-core) alongside the WiFi stack - never do Serial2/LoRa I/O directly inside an HTTP or WebSocket handler.
- **Serial2 (UART2) must have a single owner task** - use a FreeRTOS request/response queue so HTTP/WS handlers post requests and block for results. See `ESP/SerialController/` for the queue/task wiring pattern (ignore protocol details - only the concurrency structure applies).
- **Shared state must be protected by a FreeRTOS mutex** - `volatile` alone is not sufficient on dual-core ESP32. See `ESP/README.md` §"Java volatile vs FreeRTOS Mutex".

## WebServer / REST API Conventions

- **Server:** `ESPAsyncWebServer server(80)` - fully non-blocking; `handleServerRequests()` is an intentional no-op.
- **Browser UI:** serve from PROGMEM via `request->send_P(200, "text/html", INDEX_HTML)` - never load HTML into DRAM.
- **PUT handler with body:** signature is `(AsyncWebServerRequest*, uint8_t* data, size_t len, size_t, size_t)`.
- **JSON:** `JsonDocument doc; serializeJson(doc, json); request->send(200, "application/json", json)`.
- **Route registration:** add all `/api/*` endpoints in `RestService::registerRoutes()`, not scattered in `setupServer()`.
- **API documentation:** every endpoint must be added to both `src/LoRaIoT/src/openapi_json.h` (source of truth, served at `GET /openapi`) and `doc/RestApi.md` (human-readable companion) in the same change that registers it.
- **Manual-use endpoints must be `GET`** so they work from a browser address bar or a link on the index page. Add a link for each one to `index_html.h`.
- **WiFi failure:** `statusLed.setState(LedState::FAULT)` then `vTaskSuspend(NULL)` - ledTask drives SOS forever.
- **Diagnostics endpoints** (already wired in `Server.cpp`): `GET /api/log`, `PUT /api/log/level`, `GET /status`, `GET /reboot`.
- **NEVER call `GET /reset`.** It erases the WiFi credentials and ALL NVS settings (tier, LoRa channel, address, log level, CPU frequency) and reboots into the captive portal, so the developer has to re-provision the node by hand. The agent must never call it, not for testing, not to "restart" a node, not for any reason. To restart a node use `GET /reboot` (`curl -s http://<node-ip>/reboot`), which keeps all NVS settings. If a task seems to need a factory reset, ask the developer instead.
- **Log level** is persisted in NVS (`sc_prefs` / `logLevel`) and survives reboots. Set via captive-portal form or `PUT /api/log/level`.

### Known node addresses (manual testing)

Typical REST API addresses during development (`GET /api/log`, `GET /api/lora/nodes`, etc.):

- LOCAL node: `http://192.168.1.147`
- REMOTE node: `http://192.168.1.174`

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

- **Multi-node log comparison** - serial monitors can only show one COM port at a time, so fetch and compare the live logs of both nodes via the network API using `curl` (addresses in "Known node addresses" above):
  ```bash
  curl -s http://192.168.1.147/api/log
  curl -s http://192.168.1.174/api/log
  ```

## LoRa / E220 Constraints

Architecture and rationale: `doc/Feasibility.md`. Pin mapping: `doc/Feasibility.md` §3.1 and `README.md` §3. Register values and configuration: named constants in `src/lora/src/LoRaTransport.h` and `doc/Feasibility.md` §3.3.

- Lightweight TCP/IP (DNS, NTP, MQTT, small REST with MTU=190) is viable over the LoRa link. General-purpose IP routing is not. See `doc/Feasibility.md` Appendix A.
- **AUX=HIGH does not mean RF transmission is complete** - the last sub-packet may still be in the air. Correct pattern: poll AUX HIGH -> write frame atomically -> wait AUX LOW -> wait AUX HIGH -> add 50ms guard delay.
- **Mode switching:** Set M0/M1, then wait for AUX rising edge >= 2 ms before sending any UART data.
- **Config mode (Mode 3: M1=1, M0=1)** uses fixed **9600 8N1** - switch UART to 9600 before entering, back to 115200 after returning to Mode 0.
- **Write each frame atomically** - never byte-by-byte with delays; the E220 force-flushes after 2 s of UART inactivity.
- **Wrong-format config response is `FF FF FF`** - always check and retry.
- **ADDL = node address** (LOCAL `0x01`, REMOTE `0x02..0xFE`); never `0x0000`/`0xFFFF` (monitor/broadcast).

## Frame Protocol (spec: `doc/LoRaProtocol.md`)

```
[0xAA 0x55][VER 1B][TYPE 1B][SRC 1B][DST 1B][SEQ 2B][LEN 1B][Payload 0-186B][CRC16 2B]
```
Max UART write = 200 bytes (3 E220 routing bytes + 11 B frame overhead + payload). Types: `0x01` PING, `0x02` BEACON, `0x03` ACK, `0x04` DATA, `0x05` DATA_ACK, `0x06` REQUEST, `0x07` RESPONSE. Payloads are protobuf (nanopb): PING and BEACON share the `Beacon` message, `Data` carries `Measurement{metric_id, value}`, and metric definitions (name, unit, scale) are fetched once with `REQUEST(Describe)` and cached by LOCAL. There is no compile-time metric list and no over-the-air channel change (`doc/LoRaProtocol.md` §10 to §12). Addresses: LOCAL `0x01`, REMOTE `0x02`-`0xFE`, `0xFF` broadcast, `0x00` invalid. The E220 never delivers the sender address, so SRC/DST are in our header.

## Code Style

- **No underscore-prefixed member variables** - use plain names (`server`, `ws`). Constructor parameters that shadow a member name use a descriptive prefix (e.g. `webServer` instead of `server`).
- **Never use inline if statements** - always use braces `{}`, even for single-statement bodies:
  ```cpp
  // WRONG
  if (condition) doSomething();

  // CORRECT
  if (condition) {
    doSomething();
  }
  ```
- **Constants preferred over hardcoded values** - never use "magic numbers" directly in logic. Define them as descriptive `static constexpr` or inline constants.
- **Naming semantics for constants** - use uppercase with underscores (e.g. `REG3_MODE_FIXED`, `POLL_INTERVAL_MS`). Group similar registers or settings together with matching prefixes so the codebase is naturally organized.
- **Self-documenting code** - write inline comments or place constants directly near the code where they are used when required, making it clear what each value represents without external lookup.
- **Function prototypes belong in header files** - any function called from another translation unit must be declared in the corresponding `.h` file.
- Use `Log_*` macros everywhere - `Serial.println()` directly is only acceptable in standalone diagnostic sketches, never in firmware library code.
