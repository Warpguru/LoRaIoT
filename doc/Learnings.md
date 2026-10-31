# LoRaIoT - Learnings

Lessons from real debugging sessions. Each chapter states the symptom, the evidence, the
fix, and what is proven versus what is still an explanation. Read this before adding a new
network-facing task.

---

## 1. LOCAL receives no LoRa frames after adding the MQTT task

### 1.1 Symptom

After flashing the MQTT service on the LOCAL node (broker reachable at boot):

- `GET /api/lora/stats` returned `{"tx_frames":0,"rx_frames":0,"crc_errors":0,"ack_loss":0}`
  and stayed at zero for minutes.
- `GET /api/lora/nodes` returned `{"nodes":[]}`.
- `POST /api/lora/nodes/control?addr=3` answered `{"error":"node not found"}`, because LOCAL
  had never heard from node 3.
- REMOTE was fine: its log showed a BEACON every ~5.5 s on the correct channel, and
  `DATA pushed to 0x01`.
- The rest of LOCAL looked healthy: WiFi connected, REST API answering, MQTT connected,
  every task present in `/status` with plenty of stack, plenty of free heap.

So the radio link was fine and LOCAL was alive, but nothing arrived in the application.

### 1.2 Evidence that pointed at the boot sequence

The LOCAL log buffer (last 128 lines) still covered the whole boot. It contained
`LoRa transport layer task started`, `PingTask: started` and `MqttService: connected`,
and then only HTTP `GET` lines. It did **not** contain:

```
applicationLoop: boot diagnostics complete - LoRa radio gate opened
```

That line is written by `applicationLoop()` in `Application.cpp` after the first
`diagnosticsLoop()` returns. A node that boots normally prints it. Its absence means the
Arduino loop task never finished its first pass.

Why that matters, from the code (all verified by reading):

- `diagnosticsLoop()` runs the E220 check on the first pass (`runRequested` starts as `true`).
- `runE220Check()` calls `LoRaTransport.pause()` first (suspends `loraTransportTask`
  and closes UART2) and `LoRaTransport.resume()` at the end (reopens UART2 and resumes the
  task) - see `Diagnostics.cpp` and `LoRaTransport.cpp`.
- `loraReady` is set only after `diagnosticsLoop()` returns, and every radio task polls
  `loraIsReady()` before it touches the radio.

If the loop task stops between `pause()` and `resume()`, or never gets to run, UART2 stays
closed and the transport task stays suspended. No reboot recovers it, because the same
thing happens on the next boot.

```mermaid
sequenceDiagram
    participant Loop as Arduino loop task (prio 1)
    participant Diag as diagnosticsLoop / runE220Check
    participant Lora as loraTransportTask (prio 2)
    participant Uart as UART2 + E220
    participant Mqtt as MqttTask

    Note over Loop,Mqtt: Normal boot
    Loop->>Diag: first applicationLoop()
    Diag->>Lora: LoRaTransport.pause() - suspend
    Diag->>Uart: close UART2, run E220 wiring check
    Diag->>Uart: reopen UART2
    Diag->>Lora: LoRaTransport.resume()
    Loop->>Loop: loraReady = true, "radio gate opened"
    Lora->>Uart: read frames, onLoRaReceive()

    Note over Loop,Mqtt: Observed failure (MqttTask at priority 1, broker reachable)
    Mqtt->>Mqtt: connect to broker, client.loop()
    Loop--xDiag: never completes its first pass
    Note over Lora,Uart: stays suspended, UART2 stays closed
    Note over Loop: rx_frames = 0, no "radio gate opened" line
```

### 1.3 The fix

`MQTT_TASK_PRIORITY` was changed from `1` to `0` in `MqttService.h`, so `MqttTask` runs below
the Arduino loop task. Verified on the live LOCAL node after re-flashing:

| Check | Before | After |
|---|---|---|
| `GET /api/lora/stats` | `tx_frames` 0, `rx_frames` 0 | `tx_frames` 31, `rx_frames` 47 |
| `GET /api/lora/nodes` | empty | addr 3 online, 1 device |
| `GET /api/mqtt/status` | not connected | `connected` |
| Log | no "radio gate opened" line | BEACON and `DATA ... stored` lines flowing |

### 1.4 What is proven and what is not

Proven (observed on the hardware):

- With `MqttTask` at priority 1 and the broker up, LOCAL had `rx_frames = 0` and no
  "radio gate opened" line.
- With priority 0, the same code receives frames normally.
- The code path `pause()` -> E220 check -> `resume()` -> `loraReady` exists as described.

