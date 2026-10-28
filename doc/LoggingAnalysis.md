# Logging Infrastructure Analysis: PROGMEM for Format Strings

**Scope:** `src/util/src/LogBuffer.h` / `LogBuffer.cpp` and all `Log_*` call sites in the codebase.
**Question:** Should format string literals in `Log_*` calls be moved to PROGMEM?

---

## 1. How the Logging Infrastructure Works

### Macro chain

```
Log_info("fmt", ...)
  -> LogBuffer.logInfo(__FILE__, __LINE__, "fmt", ...)
     -> append(LogLevel::INFO, file, line, "fmt", va_list)
        -> vsnprintf(message, sizeof(message), format, formatArgs)   // stack buffer, 160 B
        -> snprintf(logLine, LOG_BUFFER_LINE_LEN, "[%-5s][%s:%d] %s", ...)
        -> Serial.write(logLine + "\r\n")   // one write, so lines from different tasks cannot interleave
        -> strncpy into ring buffer (128 x 160 B static BSS)
```

The `format` parameter is a `const char*` passed all the way to [`vsnprintf`](src/util/src/LogBuffer.cpp:212). There is no intermediate copy; only the rendered output string is ever written to the stack or ring buffer. The format string itself is consumed read-only by `vsnprintf` and then discarded.

### Ring buffer

`lines[128][160]` lives in BSS (DRAM) - about 20 KB of static RAM. This is the primary RAM cost of the logger and is unrelated to format string storage.

---

## 2. Where String Literals Live on ESP32

### The ESP32 memory model

The ESP32 runs the Xtensa LX6 (or LX7) core with a **Harvard-derived memory map**, but crucially it exposes flash via an **MMU-backed instruction/data cache** (the IRAM/DRAM bus). The IDF linker script places the `.rodata` section (which holds all C++ `const char*` string literals) in **flash (SPI Flash)** and maps it through the cache into the CPU's virtual address space. The CPU can read `.rodata` addresses with ordinary load instructions - no special `pgm_read_byte` dance is required.

This means:

> **String literals in `Log_info("some text")` already live in flash, not DRAM, on ESP32.**

They are placed in `.rodata` at link time. The linker for the ESP32 Arduino/ESP-IDF toolchain does NOT bulk-copy `.rodata` into DRAM at startup (unlike `.data`). The string is accessed directly through the flash cache on every `vsnprintf` call.

### What `PROGMEM` does on ESP32 vs AVR

| Platform | `PROGMEM` attribute | Effect |
|---|---|---|
| **AVR** | `__attribute__((progmem))` | Forces symbol into a separate Harvard `PROGMEM` address space that is not byte-addressable by normal pointers. Access requires `pgm_read_byte()` / `pgm_read_ptr()` helpers. `vsnprintf_P` is the PROGMEM-aware variant. |
| **ESP32 (Arduino/IDF)** | `__attribute__((section(".rodata")))` | Effectively a no-op for string literals - the compiler already places `const char*` string literals in `.rodata` in flash. `PROGMEM` is defined in the ESP32 Arduino core headers as an alias for this same section attribute. It changes nothing. |

On ESP32, `PROGMEM` is defined in `pgmspace.h` as:

```cpp
#define PROGMEM __attribute__((section(".rodata")))
```

The ESP32 Arduino core also provides `F()` macro (wraps a string in a `__FlashStringHelper*`), but since all `.rodata` is already in flash, `F()` provides no benefit over a plain string literal. It exists for source-level compatibility with AVR libraries.

### Does `vsnprintf_P` exist on ESP32?

`pgmspace.h` on ESP32 Arduino defines `vsnprintf_P` as an alias for standard `vsnprintf`:

```cpp
#define vsnprintf_P(buf, size, fmt, args)  vsnprintf(buf, size, fmt, args)
```

It compiles and links, but it is **identical** to `vsnprintf` - the `_P` suffix is meaningless on ESP32 because the format string is already accessible via a normal pointer.

---

## 3. Is There Actually a Problem to Solve?

No. On ESP32:

- String literals in `Log_info("format string")` already reside in flash (`.rodata`).
- They are **not** copied into DRAM at boot.
- `vsnprintf` can read them directly without any helper, because flash is memory-mapped.
- Adding `PROGMEM` or `F()` wrappers changes the generated machine code not at all.

The only RAM cost of the logging system is the **ring buffer** (`lines[128][160]` = ~20 KB in BSS) and the two stack-allocated temporary buffers (`message[160]` + `logLine[160]`, 320 B per call) that live and die within `append()`. Neither is affected by format string placement.

---

## 4. Call Site Survey

### Total call count: 312 calls across 18 source files

