# Frequently Asked Questions

## 1. System Overview

### What is LoRaIoT and how does it work?

LoRaIoT is a small wireless network of ESP32 microcontroller nodes that talk to each other over
a LoRa radio link. LoRa is a low-power, long-range radio technology well suited to IoT
applications where WiFi does not reach or battery life matters.

The network always has exactly **one LOCAL node** and up to **three REMOTE nodes**. Each node
exposes a REST API. LOCAL connects to your WiFi network; a REMOTE node either joins your WiFi
network, hosts its own WiFi access point, or does both (see "Which WiFi modes can a REMOTE node
use?" below).

### What is the LOCAL node?

LOCAL is the central hub of the system. It sits on your home network and acts as the bridge
between the LoRa radio world and everything else.

- It collects sensor readings pushed to it by each REMOTE node and makes them available via
  its REST API.
- It holds the control setpoints (target values) for each REMOTE node and sends them out over
  LoRa.
- It is the only node that home automation software such as **Home Assistant** talks to. Home
  Assistant reads sensor data from LOCAL and writes control setpoints to LOCAL - it never
  needs to know that REMOTE nodes exist.

### What is a REMOTE node?

A REMOTE node lives close to whatever it is monitoring or controlling - often in a location
where running a network cable would be impractical. It communicates with LOCAL exclusively over
the LoRa radio link.

Up to three REMOTE nodes are supported, each with a unique address (node 1, 2, or 3).

Typical examples:

| REMOTE role | What it does |
| :--- | :--- |
| **IoT sensor node** | Reads a temperature, humidity, or voltage sensor and pushes the measurements to LOCAL over LoRa. |
| **DC/DC converter controller** | Receives a target voltage or current setpoint from LOCAL and adjusts its output accordingly. |
| **Relay / actuator node** | Receives an on/off or PWM command from LOCAL and drives a load (pump, fan, heater, etc.). |

### How do the pieces fit together?

```
Home Assistant                   LOCAL node                        REMOTE nodes            IoT devices
(or any REST client)             (WiFi + LoRa)                     (WiFi + LoRa)           (local to REMOTE)
─────────────────────            ───────────────────────           ─────────────           ─────────────────
Reads sensor data     <──REST──  Caches sensor readings  <──LoRa── Node 1        <──REST── Sensor hardware
Writes control data   ──REST──>  Holds control setpoints ──LoRa──> Node 2        ──REST──> DC/DC converter
                                                         ──LoRa──> Node 3                  Relay / actuator
```

LOCAL is the only node that needs to be reachable from your network. REMOTE nodes use WiFi for
their own REST API: either as an access point that the local IoT devices join, or as a client of
your network (useful for direct inspection or firmware updates). All data between the nodes
flows over LoRa.

**Connecting IoT devices to a REMOTE node:**

Out of the box, any IoT device that can make HTTP calls can push sensor data to or read control
setpoints from a REMOTE node using its REST API - no firmware changes needed. In `AP` or `AP_STA`
mode the device joins the REMOTE node's WiFi (SSID `LoRaIoT-Node-<n>`, default address
`192.168.4.1`); in `STA` mode it uses the REMOTE node's address on your network.

Interfacing via the ESP32's hardware peripherals (GPIO, I2C, SPI, ADC, PWM, UART, etc.) is
possible but requires modifying this application to add that support. It is not included out of
the box and is constrained by the flash and RAM remaining after the base firmware is flashed.


### Which WiFi modes can a REMOTE node use?

| Mode | What it does | How to reach the node |
| :--- | :--- | :--- |
| `STA` (default) | The node joins your WiFi network, as LOCAL does. | Its address on your network |
| `AP` | The node hosts its own WiFi access point and joins no other network. | Join its SSID, then `192.168.4.1` (default) |
| `AP_STA` | Both at once: own access point and connection to your network. | Either address |

### How do I switch a REMOTE node to access point mode, and back?

1. Provision the node in `STA` mode first (tier and LoRa channel are set in the captive portal,
   which is not available in `AP` mode).
2. Send `PUT /api/wifi/mode` with `{"mode":"AP"}` (optionally `ssid`, `password`, `ip`, `channel`).
   The node reboots into the new mode and is then no longer reachable at its old network address.
3. To go back, join the node's SSID and send `PUT /api/wifi/mode` with `{"mode":"STA"}`.

