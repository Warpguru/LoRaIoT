# Project Coding Rules (Non-Obvious Only)

- The `src/main/java/` directory is empty - do NOT create Java files there. All firmware is C++ for ESP32.
- `ESP/` is a symlink to a read-only reference project - never write to anything under `ESP/`. Use it only to copy proven patterns.
- `HardwareSerial LoRaSerial(2)` on UART2 pins 16/17 is the established pattern from `doc/Feasibility.md` - do not use `Serial1` or software serial.
- Payload compression is required before LoRa transmission - never forward raw TCP/IP frames. Use MessagePack, CBOR, or the custom binary frame spec in `doc/Feasibility.md` §6.
- CRC16 must be computed over **Type + SeqNo + PayloadLen + Payload** only (not the header bytes `0xAA 0x55`).
- AUX pin check is **mandatory** before every `LoRaSerial.write()` - not optional defensive code.
- E220 register writes must use Mode 3 (M1=1, M0=1) at **9600 baud** fixed - switching the baud in REG0 does not affect the config-mode UART speed.
- Use `TASK_CREATE` macro (defined in `src/LoRaIoT/src/TaskUtils.h`) instead of `xTaskCreatePinnedToCore` or `xTaskCreate` directly.
- **No underscore-prefixed member variables** (`_server`, `_ws`, etc.) - use plain names (`server`, `ws`). Constructor parameters that would shadow the member name use a descriptive prefix instead (e.g. `webServer`, `webSocket`).
- **Function prototypes belong in header files** - never declare a function only in a `.cpp` file if it is called from another translation unit.