| File | Log_* calls |
|---|---|
| `diagnostics/src/Diagnostics.cpp` | 77 |
| `LoRaIoT/src/Application.cpp` | 59 |
| `LoRaIoT/src/Server.cpp` | 26 |
| `service/src/RestService.cpp` | 20 |
| `lora/src/LoRaTransport.cpp` | 20 |
| `LoRaIoT/src/LoRaBroker.cpp` | 19 |
| `registry/src/PeerRegistry.cpp` | 13 |
| `LoRaIoT/src/DescribeCrawler.cpp` | 11 |
| `LoRaIoT/src/PingTask.cpp` | 10 |
| `util/src/LogBuffer.h` + `LogBuffer.cpp` | 11 |
| `registry/src/DeviceTable.cpp` | 9 |
| `LoRaIoT/src/PushTask.cpp` | 9 |
| `lora/src/LoRaFrame.cpp` | 7 |
| `LoRaIoT/src/BeaconTask.cpp` | 6 |
| `lora/src/LoRaState.cpp` | 5 |
| `LoRaIoT/src/Tier.cpp` | 5 |
| `LoRaIoT/src/LoRaConfig.cpp` | 4 |
| `service/src/WebSocketService.cpp` | 1 |
| **Total** | **312** |

### Pattern observed

All 133 call sites use plain string literals as format arguments, e.g.:

```cpp
Log_info("GET /api/diag/e220: E220 check requested");
Log_error("LoRaTransport::begin - Failed to create TX Queue!");
Log_warn("LoRaTransport::sendRaw - Outgoing queue full! Frame dropped.");
```

None currently use `F()`, `PROGMEM`, or `PGM_P`. All strings are already in flash.

---

## 5. Pros / Cons of Moving Format Strings to PROGMEM

| | **Pro** | **Con** |
|---|---|---|
| **DRAM savings** | None - strings are already in `.rodata` (flash) on ESP32. Zero bytes saved. | - |
| **Portability to AVR** | Code would compile and work on AVR targets without change. | The project targets ESP32 only. AVR portability provides no value. |
| **`vsnprintf` compatibility** | `vsnprintf_P` exists on ESP32 but is a no-op alias. No risk. | On AVR, `vsnprintf` would silently misread PROGMEM pointers and produce garbage output. The `append()` function in `LogBuffer.cpp` would need to call `vsnprintf_P` instead of `vsnprintf` - a real AVR migration risk. |
| **Code clarity** | None - `F("literal")` and `PROGMEM` markers add visual noise with no runtime meaning on ESP32. | Adds call-site changes (`F("...")` wrapper on every format string) for zero functional benefit. Increases diff size, review burden, and future maintenance cost. |
| **Macro hygiene** | None. | `F()` returns `__FlashStringHelper*`, which is NOT a `const char*`. `vsnprintf` does not accept it directly. Every call site would require a cast or a new macro overload, complicating `LogBuffer.h`. |
| **Existing ESPAsyncWebServer / ArduinoJson usage** | Those libraries already use PROGMEM where beneficial on ESP32 (they don't). | Pattern inconsistency: no other part of this codebase uses `F()` for performance reasons. |
| **Source compatibility** | Wrapping in `F()` would make the call sites look idiomatic to Arduino AVR developers. | The existing call sites already look clean and readable without the wrapper noise. |

---

## 6. Specific Questions Answered

**Q: Where do string literals in `Log_info("GET /api/diag/e220: E220 check requested")` live at runtime?**

In **flash** (SPI Flash, accessed via the MMU-backed cache). They are in the `.rodata` section and never copied to DRAM. Not stack, not heap.

**Q: What does `PROGMEM` actually do on ESP32?**

It expands to `__attribute__((section(".rodata")))` - exactly where string literals already are. It is a no-op.

**Q: Does `F()` macro exist and work on ESP32?**

Yes, it is defined in the ESP32 Arduino core. It compiles. It returns a `__FlashStringHelper*` that points into `.rodata`. Since `.rodata` is already in flash and is memory-mapped, `F()` provides no runtime benefit over a plain string literal. Its primary purpose is AVR source-level compatibility.

**Q: If string literals are already in flash, is there a problem to solve?**

No. There is no DRAM waste to reclaim. The only real DRAM cost in the logger is the 20 KB ring buffer in BSS, which is unaffected by format string placement.

**Q: What would be required to use PROGMEM format strings correctly?**

On AVR (hypothetically): replace `vsnprintf` with `vsnprintf_P` in `append()`, change the `format` parameter type from `const char*` to `PGM_P`, update all five `logXxx` method signatures in the class and header, and wrap every call site format string in `PSTR()` or `F()`. On ESP32, all of this compiles to the same machine code as today.

---

## 7. Recommendation

**Do not refactor the logging to use PROGMEM format strings.**

The motivation for PROGMEM on AVR - keeping string literals out of the severely limited 2 KB SRAM - does not apply to the ESP32. All string literals are already in flash. Adding `PROGMEM` markers, `F()` wrappers, and `vsnprintf_P` to the call sites and `LogBuffer` internals would:

- Save **0 bytes** of DRAM.
- Add **non-trivial refactor complexity** (type change from `const char*` to `PGM_P` / `__FlashStringHelper*` throughout the API).
- Introduce a **silent correctness hazard**: if the codebase ever needed to build on AVR, `vsnprintf` would silently produce garbage output when reading PROGMEM pointers - the opposite of the safe outcome.
- Pollute call sites with syntactic noise (`F("...")`) that conveys false intent on this platform.

If DRAM pressure becomes a concern in the future, the higher-value target is the ring buffer itself - documented in `doc/TechDebt.md §1` - which holds 20 KB of fixed-width rows in BSS. Switching to a variable-length or compressed ring buffer would recover real DRAM. Format strings are not the bottleneck.