Details are in [`doc/RestApi.md`](doc/RestApi.md).

---

## 2. Data Types

### What is sensor data?

Sensor data is the physical measurements collected by a REMOTE node - things like temperature,
voltage, or humidity read from hardware sensors attached to that node.

REMOTE **owns** its sensor readings: it creates them locally and pushes them over the LoRa
radio to LOCAL, which keeps a cached copy.

### What is control data?

Control data is the set of setpoints and configuration values that LOCAL sends out to a REMOTE
node to tell it what to do - for example a target temperature or a relay state.

LOCAL **owns** the control values: it creates them and transmits them over LoRa to REMOTE,
which keeps a cached copy.

### Which node owns which data?

```
┌─────────────────────────────────┐                 ┌────────────────────────────────────┐
│           LOCAL Node            │                 │           REMOTE Node              │
│                                 │                 │                                    │
│  • Control Data:  OWNER         │   LoRa Radio    │  • Control Data:  CACHE            │
│    (Holds setpoints, relays)    │ ─────────────── │    (Receives setpoints from LOCAL) │
│                                 │                 │                                    │
│  • Sensor Data:   CACHE         │                 │  • Sensor Data:   OWNER            │
│    (Receives sensor readings)   │                 │    (Physical sensors attached)     │
└─────────────────────────────────┘                 └────────────────────────────────────┘
```

A simple rule: each node owns the data it **generates**, and caches the data it **receives**.

---

## 3. Retrieving Data

### How do I read sensor data?

- **Cached copy on LOCAL** (fast, no radio): `GET /api/lora/nodes/sensor?addr=<N>`
- **Live read over LoRa** (triggers a fresh radio request): `GET /api/lora/nodes/sensor?addr=<N>&live=true`

Replace `<N>` with the LoRa address of the REMOTE node (e.g. `3` for the first REMOTE).

### How do I read control data?

- **On LOCAL** (already in RAM, instant): `GET /api/lora/nodes/control`
- **Live read on REMOTE** (triggers a fresh radio request back to LOCAL): `GET /api/lora/nodes/control?live=true`

### What does `live=true` do?

Without `live=true` the node returns whatever value it currently has in its local memory cache.
This is fast but may be a few seconds old.

With `live=true` the node sends a LoRa radio packet to the owning node right now and waits for
a fresh reply before returning the result. Use this when you need the most up-to-date value and
can tolerate the extra round-trip time (roughly 100-200 ms).

### What does `refresh=true` do?

`refresh=true` works the same as `live=true` for the purposes of triggering an over-the-air
update. Both parameters request a fresh read from the owning node instead of the local cache.

### Why do I get an error when I add `live=true` to a request on LOCAL for control data?

LOCAL is the **owner** of control data - it created those values and holds them directly in RAM.
There is no remote node to ask for a fresher copy. Adding `live=true` here makes no sense, so
the API rejects it with HTTP 400.

**What to do instead:**

| Goal | Node to call | Endpoint |
| :--- | :--- | :--- |
| Read control setpoints | LOCAL | `GET /api/lora/nodes/control` (no `live`) |
| Read live sensor data from REMOTE node 3 | LOCAL | `GET /api/lora/nodes/sensor?addr=3&live=true` |
| Read control data as seen by REMOTE (live) | REMOTE | `GET /api/lora/nodes/control?live=true` |

### Can I set a timeout when using `live=true`?

Yes. Add `timeout=<ms>` to the request, for example `?live=true&timeout=3000` to wait up to
3 seconds for the radio reply. If the remote node does not respond in time the API returns an
error rather than stale data.

---

## 4. Resource Usage and Headroom

### What hardware does this run on?

Each node is an **ESP32-WROOM-32** module (ESP32-D0WD-V3, dual-core 240 MHz, 4 MB flash,
288 KB usable heap RAM, no PSRAM).

### How much flash does the application use?

The compiled firmware image is approximately **1,208 KB**. Each OTA partition slot is **1,280 KB**,
leaving roughly **72 KB of headroom** - about 5.6% free. This is tight. Adding large new
features, enabling more ESP-IDF components, or increasing debug log levels can easily overflow
the partition.

