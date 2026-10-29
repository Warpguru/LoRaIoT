# WebSocket Dashboard Feasibility Analysis

## Background

The current `src/LoRaIoT/src/index_html.h` is a minimal landing page that links to
the OpenAPI/Swagger UI. It does not show live data. The proposal is to replace it
with a real-time dashboard using the WebSocket infrastructure already present in
this project, using `ESP/SerialController/src/SerialController/src/index_html.h`
as a structural template.

---

## 1. Template Analysis (SerialController)

[`ESP/SerialController/src/SerialController/src/index_html.h`](../ESP/SerialController/src/SerialController/src/index_html.h)

- **Size:** 55,073 bytes on disk including the PROGMEM wrapper; the HTML/CSS/JS
  payload is approximately **53 KB**. Fully self-contained, zero CDN dependencies,
  no external fonts or images.
- **WebSocket model:**
  - Client connects to `ws://<host>/ws/data` immediately on page load.
  - Exponential backoff reconnect on close/error: starts at 1 s, doubles each
    attempt, caps at 30 s.
  - **Server pushes JSON to all clients every 1 second** via a FreeRTOS task using
    `vTaskDelayUntil` for a stable period. The client never polls; it is entirely
    server-driven.
  - Clients can also *send* JSON commands (setVoltage, setCurrent, setOutput,
    setKeypad). Those are received into a single-slot FreeRTOS queue and drained
    by the application loop on Core 1.
  - An activity indicator flashes for 250 ms on every WS send or receive.
- **Server-side broadcast payload:** ~300-400 bytes per push, containing 21 scalar
  fields (doubles, bools, strings). Serialised into a `JsonDocument` and sent with
  `ws->textAll(json)`. `ws->cleanupClients()` is called each cycle to reclaim
  memory for stale connections.
- **Reusable:** the connect/reconnect loop, activity indicator, and exponential
  backoff pattern are directly applicable verbatim.
- **Not reusable:** ~25 KB of domain-specific machinery (Canvas slider tick marks,
  press-and-hold auto-repeat timers, debounced setpoint sends, topology-aware
  voltage ceilings) - none of that is needed for LoRaIoT.

---

## 2. LoRaIoT WebSocket Infrastructure - What Already Exists

`src/service/src/WebSocketService.cpp` already provides:

- A 1 s `vTaskDelay` broadcast task (priority 1, 4096 B stack), registered with
  `TaskRegistry`.
- Current broadcast payload (~150-200 B JSON):

```json
{
  "uptime_s":    <seconds>,
  "tier":        "LOCAL" | "REMOTE" | "UNKNOWN",
  "lora": {
    "tx_frames":  <uint32>,
    "rx_frames":  <uint32>,
    "crc_errors": <uint32>,
    "ack_loss":   <uint32>
  },
  "echo": {
    "sent":          "<text>",
    "received":      "<text>",
    "seq_no":        <uint32>,
    "round_trip_ms": <uint32>,
    "success":       <bool>
  } | null
}
```

- The WebSocket path `/ws` is already registered and served.

> **Note on `vTaskDelay` vs `vTaskDelayUntil`:** SerialController uses
> `vTaskDelayUntil` for a stable period regardless of serialisation time. LoRaIoT
> uses `vTaskDelay`, so the interval drifts by the serialisation time. For a 1 s
> period and a few hundred bytes of JSON this drift is negligible (<1 ms).

**What is missing** to power a live dashboard:

| Missing piece | Notes |
|---|---|
| Sensor registry snapshot in broadcast | `GET /api/lora/nodes/sensor` data, not yet included |
| Control registry snapshot in broadcast | `GET /api/lora/nodes/control` data, not yet included |
| Inbound command queue | Only needed if browser writes control setpoints; not required for a read-only dashboard |

---

## 3. PROGMEM (Flash) Budget

| Existing constant | File | Approx. size | Notes |
|---|---|---|---|
| `INDEX_HTML` (current) | `src/LoRaIoT/src/index_html.h` | ~3,800 B | Static link list, no JS. Replaced by new UI. |
| `OPENAPI_JSON` | `src/LoRaIoT/src/openapi_json.h` | ~41,491 B | Full OpenAPI 3.0 spec. |
| `OPENAPI_UI_TEMPLATE` | `src/LoRaIoT/src/openapi_json.h` | ~1,186 B | Swagger UI CDN loader stub. |
| **Total today** | | **~46,500 B** | |
| SerialController template (reference) | `ESP/SerialController/.../index_html.h` | ~53,000 B | Full interactive UI with Canvas sliders. |
| Estimated LoRaIoT read-only dashboard | `src/LoRaIoT/src/index_html.h` | **20-30 KB** | No sliders, no Canvas, no debounce. |

The default ESP32 Arduino partition table allocates **1,310,720 bytes (1.25 MB)**
to the application partition. No custom `partitions.csv` exists in this project,
so the default is in use. Total flash for string constants is currently ~46.5 KB -
a trivial fraction of available space. Even if the new page were identical in size
to the SerialController template (~53 KB), the combined total would be ~100 KB,
leaving more than 1.2 MB for code, stack, and NVS. Flash is **not a constraint**.

The current `INDEX_HTML` is deprecated (noted in its file header) and will be
replaced wholesale - the new page costs no additional flash relative to the old
page being deleted.

---

## 4. DRAM / Heap Impact

No persistent DRAM cost. The broadcast JSON is heap-allocated via `JsonDocument`,
serialised to all connected WebSocket clients, and immediately freed. Estimated
broadcast payload size for a typical multi-node deployment: **600-1200 B**.

