# LoRaIoT - Technical Debt and Improvement Notes

Tracked improvements that are not urgent enough to block current milestones but
should be revisited before a production release.

---

## ESP32 Memory Layout

Understanding which memory region is appropriate for a given purpose is essential
for avoiding DRAM overflows and making informed sizing decisions.

| Region | Size | R/W | Characteristics |
|---|---|---|---|
| DRAM (`.bss` / `.data`) | ~320 KB | R/W | Fast, byte-addressable. Holds all globals, statics, stack, and heap. WiFi stack consumes ~100 KB of this at runtime, leaving ~170-200 KB for application use. The linker places all `static` and global variables here automatically. |
| IRAM | ~128 KB | R/W | Instruction cache. Can hold data with `IRAM_ATTR` but intended for ISRs and time-critical code only. Not suitable for large data structures. |
| PSRAM (external) | 4-8 MB | R/W | SPI-attached pseudo-SRAM. Byte-addressable; works with `ps_malloc()` / `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. ~10x slower than DRAM. Only present on **WROVER** modules - absent on the common **WROOM**. Requires `CONFIG_SPIRAM` in `sdkconfig`. |
| RTC SLOW RAM | 8 KB | R/W | Survives deep sleep when marked `RTC_DATA_ATTR`. For tiny persistent state (boot counters, wake reason). Too small for registries. |
| Flash - NVS | ~20 KB (partition) | R/W | Wear-levelled key-value store via `Preferences`. Survives power cycles. Already used for channel, address, log level. Slow (SPI flash); not suitable for live measurement caches. |
| Flash - PROGMEM | ~4 MB (partition) | R only | Read-only at runtime. Used for large constant strings via `PROGMEM` / `F()` / `ICACHE_RODATA_ATTR`. Already used for `INDEX_HTML` and `openapi_json.h`. Not writable. |

### DRAM budget at a glance (Arduino WiFi build, ESP32 WROOM)

```
Total DRAM:              ~320 KB
WiFi stack + lwIP:       ~100 KB  (system, not in .bss)
FreeRTOS task stacks:     ~20 KB  (4 tasks × ~4-5 KB each, heap)
FreeRTOS queues/mutexes:   ~4 KB  (heap)
LogBuffer static array:   ~20 KB  (.bss)
Other static globals:      ~3 KB  (.bss)
Phase 1 registries:        ~7 KB  (.bss)
─────────────────────────────────
Estimated free heap:     ~166 KB  (reported by ESP.getFreeHeap() at runtime)
```

The single largest controllable cost in `.bss` is `LogBuffer` - see §1 below for
improvement options. Everything else (nanopb encode/decode, REST handlers,
OpenAPI strings) adds flash size, not DRAM.

### When to use each region

| Use case | Region |
|---|---|
| Mutable runtime state (registries, caches) | DRAM - plain globals/statics |
| Large read-only strings and lookup tables | PROGMEM (flash, read-only) |
| Surviving a deep-sleep cycle | RTC SLOW RAM (`RTC_DATA_ATTR`) |
| Persisting across power cycles | NVS (`Preferences`) |
| Large mutable buffers on WROVER only | PSRAM (`ps_malloc`) |
| ISR / time-critical hot path data | IRAM (`IRAM_ATTR`) |

---

## 1. LogBuffer RAM efficiency (`src/util/src/LogBuffer.h/.cpp`)

### Current design

The ring buffer is a static 2D array of fixed-width rows:

```cpp
char lines[LOG_BUFFER_LINES][LOG_BUFFER_LINE_LEN];  // 128 × 160 = 20 KB static BSS
```

All 20 KB are reserved at link time regardless of how many entries exist.
A typical log line (`[INFO ][Application.cpp:52] LoRa started`) is ~50 chars,
so each 160-byte slot wastes ~110 bytes of padding - roughly 13 KB of the 20 KB
is air at normal INFO-level logging.

The REMOTE node has no serial monitor attached (only power), making `GET /api/log`
over WiFi the sole diagnostic channel. The buffer must therefore stay in RAM;
dropping it is not an option.

### Option A - Reduce fixed dimensions (trivial)

Change the two constants in `LogBuffer.h`:

```cpp
static constexpr int LOG_BUFFER_LINES   = 64;   // was 128
static constexpr int LOG_BUFFER_LINE_LEN = 100;  // was 160
```

Result: 64 × 100 = **6.4 KB** (saves 13.6 KB). Still wastes space per line but
requires no structural change. Good as a quick fix.

Constraint: lines longer than 99 chars are silently truncated. Current longest
lines (`[INFO ][LoRaTransport.cpp:163] LoRa TX: target=...`) fit within 100 chars
at INFO level; DEBUG/TRACE hex dumps may be clipped.

### Option B - Heap-allocated pointer ring (moderate effort, ~50 lines)

Store only a pointer array statically; `malloc` each line on first write and
`free` it when the ring overwrites it:

```cpp
char* lines[LOG_BUFFER_LINES];   // 128 × 4 B = 512 B static BSS
```

Each slot is allocated to exactly `strlen(line) + 1` bytes on first use.

- Static BSS: **512 B**
- Heap at runtime: ~64 lines × ~55 B avg = **~3.5 KB live**
- Cost: one `malloc` + one `free` per log call (~5 µs each on ESP32 heap allocator)
- Risk: mild heap fragmentation if line lengths vary widely (they don't in practice)

### Option C - Single contiguous heap block with offset index (best balance)

Allocate one flat buffer on the heap at `begin()`. Keep a compact `uint16_t`
offset index. Lines are packed end-to-end with `'\0'` separators; the ring
advances a write pointer and wraps, evicting the oldest entry when space runs out.

```cpp
uint16_t offsets[LOG_BUFFER_LINES];  // 128 × 2 B = 256 B static BSS
char*    buf;                         // single heap alloc at begin(), e.g. 6 KB
```

- Static BSS: **256 B**
- Heap: one `malloc` of a fixed size at boot - no further allocations ever
- No fragmentation risk
- Implementation: ~50 extra lines in `append()` and `getJson()`
- This is the pattern used by embedded syslog implementations

### Recommendation

Apply **Option A** immediately if DRAM pressure is felt. Implement **Option C**
as a dedicated task once Phase 2-3 of the REST plan are complete and total heap
usage is stable.

---

## 2. REST API URL design: path parameters vs query parameters

### Context

The Phase 2 REST endpoints address variable resources: a specific remote node
(`addr`), a specific device on that node (`device`), and so on. Two URL styles
were considered.

### Option A - Path parameters (strict REST convention)

```
GET /api/lora/nodes/2/devices/1/metrics
```

This is the canonical REST style where every resource has its own URI. It is
the pattern described in the original `RestPlan.md` (`{addr}`, `{device}`).

**Problem on ESPAsyncWebServer:** the library's `server->on(path, ...)` matcher
does exact string matching only. It has no built-in support for named path
parameters. Supporting `{addr}` requires either:

- A custom `AsyncWebHandler` subclass with a `canHandle()` override that does
  prefix matching and manual `request->url()` substring extraction, or
- A dedicated helper (`UrlParams.h`) that parses segment N from the raw URL
  string after the fact.

Either approach adds non-trivial code with its own edge cases (trailing slashes,
hex vs decimal address notation, `url()` encoding). On a two-node embedded
system with no external API consumers this complexity has no payoff.

### Option B - Query parameters (chosen)

```
GET /api/lora/nodes/devices?addr=2&device=1
GET /api/lora/nodes/metrics?addr=2&device=1&live=true&timeout=2000
POST /api/lora/nodes/echo?addr=2
GET  /api/lora/nodes/ping?addr=2
POST /api/lora/nodes/data?addr=2&ack=true
```

`ESPAsyncWebServer` handles query parameters natively via `hasArg`/`arg`
(the same API already used by `GET /api/log?clear=1` in `Server.cpp`):

```cpp
if (!request->hasArg("addr")) { /* 400 */ }
uint8_t addr = (uint8_t)request->arg("addr").toInt();
```

Zero helper code. Every route is a flat exact-match string. Parameter validation
is a single `getParam()` + range-check inline in the handler.

**Trade-off accepted:** the URLs are slightly less "RESTful" by textbook
convention. In practice REST style is a guideline, not a protocol. The W3C and
RFC 3986 make no distinction between path segments and query strings as resource
identifiers. For a firmware API used only by `curl`, Home Assistant, and the
built-in Swagger UI, query parameters are indistinguishable from path parameters
in every observable way.

### Full Phase 2 URL reference

| Method | Path | Query params | Node |
|---|---|---|---|
| `GET` | `/api/lora/nodes` | - | LOCAL |
| `GET` | `/api/lora/nodes/devices` | `addr` | LOCAL |
| `GET` | `/api/lora/nodes/devices` | `addr`, `device`, `?refresh=true` | LOCAL |
| `GET` | `/api/lora/nodes/metrics` | `addr`, `device`, `?live=true`, `?timeout=ms` | LOCAL |
| `POST` | `/api/lora/nodes/data` | `addr`, `?ack=true\|false` | LOCAL |
| `POST` | `/api/lora/nodes/echo` | `addr` | LOCAL |
| `GET` | `/api/lora/nodes/ping` | `addr` | LOCAL |
| `POST` | `/api/lora/nodes/ping` | `addr` | LOCAL |
| `GET` | `/api/lora/ping` | - | LOCAL (broadcast) |
| `POST` | `/api/lora/beacon` | - | LOCAL |
| `GET` | `/api/lora/stats` | - | LOCAL |
| `POST` | `/api/sensor/data` | - | REMOTE |

---

## 3. WebSocket infrastructure: keep and build a GUI, or remove

### Current state

`/ws/data` is a live, connectable WebSocket endpoint. `WebSocketService::broadcastTask`
(`src/service/src/WebSocketService.cpp`) runs as a FreeRTOS task on every boot,
loops at 1 s intervals, and sends nothing - the `ws->textAll()` call is commented
out pending a payload definition. The infrastructure (endpoint registration,
task creation, mutex) is wired but produces no output.

The endpoint was carried over from the SerialController template project, which
had a real browser GUI that consumed the 1 s push to drive a live data table.

### Why WebSockets only make sense with a browser GUI

REST polling (`GET /api/lora/nodes/metrics`) works for any client. WebSockets
add value only for a browser page that must update without user-initiated
reloads - a live dashboard. There is no device-to-device or server-push use
case in this project that would justify WebSockets over plain HTTP.

REMOTE runs in SoftAP mode (LAN-disconnected after provisioning); a browser
cannot reach it over the LAN. A GUI is therefore only meaningful on LOCAL.

### Option A - Add a real browser GUI on LOCAL (keep WebSockets)

Implement a self-contained HTML page (inline CSS + JS WebSocket client) stored
in PROGMEM at `src/LoRaIoT/src/index_html.h`, served at `GET /`. The page
connects to `ws://<host>/ws/data` and renders a live dashboard showing:

- Active tier and gateway status
- Node registry table (addr, boot_id, registry_rev, last_seen)
- Per-device metric values with age
- LoRa link stats (TX/RX/CRC/ACK counters)

The SerialController template (`ESP/SerialController/src/SerialController/src/index_html.h`,
~1,110 lines) is the reference pattern to adapt.

**Cost:**
- Flash: ~30-40 KB additional PROGMEM for the HTML/CSS/JS
- Current flash headroom: ~105 KB (92% used of 1,280 KB)
- After frame protocol layer (est. ~20-30 KB): headroom shrinks to ~55-75 KB
- A 35 KB GUI would consume roughly half that remaining margin
- `WebSocketService::broadcastTask` payload must be implemented
- `index_html.h` deprecation notice must be removed

**Constraint:** implement only after the frame protocol layer is complete and
the final flash budget is known. If headroom is < 50 KB at that point, the GUI
should be hosted externally (e.g. a static page on the LAN that fetches from
the ESP32 API) rather than embedded in flash.

### Option B - Remove WebSocket infrastructure (simplify)

Remove the WebSocket endpoint and service entirely. `GET /openapi/ui` (Swagger UI)
already provides a fully interactive, self-documenting explorer for every endpoint.
Cached and live metric reads are available via `GET /api/lora/nodes/metrics`.
Neither requires a persistent connection.