```
Flash partition layout (4 MB total)
────────────────────────────────────────────────────────
 nvs        20 KB   NVS key-value storage (WiFi, config)
 otadata     8 KB   OTA bookkeeping
 app0     1280 KB   Active firmware image (~1208 KB used, ~72 KB free)
 app1     1280 KB   OTA update staging slot
 spiffs   1408 KB   Reserved data partition
 coredump   64 KB   Crash dump
────────────────────────────────────────────────────────
```

What this means for adding hardware peripheral support: any code you add for GPIO, I2C, SPI,
ADC, PWM, etc. competes directly with that 72 KB headroom. A typical sensor driver adds
10-40 KB; a communication library (Modbus, CAN) can add 30-80 KB. Plan accordingly and
monitor the image size after each addition.

### How much RAM is available?

At runtime, after boot, both nodes leave roughly **130-137 KB of free heap** out of 288 KB
total. The rest is consumed by the WiFi/lwIP stack, FreeRTOS task stacks, and application
buffers.

```
Heap at runtime (typical, both nodes)
──────────────────────────────────────────────────────────
 Total usable heap              288 KB
 Used by WiFi stack + lwIP     ~100 KB  (system, not visible)
 Used by application             ~51 KB  (tasks, buffers, registry)
 Free heap                      ~130-137 KB
──────────────────────────────────────────────────────────
 Heap fragmentation               ~38%   (WARNING threshold: 30%)
 Largest contiguous block        ~82-84 KB
──────────────────────────────────────────────────────────
```

Fragmentation is at 38%, meaning the free memory is spread across many small blocks rather
than one large one. The largest single allocation that would succeed is about 82-84 KB.
ArduinoJson documents and nanopb message buffers are small and unaffected, but adding a
feature that needs a large contiguous buffer (e.g. a big HTTP response body) could fail
even though total free heap looks adequate.

### How much RAM can user code add?

Any new feature that allocates heap at runtime reduces the ~130 KB free pool. As a rough
guide:

| Addition | Approximate heap cost |
| :--- | :--- |
| One extra FreeRTOS task (4 KB stack) | ~4-6 KB |
| ArduinoJson document (sensor payload) | 1-4 KB |
| Additional device in the peer registry | ~1 KB |
| HTTP client request with response buffering | 8-20 KB |
| MQTT client (PubSubClient) | ~4-8 KB |

Keep the free heap above 50 KB at all times to leave headroom for the WiFi stack's burst
allocations during reconnect events.

### What are the FreeRTOS task stacks currently using?

Each task is allocated a fixed stack at creation time. The figures below are from live nodes
and show how many bytes remain unused at the closest point the task has ever reached to its
stack bottom (the lower the number, the tighter the margin).

| Task | Stack | Free min | Status |
| :--- | ---: | ---: | :--- |
| LoRaTransTask | 6144 B | ~3140-4196 B | OK |
| WSBroadcast | 4096 B | ~3028-3168 B | OK |
| Ping | 4096 B | ~2352-2360 B | OK |
| Push | 4096 B | ~1636-2200 B | OK |
| Beacon (REMOTE only) | 4096 B | ~1836 B | OK |
| StatusLed | 1024 B | ~496 B | WARNING - close to 400 B lower bound |

The **StatusLed** task has the tightest margin (~496 B free out of 1024 B allocated). All
others are comfortable. New code added inside an existing task reduces its free minimum.

### Is there room to add hardware peripheral support?

Yes, but with care. The practical limits are:

- **Flash:** ~72 KB free. A single new driver library can consume 10-80 KB. Check the
  image size after every library addition.
- **Heap:** ~130 KB free at runtime. Keep above 50 KB at all times. Prefer `static` buffers
  over heap allocations in task loops.
- **Task stacks:** all tasks have comfortable margins except StatusLed. Adding a new task
  needs a new stack allocation (typically 2-6 KB).
- **CPU:** dual-core at 240 MHz. The LoRa radio task owns Core 0; new tasks run on Core 1
  alongside the Arduino main task. CPU is not a bottleneck for typical sensor/control work.

The short answer: one or two simple sensor drivers or a small REST client will fit. A large
protocol stack (MQTT broker, Modbus RTU, CAN bus) may push the flash over its limit and
requires resizing the partition table.

---

## 5. Validating Your Installation with Bob Skills

### What are the Bob verification skills?