Not proven (an explanation, not a measurement):

- **The exact mechanism of the stall.** The working explanation is that `client.loop()` on a
  `WiFiClient` blocks inside the lwIP socket layer while holding a lock the loop task needs
  (or simply keeps the core busy), so the loop task cannot finish. This was not confirmed
  with a trace.
- FreeRTOS normally **time-slices** tasks of equal priority on the same core, so pure
  "equal priority" does not by itself explain a permanent stall. A lock held across a
  blocking call (a deadlock rather than starvation) fits the symptom just as well, and
  lowering the priority may only have changed the timing so that the window no longer
  occurs. Treat priority 0 as a verified workaround whose root cause is still open.
- The report that rebooting LOCAL with the broker switched off made the problem disappear
  is from the investigation notes. It was not re-observed in this session.

How to settle it if it ever matters: log a line before and after `LoRaTransport.pause()`
and `resume()`, log from the first `applicationLoop()` pass, and dump task states with
`uxTaskGetSystemState()` while the node is stuck. If the loop task is in the Blocked state
waiting on a mutex, it is a lock problem. If it is Ready but never scheduled, it is
scheduling.

### 1.5 Diagnosing this class of problem

Follow the symptom, not the component you just added:

1. Compare both nodes: if REMOTE transmits (`LoRa TX` lines, BEACON) and LOCAL's
   `rx_frames` is 0, the fault is on the LOCAL receive side, not in the radio link.
2. Read `GET /api/lora/stats` on both nodes. `rx_frames = 0` and `tx_frames = 0` together
   means the transport task never ran its normal loop, not that the air is quiet.
3. Read the **start** of the log, not the end. Look for the boot milestones and for lines
   that should be there but are missing (here: "radio gate opened").
4. Check `GET /status` `tasks[]` to rule out a crashed or missing task, and the stack
   free-minimum to rule out an overflow.
5. Only then look at what changed (`git diff`). Here the diff showed one new task on Core 1
   with the same priority as the Arduino loop task.

### 1.6 Rule

> A task that performs blocking network I/O (TCP sockets, `WiFiClient`, MQTT, HTTP client)
> and shares a core with the Arduino loop task must run at a **lower** priority than the
> loop task (priority 0). The boot sequence in `applicationLoop()` gates the whole LoRa
> stack; nothing may be able to delay it.

The same rule is recorded in `doc/Architecture.md` (Concurrency Rules and section 8).

---

## 2. `MQTTClient::setOptions` takes the timeout in milliseconds

Symptom: every connect attempt failed with `rc=-9` (`LWMQTT_MISSING_OR_WRONG_PACKET`) even
though the broker was reachable and the credentials were right.

Cause: the timeout constant (`MQTT_CONNECT_TIMEOUT_MS = 5000`) was divided by 1000 before
being passed to `client.setOptions(keepAlive, cleanSession, timeout)`. The library expects
milliseconds, so the effective timeout was 5 ms, shorter than a WiFi TCP round trip. The
CONNACK never arrived in time.

Fix: pass `MQTT_CONNECT_TIMEOUT_MS` unchanged.

Lesson: when a library call takes a duration, read the unit in the library header. Keep
the unit in the constant name (`..._MS`) and never convert it silently at the call site.

---

## 3. MQTT topics are case-sensitive

Symptom: the node reported `connected` and published, but `mosquitto_sub -t "LoRaIot/#"`
showed nothing.

Cause: the subscription used `LoRaIot`, the configured prefix is `LoRaIoT` (capital `T`).

Check the real prefix first:

```
curl.exe -s http://192.168.1.147/api/mqtt/config      # field mqttPrefix
mosquitto_sub -h 192.168.1.117 -u <user> -P <pass> -t "LoRaIoT/#" -v
```

Lesson: when a subscriber sees nothing, subscribe to `#` once to see whether anything
arrives at all, before suspecting the publisher.

---

## 4. Summary table

| # | Symptom | Cause | Fix | Status |
|---|---|---|---|---|
| 1 | LOCAL `rx_frames = 0`, no "radio gate opened", node 3 unknown | `MqttTask` at priority 1 on the loop task's core delayed or blocked the first `applicationLoop()` pass, so UART2 was never reopened | `MQTT_TASK_PRIORITY = 0` | Verified fix, mechanism not proven |
| 2 | MQTT `rc=-9` on every connect | Timeout passed as 5 ms instead of 5000 ms | Pass milliseconds unchanged | Verified |
| 3 | Subscriber sees no data | Topic prefix typed with wrong case | Use `LoRaIoT` | Verified |
