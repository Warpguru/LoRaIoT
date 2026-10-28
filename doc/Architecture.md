# LoRaIoT - Firmware Architecture

This document is the developer reference for the LoRaIoT firmware internals.
For hardware wiring, quick-start examples, and operational guides see
[`README.md`](../README.md).

---

## 1. Threading Model

To ensure that real-time RF communication never blocks the TCP/IP stack or HTTP/WebSocket
servers, LoRaIoT enforces a strict multi-threaded layout using FreeRTOS.

```
            Core 0 (System Core)                   Core 1 (Application Core)
     +---------------------------------+      +---------------------------------+
     | - WiFi Stack & LwIP             |      | - Main Arduino loop()           |
     | - ESPAsyncWebServer (Callbacks) |      |   applicationSetup/Loop()       |
     +---------------------------------+      | - loraTransportTask (Priority 2)|
                      |                       | - WebSocketService broadcastTask|
                      |                       |   (Priority 1)                  |
                      |                       | - BeaconTask [REMOTE only]      |
                      |                       |   (Priority 1)                  |
                      |                       +---------------------------------+
                      |                                        |
                      +======= [ txQueue (Serialized) ] =======+
                                         |
                                      UART2 -> E220
```

> On single-core builds all tasks run on the only available core with no affinity pinning.
> The `TASK_CREATE` macro in `src/util/src/TaskUtils.h` handles this automatically.

### Core Assignments

- **Core 0 (System Core):** Handles the high-frequency WiFi stack, TCP/IP processing, and
  non-blocking asynchronous web callbacks from `ESPAsyncWebServer` (REST API and WebSocket
  events).
- **Core 1 (Application Core):** Runs the primary application loop, the low-level
  `loraTransportTask`, WebSocket broadcast, and (on REMOTE) the beacon task.

### Concurrency Rules

1. **Single UART2 Ownership:** The `loraTransportTask` is the **sole owner** of `LoRaSerial`
   (UART2). Web server callbacks on Core 0 must never write directly to UART2. The diagnostics
   engine borrows UART2 via `LoRaTransport.pause()` / `LoRaTransport.resume()`.
2. **The `txQueue` Bridge:** Any task intending to transmit calls `LoRaTransport.sendFrame()`
   or `LoRaTransport.sendRaw()`, which copies the packet into a `LoRaTxMessage` and posts it
   onto the FreeRTOS `txQueue`. The `loraTransportTask` drains the queue, executes the AUX
   handshake, and writes bytes atomically to UART2.
3. **LoRa Readiness Gate:** The `loraIsReady()` function (declared in `Application.h`) returns
   `true` only after the boot diagnostics have completed on the first `applicationLoop()` tick.
   Radio tasks (e.g. `BeaconTask`) must poll this before transmitting to avoid corrupting the
   E220 UART state during the diagnostics Mode 3 sequence.
4. **Shared State Mutexes:** All shared state structs (`LoRaState`, `SensorRegistry`,
   `ControlRegistry`, `LogBuffer`) are protected by FreeRTOS mutexes.

---

## 2. LoRa Message Transmission and Reception Lifecycle

The diagram below details how requests from REST callbacks or background tasks are dispatched
across FreeRTOS queues to the single UART2 driver task, how AUX hardware flow control is
enforced, and how incoming frames are parsed, validated, and routed to application handlers.
The RX path shows both the synchronous response path (used when a Caller is blocking on a
reply) and the asynchronous application routing path (used for unsolicited frames such as
BEACON, DATA_ACK, and PING).

```mermaid
sequenceDiagram
    autonumber
    participant Caller as Caller Task / REST Handler
    participant Broker as LoRaBroker
    participant TxQ as txQueue (FreeRTOS Queue)
    participant Task as loraTransportTask (Core 1)
    participant E220 as Ebyte E220 Module (UART2)
    participant RxCB as onLoRaReceive Callback
    participant App as Application / Registry

    Note over Caller, E220: Outgoing Transmission Flow TX
    Caller->>Broker: loraBrokerSend(type, dstAddr, payload)
    Broker->>Broker: encodeFrame(LoRaFrame)
    Broker->>TxQ: xQueueSend(txQueue, LoRaTxMessage)
    Broker->>Broker: Block on responseSem (if waiting for reply)

    Task->>TxQ: xQueueReceive(txQueue, msg)
    Task->>E220: Poll AUX pin (wait until AUX is HIGH)
    Task->>E220: Atomic UART2 write: ADDH + ADDL + CHAN + Payload
    E220-->>Task: AUX drops LOW (transmission in flight)
    E220-->>Task: AUX rises HIGH (buffer empty)
    Task->>Task: Wait 50 ms RF guard delay (GUARD_DELAY_MS)
    Task->>Task: Increment statsTxFrames

    Note over E220, App: Incoming Reception Flow RX
    E220->>Task: UART2 RX bytes stream in
    Task->>Task: Sync scanner matches 0xAA 0x55
    Task->>Task: Accumulate header, payload, and CRC16
    Task->>Task: decodeFrame(rxBuffer) verifies CRC and DST
    Task->>Task: Increment statsRxFrames
    Task->>RxCB: onLoRaReceive(rxFrame)

    alt Frame is a RESPONSE or ACK matching a pending Caller
        RxCB->>Broker: loraBrokerOnResponse(response)
        Broker-->>Caller: Give responseSem and unblock caller
    else Unsolicited frame (BEACON, DATA_ACK, PING, DATA)
        RxCB->>App: Route by TYPE: update LoRaState, store in SensorRegistry / ControlRegistry
        App->>TxQ: Enqueue reply if required (ACK for DATA_ACK, ACK for PING, PING reply to BEACON)
    end
```