**What to remove:**
- `src/service/src/WebSocketService.h/.cpp` - delete both files
- `src/LoRaIoT/src/Server.cpp` - remove `AsyncWebSocket ws(WS_PATH)`,
  `WebSocketService wsService(&ws)`, `server.addHandler(&ws)`, `wsService.begin()`
- `src/LoRaIoT/src/Server.h` - remove `WS_PATH` constant and `extern WebSocketService wsService`
- `src/LoRaIoT/src/Application.cpp` - remove the drain-WebSocket-command-queue TODO comment
- `src/LoRaIoT/src/index_html.h` - already has deprecation notice; can be removed or replaced
  with a one-line redirect to `/openapi/ui`
- `doc/RestApi.md` - remove the `WebSocket /ws/data` entry from the summary table and section
- `src/LoRaIoT/src/openapi_json.h` - no entry to remove (WebSocket is not in the OpenAPI spec)

**Savings:** ~4 KB flash (compiled task code) + 4 KB DRAM (task stack) + simplifies
the threading diagram in `doc/TechDebt.md §ESP32 Memory Layout` and `README.md`.

### Recommendation

Do not act on either option until the frame protocol layer is complete and a
final flash measurement is available. At that point:

- If flash headroom > 60 KB: Option A is viable.
- If flash headroom < 60 KB: choose Option B.
- If a LAN-hosted dashboard is acceptable: choose Option B regardless and host
  the GUI as a static page that polls the REST API.