The Bob skills are automated verification scripts that run inside the Bob AI assistant. They
fetch live logs and REST responses from both nodes, check all expected log lines, cross-check
frame sequence numbers, and report a structured PASS/FAIL verdict. They are the fastest way to
confirm a fresh deployment or diagnose a suspected issue without opening a serial monitor.

They require a Bob session with network access to both nodes. They do not change any firmware
or configuration.

### When should I run them?

| Situation | Skill to run |
| :--- | :--- |
| After first deployment of both nodes | `loraiot-ping` first, then `loraiot-control` |
| After reconfiguring a node (new address, channel, or air rate) | `loraiot-ping` |
| Suspecting the LoRa radio link is broken or degraded | `loraiot-ping` |
| Verifying that a LOCAL-to-REMOTE setpoint push works end to end | `loraiot-control` |
| Verifying that a REMOTE-to-LOCAL sensor data push works end to end | `loraiot-sensor` |
| After changing LoRa air rate (`PUT /api/lora/airrate`) | `loraiot-ping` + `loraiot-control` |
| Before adding I2C, GPIO, SPI, or any hardware peripheral support to REMOTE | `loraiot-resources` |
| Suspecting a memory or flash shortage after adding new code or libraries | `loraiot-resources` |

### What does each skill verify?

**`loraiot-ping`** - verifies the radio link in both directions:
- Reboots both nodes and waits for them to come back.
- Checks LOCAL log for: PingTask started, LoRa gate open, PING sent, ACK received with RTT
  in the expected range (80-200 ms at 62.5k, ~700 ms at 2.4k).
- Checks REMOTE log for: PING received from LOCAL, ACK sent back.
- Builds a sequence number cross-check table covering every PING/ACK pair.
- Tests the REST ping endpoint (`GET /api/lora/nodes/ping?addr=<N>`).

**`loraiot-control`** - verifies LOCAL-to-REMOTE control data delivery:
- POSTs a test control device with two metrics to LOCAL.
- Checks LOCAL log for DATA_ACK sent and acknowledged.
- Reads REMOTE's control cache and confirms the values arrived correctly.
- Checks both logs for the full confirmed data frame ([`DATA_ACK`](doc/LoRaProtocol.md)) exchange.

**`loraiot-sensor`** - verifies REMOTE-to-LOCAL sensor data delivery:
- POSTs test sensor readings to REMOTE's sensor ingestion endpoint.
- Waits for PushTask to deliver them over LoRa to LOCAL.
- Reads LOCAL's sensor cache and confirms the values match.
- Checks both logs for the full frame exchange.

**`loraiot-resources`** - checks RAM, flash, and task stack headroom on both nodes:
- Fetches live diagnostics from `GET /status` on both nodes.
- Reports free heap, heap fragmentation, flash image size vs. slot size, and per-task stack
  free minimum with OK/WARNING/CRITICAL ratings.
- Particularly useful before adding hardware peripheral support to a REMOTE node: confirms
  how much flash and RAM headroom is available for a new driver or library, and flags any
  tasks whose stack margin is already tight.
- See also FAQ §4 (Resource Usage and Headroom) for typical baseline figures.

### What does a passing result look like?

Each skill produces a structured verdict table. A clean passing run looks like:

```
| Component          | Result |
|--------------------|--------|
| PING LOCAL->REMOTE | PASS   |
| PING REMOTE->LOCAL | PASS   |
| REST ping endpoint | PASS   |
```

RTT values outside the expected range, missing log lines, or HTTP errors in the REST ping
response are flagged as FAIL or PARTIAL with the exact log line evidence.

### Known transients (do not treat as failures)

- **First PING after boot times out once:** LOCAL queues a Describe request and a PING
  back-to-back immediately after the first BEACON. On a half-duplex channel these can
  collide. Self-recovers on the next BEACON cycle.
- **`delivery:after_describe` on first control push:** REMOTE rebooted and does not have
  the device definitions yet. The firmware fetches them automatically and re-pushes the
  value. Not an error.
- **Single REST ping HTTP 504:** A one-off RF collision between REMOTE's ACK and its own
  BEACON. Flag only if it reproduces on every attempt.

### How do I invoke a skill?

In a Bob session, simply ask:

> "Run the loraiot-ping skill" or "Verify that LOCAL and REMOTE communicate."

Bob will activate the skill and execute all steps automatically.