---

## 3. Boot Initialisation Sequence

The diagram below traces the exact sequence of steps from power-on (or reset after reflash)
until the node is fully ready to send and receive LoRa frames.

```mermaid
sequenceDiagram
    autonumber
    participant HW   as Hardware / Reset
    participant App  as applicationSetup
    participant Cfg  as LoRaConfig / Gateway / LogBuffer
    participant WM   as WiFiManager captive portal
    participant E220 as E220 Module UART2
    participant Task as loraTransportTask
    participant Diag as diagnosticsLoop
    participant Radio as Radio Tasks BeaconTask

    HW->>App: Power-on / reset

    Note over App,Cfg: Phase 1 - Singletons and NVS
    App->>App: statusLed.begin() - LED enters BOOTING state
    App->>Cfg: SensorRegistry.begin() / ControlRegistry.begin()
    App->>Cfg: LogBuffer.begin() - load persisted log level from NVS
    App->>Cfg: Gateway.begin() - load tier LOCAL / REMOTE from NVS
    App->>Cfg: LoRaConfig.begin() - load channel, address, cpuFreq from NVS
    App->>App: setCpuFrequencyMhz - apply saved CPU frequency

    Note over App,WM: Phase 2 - WiFi and Provisioning
    App->>WM: wm.autoConnect - start WiFiManager
    alt Node is unprovisioned
        WM-->>App: Blocks - captive portal served on 192.168.4.1
        Note over WM: User selects Tier and Channel in browser
        WM->>Cfg: saveCallback - Gateway.save and LoRaConfig.save
    else Credentials already in NVS
        WM-->>App: Connects immediately, portal skipped
    end
    App->>App: RestService.registerRoutes - all /api/* routes wired
    App->>App: WebSocketService.begin - /ws/data handler active
    App->>App: server.begin - HTTP + WS live on port 80

    Note over App,E220: Phase 3 - E220 Hardware Init at 9600 baud Mode 3
    alt Gateway tier is configured
        App->>App: LoRaState.begin
        App->>App: LoRaTransport.setReceiveCallback
        App->>E220: LoRaTransport.begin
        E220-->>App: setMode M1=1 M0=1 - enter Config mode
        E220-->>App: Write C0 00 08 ADDH ADDL REG0 REG1 CH REG3 CRYPT_H CRYPT_L
        E220-->>App: Verify response != FF FF FF - retry up to 3x, FAULT on failure
        E220-->>App: setMode M1=0 M0=0 - Normal mode at 115200 baud
        App->>Task: TASK_CREATE loraTransportTask priority 2 - UART2 now owned
        App->>App: loraBrokerBegin - semaphore bridge initialised
        alt Tier is REMOTE
            App->>Radio: beaconTaskBegin - task created, polls loraIsReady before first TX
        end
        App->>App: statusLed setState READY
    else Tier not configured
        App->>App: statusLed setState NO_DEVICE - REST API still reachable
    end

    Note over App,Diag: Phase 4 - Boot Diagnostics and LoRa Gate
    App->>Diag: diagnosticsLoop - 6-step E220 wiring check + nanopb self-test
    Note over Diag: pause suspends loraTransportTask then resume restores it
    Diag-->>App: All steps logged to /api/log
    App->>App: loraReady = true - loraIsReady gate opens

    Note over Radio,Task: Phase 5 - Normal Operation
    Radio->>Radio: BeaconTask unblocks - loraIsReady returns true
    Radio->>Task: Enqueue BEACON frame onto txQueue
    Task->>E220: AUX handshake + atomic UART2 write
    Note over Task,E220: Node is fully operational - TX and RX active
```

---

## 4. Unified Singleton Design Pattern

The codebase is built around the **Arduino Singleton Pattern** to prevent global variable
name-shadowing collisions in C++ while presenting a clean, intuitive API. Every hardware
controller or configuration subsystem defines an underlying class with a `-Class` suffix and
exports a single global instance with a clean name:

- **`LoRaTransportClass` -> `LoRaTransport`**: Manages low-level UART2 transceiving, the
  FreeRTOS background task, and the `txQueue`.
- **`LoRaConfigClass` -> `LoRaConfig`**: Manages persistent NVS parameters (channel, address,
  CPU frequency).
- **`GatewayClass` -> `Gateway`**: Manages the persistent gateway role (`LOCAL` or `REMOTE`).
- **`LogBufferClass` -> `LogBuffer`**: Manages the multi-level (`INFO`/`DEBUG`/etc.) log ring
  buffer in RAM and provides JSON serialization.
- **`StatusLed` -> `statusLed`**: Manages the asynchronous status LED state machine.
- **`LoRaStateClass` -> `LoRaState`**: Mutex-protected per-SRC link state (duplicate
  detection, reboot detection via `boot_id`, LRU registry with 180s stale purge). Used on
  both LOCAL and REMOTE.
- **`PeerRegistry` -> `SensorRegistry`**: Manages sensor devices and metric definitions
  (owned on REMOTE, cached on LOCAL).
- **`PeerRegistry` -> `ControlRegistry`**: Manages control devices and setpoint definitions
  (owned on LOCAL, cached on REMOTE).