The broadcast task stack should be bumped from **4096 → 6144-8192 bytes** (one-line
change in the `TASK_CREATE` call) to accommodate the larger `JsonDocument` needed
for the registry snapshot.

---

## 5. Polling Interval Recommendation

| Constraint | Value | Implication |
|---|---|---|
| PING RTT at 62.5 kbps (default) | 117-137 ms | LoRa channel is idle most of the time at 1 s push cadence. |
| PING RTT at 2.4 kbps (slowest) | 698-700 ms | A single round-trip nearly consumes a full second. |
| DATA_ACK delivery | Event-driven (REMOTE pushes whenever a sensor posts) | Sensor values arrive at most once per DATA push cycle, not on a timer. |
| BEACON interval (REMOTE) | 60 s (slow) | Liveness updates are infrequent; `peer_last_seen_ms_ago` is the freshness indicator. |
| broadcastTask interval | 1,000 ms (current) | Already set. JSON snapshot of cached registry data is produced at this rate. |

**Recommendation: keep the 1 s push interval.**

- Sensor values are pushed by REMOTE on demand, not on a timer. The LOCAL cache is
  updated whenever a `DATA_ACK` arrives; broadcasting faster yields no new data.
- At 2.4 kbps a single LoRa round-trip takes ~700 ms. The broadcast task reads
  **only from the in-RAM registry cache** and never initiates LoRa traffic, so the
  air rate does not gate the UI refresh rate.
- Going faster than 1 s increases WiFi and CPU load with no benefit, since the
  registry cache cannot update faster than the LoRa channel delivers frames.
- **Minimum safe UI display interval: 1 s.** Stale values are handled gracefully by
  showing `age_ms` / `peer_last_seen_ms_ago`.

---

## 6. Proposed UI Scope

### Minimal starting point - formatted JSON (~5 KB HTML/JS)

A `<pre>` block with `JSON.stringify(data, null, 2)` updated on every WebSocket
message. Requires almost no JS beyond the WebSocket connect/reconnect loop from
the SerialController template.

### Extended broadcast payload (nodes[] to add to WebSocketService.cpp)

```json
{
  "uptime_s": ..., "tier": "LOCAL"|"REMOTE", "lora": {...}, "echo": {...}|null,
  "nodes": [
    {
      "addr": 2, "online": true, "peer_last_seen_ms_ago": 412,
      "devices": [
        {
          "device": 1, "name": "TempSensor", "has_data": true, "age_ms": 1200,
          "metrics": [
            { "id": 1, "name": "Temperature", "unit": "C", "scale": 10, "value": 215 },
            { "id": 2, "name": "Humidity",    "unit": "%", "scale": 1,  "value": 63  }
          ]
        }
      ]
    }
  ]
}
```

Estimated payload: **400-800 bytes** for a typical 2-node, 2-device, 3-metrics
deployment. Comfortably within `textAll()` limits.

### Practical starting point - node/device table (~15-20 KB HTML/JS)

No CSS framework. Per-node sections, one row per metric:

```
LoRaIoT Dashboard - LOCAL                         [uptime] [air rate]
-------------------------------------------------------------------
Node 3 · online · last seen 2s ago · boot 93207671
  Thermostat   target_temp   21.00 C   age 92s   alert: -
-------------------------------------------------------------------
```

**LOCAL node:** shows sensor readings cached from REMOTE peers (`nodes[]` keyed by
addr, devices, metrics with scaled values and age).

**REMOTE node:** same layout, shows control setpoints pushed from LOCAL, plus the
node's own sensor data in the owner view. The `tier` field drives what labels the
browser shows.

Both variants are well within the flash budget.

### Summary table

| Dimension | Current | After change | Status |
|---|---|---|---|
| PROGMEM (flash strings) | ~46.5 KB | ~68-76 KB (replace 3.8 KB with 25-30 KB) | OK - ample headroom |
| DRAM / heap at runtime | No change from current WS task | ~1-2 KB extra per broadcast (larger JSON; freed immediately) | OK - negligible |
| Broadcast task stack | 4,096 B | Increase to 6,144-8,192 B (one-line change) | Minor adjustment needed |
| WS server-side push | Working, 1 s | Add registry data to payload; no structural change | Additive only |
| UI refresh interval | N/A | 1 s (matches LoRa DATA push cadence) | Correct |
| Template availability | - | SerialController WS connect/reconnect/activity pattern reusable verbatim | Direct fit |
| External CDN | Swagger UI loads from CDN | New dashboard 100% self-contained (no CDN) | Works offline |
| Inbound WS commands | Not implemented | Not needed for read-only dashboard; add later if browser control writes required | Optional / deferred |

---

## 7. Required Changes

| # | Change | File(s) | Effort |
|---|---|---|---|
| 1 | Add `nodes[]` registry snapshot to `broadcastTask` payload | `WebSocketService.cpp` | Small |
| 2 | Write new `INDEX_HTML` (15-25 KB, read-only dashboard) | `src/LoRaIoT/src/index_html.h` | Medium |
| 3 | Bump `broadcastTask` stack from 4096 to 6144 bytes | `WebSocketService.cpp` | Trivial |
| 4 | *(optional)* Inbound WS command queue to POST control setpoints from browser | `WebSocketService.cpp`, `Application.cpp` | Medium |

---

## 8. Verdict

**Feasible - no significant caveats.**

The WebSocket infrastructure is already in place. The broadcast task needs a
larger payload (change 1) and slightly more stack (change 3). The new dashboard
page (change 2) is the main effort - modelling the connect/reconnect loop on
the SerialController template keeps it straightforward. Starting with formatted
JSON output limits scope further and is a valid first milestone before adding
a proper table layout.
