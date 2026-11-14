# StatusLed - Status LED and LoRa Activity LEDs

Design and implementation plan for extending the single status LED with a LoRa RX LED and a LoRa TX LED.

Table of contents:

1. Goal and scope
2. Current implementation (single status LED)
3. Analysis results
4. Target design
5. Deadline-driven `ledTask`
6. Integration points
7. Implementation steps (sub-tasks)
8. Risks, edge cases and decisions
9. Manual verification

---

## 1. Goal and scope

Each node gets three LEDs, all wired the same way (GPIO -> LED anode, cathode -> GND; the LEDs have a built-in series resistor):

| LED | Meaning |
|---|---|
| `STATUS_LED` | Node state: booting (steady ON with RX/TX), ready (OFF), warning (fast blink 250 ms ON / 250 ms OFF if unconfigured), fault (SOS). Also flashes ~100 ms on decode error. |
| `RX_LED` | Lit for ~100 ms on received LoRa frame; fast warning blink (250 ms ON / 250 ms OFF) when no peers/gateway are connected (REMOTE: LOCAL unreachable; LOCAL: 0 online remotes) |
| `TX_LED` | Lit while a LoRa frame is being sent (duration follows air rate + 50 ms guard delay) |

Constraints that apply to every step (from `AGENTS.md`):

- No new FreeRTOS task, no software timers. The existing `ledTask` is extended.
- `lora` may include `util`, never the other way round. `StatusLed` stays in `src/util/src/`.
- Braces on every `if`, named constants instead of magic numbers, prototypes in headers, no underscore-prefixed members.
- The agent never builds, flashes or calls `GET /reset`. The developer builds and verifies manually.
- Never write under `ESP/`.

---

## 2. Current implementation (single status LED)

Files: `src/util/src/StatusLed.h` and `StatusLed.cpp`. Global singleton `statusLed`.

- One LED on `STATUS_LED_PIN = 21`. The onboard LED (GPIO 2) is not usable because the USB-serial bridge flickers it.
- One permanent task `ledTask` (stack 1024 B, priority 1, `xTaskCreate` with no core affinity, registered with `TaskRegistry`).
- States (`enum class LedState`): `BOOTING` (steady on), `NO_DEVICE` (1 s on / 1 s off), `READY` (off), `FAULT` (SOS pattern).
- `setState()` stores the new state in a `volatile` member and wakes the task with `xTaskNotifyGive()`.
- `ledTask` sleeps in `ulTaskNotifyTake()`:
  - `BOOTING` / `READY`: set the pin, sleep forever (zero CPU).
  - `NO_DEVICE`: toggle, sleep 1 s (or until notified).
  - `FAULT`: `blinkSOS()` runs one full cycle with blocking `delay()` calls (about 3.5 s) and then re-checks the state.
- Callers: `Application.cpp` (`begin()`, `READY`, `NO_DEVICE`), `Diagnostics.cpp` (`NO_DEVICE`), `Server.cpp` and `LoRaTransport.cpp` (`FAULT`).

Known inaccuracies in the current code (fix while touching the files):

- The comments in `StatusLed.h` and `StatusLed.cpp` still say "GPIO 2"; the pin is 21.
- The `setState()` comment says "safe to call from ISR context". `xTaskNotifyGive()` is not ISR-safe (an ISR would need `vTaskNotifyGiveFromISR()`). Nobody calls it from an ISR; document it as task context only (any task, any core).
- `begin()` uses `xTaskCreate` on purpose (the LED task is `tskNO_AFFINITY`, see the threading model in `AGENTS.md`). Do **not** change it to `TASK_CREATE`, which would pin it to Core 1.

---

## 3. Analysis results

### 3.1 Verdict

| Condition | Verdict |
|---|---|
| Two free GPIOs without side effects | Met for esp32dev, esp32s3, esp32s2. Conditionally met for esp32c3, esp32c6, esp32h2 (the project's base pin map is already invalid there, see 3.3). |
| Small implementation effort | Met. Two files reworked (`StatusLed.h/.cpp`), about 6 lines each in `LoRaTransport.cpp` and `Application.cpp`, plus docs. |
| Minimal resource usage | Met. No new task, no timers, a few bytes of RAM, no extra wakeups while idle. |

### 3.2 Pins in use today

| GPIO | Use | Source |
|---|---|---|
| 4 | E220 M0 | `LoRaTransport.h` (`PIN_M0`) |
| 2 | E220 M1 (strapping pin on ESP32) | `LoRaTransport.h` (`PIN_M1`) |
| 15 | E220 AUX (strapping pin on ESP32) | `LoRaTransport.h` (`PIN_AUX`) |
| 16 / 17 | UART2 RX / TX to the E220 | `LoRaTransport.h` (`PIN_RX`, `PIN_TX`) |
| 21 | Status LED | `StatusLed.h` (`STATUS_LED_PIN`) |
| 1 / 3 | UART0 console | `Serial.begin` in `Application.cpp` |

`src/` contains no I2C, SPI, button, interrupt or sensor pin usage.

### 3.3 Proposed pins for the two new LEDs

| Target | RX_LED | TX_LED | Notes |
|---|---|---|---|
| esp32dev | 13 | 18 | Not strapping, not flash, not input-only, no boot glitch. GPIO 13 is the JTAG TCK pin (only relevant when debugging with JTAG). 22 / 23 are an equally safe alternative pair. |
| esp32s3 | 13 | 18 | Avoid strapping 0, 3, 45, 46, USB 19 / 20, octal PSRAM 26-37. |
| esp32s2 | 13 | 18 | Avoid strapping 0, 45, 46, USB 19 / 20. |
| esp32c3 | 6 | 7 | Tentative. Avoid strapping 2, 8, 9, USB 18 / 19, UART0 20 / 21. |
| esp32c6 | 6 | 7 | Tentative. Avoid strapping 4, 5, 8, 9, 15, USB 12 / 13, UART0 16 / 17. |
| esp32h2 | 4 | 5 | Tentative. Avoid strapping 2, 3, 8, 9, 25, USB 26 / 27. |

Important notes:

- The per-chip restrictions come from general knowledge of the Espressif datasheets, not from this repository. Verify each pin against the datasheet and against the actual board before committing the constants.
- On esp32c3, esp32c6 and esp32h2 the existing pins are already invalid (for example GPIO 21 is UART0 TX on C3, GPIO 16 / 17 are the console UART on C6, GPIO 21 does not exist on H2). That is a pre-existing problem of the untested single-core targets and independent of this feature. The status LED pin and the E220 pins for those targets are out of scope here; only the new RX / TX defaults are proposed.
- Never use GPIO 12 or 14 on ESP32 (boot level / boot-time PWM), any strapping pin, GPIO 6-11 (flash) or GPIO 34-39 (input-only).
- WROVER modules use GPIO 16 / 17 for PSRAM (pre-existing conflict with the E220 UART).
- The 2 additional LEDs RX_LED and TX_LED are supposed to match the status LED in state `BOOTING` (steady on), that is turned on after (re)booting for providing a visual function diagnostics and turned off after the `BOOTING` state. 

### 3.4 What is and is not observable (decides the LED semantics)

- **TX is observable.** `sendRawInternal` runs the whole send in one task: write, wait for AUX LOW, wait for AUX HIGH, 50 ms guard. The TX LED can therefore be on for the real send duration, which grows with lower air rates.
- **RX air time is not observable.** The ESP32 only sees a frame after the E220 has finished the radio part and outputs it on the UART. The UART runs at a fixed 115200 baud, so a full frame takes only a few ms to 17 ms, independent of the air rate. The RX LED is therefore a pulse of at least ~100 ms (`ACTIVITY_PULSE_MS`), not a "duration" indicator.
- **AUX semantics:** AUX HIGH does not mean the RF transmission is complete (`doc/Feasibility.md` chapter 3.2). Ending the TX LED after the 50 ms guard delay is the closest match to the real end of the burst.

### 3.5 Scheduling facts (why a deadline-driven task is safe)

- ESP-IDF FreeRTOS is preemptive and priority-based. A task that becomes ready runs immediately if it outranks the running task; a lower-priority task never preempts a higher one.
- Priorities in this project: `LoRaTransTask` 2 (pinned to Core 1 on dual-core builds), `StatusLed` 1 (no affinity), `WSBroadcast` / `Ping` / `Beacon` / `Push` 1, `MqttTask` 0. WiFi / lwIP system tasks and interrupts outrank all of them.
- Consequence: the LED task can never delay the LoRa task. At most it waits for it. Calling `xTaskNotifyGive()` from the LoRa task is short and non-blocking and does not cause a context switch away from the (higher-priority) LoRa task.
- The LED feature therefore adds no new source of packet loss.
- Timing facts assume the Arduino-ESP32 default tick of 1000 Hz (1 tick = 1 ms). The repository does not configure the tick rate or any power management.

### 3.6 Why not a fixed 10 / 20 / 50 ms tick

A fixed tick works but is worse than a deadline-driven sleep:

| Option | Idle wakeups | Resolution |
|---|---|---|
| Fixed tick 10 ms | 100 per second, permanently | 10 ms |
| Fixed tick 20 ms | 50 per second | 20 ms |
| Fixed tick 50 ms | 20 per second | 50 ms (blinks of 150 ms look uneven; TX needs an event timestamp, not a sampled level) |
| Deadline-driven sleep (chosen) | 0 | exact |

Today the READY state has zero wakeups. `LoRaTransTask` already wakes every 10 ms, so a 10 ms LED tick would be a second permanent wake source. The estimated cost of a wakeup is a few microseconds (an estimate, not a measurement).

### 3.7 Resource estimate

| Item | Estimate |
|---|---|
| RAM | A few dozen bytes: three atomic states, three atomic pulse counters, and the per-LED runtime array (about 25 B per LED, estimated), plus the +512 B stack increase below |
| Flash | Below 1 KB (the SOS table replaces `blinkSOS`) |
| New tasks / timers | None |
| Stack | `FAQ.md` (task stack table) reports only about 496 B free of the 1024 B stack for `StatusLed` today, the tightest margin of all tasks. The rework adds per-LED runtime data (about 25 B per LED, estimated) and a few more locals, so the runtime data must not live on the task stack: keep it in a static or member array (`LED_COUNT` entries) and raise `LED_TASK_STACK_SIZE` to 1536 B as a precaution (about 512 B more heap, an estimate). Verify the high-water mark via `GET /status` after the rework and adjust. Do not call `Log_*` inside `ledTask` (formatting needs several hundred bytes of stack). |
| Mutexes | None. All shared state is a single atomic value (state or pulse counter), so concurrent callers need no lock. The LoRa path gets no new mutex and no blocking call. |
| E220 timing | The LED calls are an atomic store plus a notify, a few microseconds, placed outside the AUX handling loops. |

---

## 4. Target design

### 4.1 Which LED: `enum class Led`

`Led` is an enum class, not a class with members. It only identifies the LED, and each LED has its own pin constant.

```cpp
enum class Led : uint8_t {
  STATUS_LED,   ///< Node state LED (exists today)
  RX_LED,       ///< LoRa frame received
  TX_LED,       ///< LoRa frame being sent
  COUNT         ///< Number of LEDs; not a valid LED
};
static constexpr size_t LED_COUNT = static_cast<size_t>(Led::COUNT);
```

Pin constants (per target via `CONFIG_IDF_TARGET_*`, overridable with `-D` flags using `#ifndef`):

```cpp
static constexpr int STATUS_LED_PIN = 21;            // unchanged
static constexpr int RX_LED_PIN     = 13;            // from the table in 3.3
static constexpr int TX_LED_PIN     = 18;            // from the table in 3.3
static constexpr uint32_t ACTIVITY_PULSE_MS = 100;   // hold time of one RX pulse
```

### 4.2 Which behaviour: `LedState` with `OFF` and `ON`

I agree that `OFF` and `ON` make sense. The RX and TX LEDs need plain steady states, and the status LED today only has them hidden inside `BOOTING` and `READY`.

```cpp
enum class LedState : uint8_t {
  OFF,         ///< Steady off
  BOOTING,     ///< Steady on (status LED: held from begin() until server and devices are ready)
  READY,       ///< Steady off (status LED: server up and communicating normally)
  WARNING,     ///< Fast warning blink (250 ms on / 250 ms off)
  FAULT,       ///< SOS pattern, repeats until the state changes
  ON           ///< Steady on
};
```

- `BOOTING` behaves exactly like `ON`, and `READY` exactly like `OFF`. They stay as separate enumerators so existing call sites and logs keep their meaning. `ledTask` handles them with shared `case` labels.
- Every state is valid for every LED (generic engine). In practice RX and TX only use `OFF` / `ON` plus `pulse()`.

### 4.3 API

All methods are task context only (any task, any core), never ISR.

```cpp
class StatusLed {
 public:
  /** Configures all three pins, all LEDs on (BOOTING visual diagnostics), starts ledTask. Call once from applicationSetup(). */
  void begin();

  /** Sets the state of one LED and wakes ledTask immediately. */
  void setState(Led led, LedState state);

  /**
   * Lights the LED for ACTIVITY_PULSE_MS on top of its current pattern (the output is pattern OR pulse).
   * A new pulse while one is running restarts the hold time. Works for every LED, including STATUS_LED.
   */
  void pulse(Led led);

 private:
  static void ledTask(void* param);

  struct LedChannel {
    std::atomic<LedState> desiredState{LedState::OFF};
    std::atomic<uint32_t> pulseTriggerCount{0};
  };
  LedChannel channels[LED_COUNT];
  TaskHandle_t taskHandle = nullptr;
};
```

Rules:

- There is no one-argument `setState(LedState)`: it is removed, and every caller passes the `Led` explicitly (the existing five call sites are migrated in Step 2).
- `setState()` returns without waking the task when the state does not change (same as today).
- `pulse()` only increments a counter and notifies. `ledTask` keeps the actual hold deadline in its own local data, so the wrap-around of `millis()` never matters for shared state.
- `ledTask` is the sole owner of all three pins. Nobody else calls `pinMode` / `digitalWrite` on them.
- The class keeps its name `StatusLed` and the global `statusLed`, even though it now drives three LEDs.

---

## 5. Deadline-driven `ledTask`

### 5.1 Principle

The task never polls on a fixed period. After every wake it:

1. reads the state and pulse counter of each LED,
2. applies every change that is due,
3. computes the earliest upcoming deadline over all three LEDs,
4. sleeps until that deadline, or forever if there is none.

Early wakes are normal: `xTaskNotifyGive()` (from `setState()` or `pulse()`) makes the sleeping `ulTaskNotifyTake()` return immediately, and the task simply starts again at step 1 and recomputes the next sleep time. A notification that arrives between step 1 and the sleep is not lost, because the pending notification makes the next `ulTaskNotifyTake()` return at once. Several notifications coalesce into one wake, which is fine because the task re-reads all state each time.

### 5.2 Per-LED runtime data (private to `ledTask`)

Held in a static or member array of `LED_COUNT` entries (not on the task stack, see 3.7), touched only by `ledTask`. For each LED:

- `activePatternState`: the state the current pattern was built for (detects a state change).
- `patternPinLevel`: current physical GPIO level dictated by the pattern.
- `sosStepIndex`: index into the SOS table (FAULT only).
- `nextPatternToggleTimeMs`: `millis()` timestamp of the next pattern edge, valid only when `hasPatternTimer` is set.
- `lastHandledPulseTrigger`: last `pulseTriggerCount` value that was handled.
- `pulseUntilTimeMs` and `isPulseLit`: end timestamp and active flag of the current pulse hold.

### 5.3 Pattern rules

| State | Output | Pattern deadline |
|---|---|---|
| `OFF`, `READY` | low | none |
| `ON`, `BOOTING` | high | none |
| `WARNING` | starts high, toggles every `WARNING_TOGGLE_MS` (250 ms) | every 250 ms |
| `FAULT` | SOS table (below) | duration of the current table step |

The final pin level is `patternPinLevel OR isPulseLit`.

`WARNING` starts with the LED on for the first 250 ms.

SOS as a constant table of step durations in ms. Even index = on, odd index = off. It reproduces the timing of today's `blinkSOS` exactly, built from the existing constants:

```
S: on DOT, off GAP, on DOT, off GAP, on DOT, off GAP + WORD_GAP
O: on DASH, off GAP, on DASH, off GAP, on DASH, off GAP + WORD_GAP
S: on DOT, off GAP, on DOT, off GAP, on DOT, off GAP + CYCLE_GAP
```

(DOT = `SOS_DOT_DURATION_MS`, DASH = `SOS_DASH_DURATION_MS`, GAP = `SOS_SYMBOL_GAP_MS`, WORD_GAP = `SOS_WORD_GAP_MS`, CYCLE_GAP = `SOS_CYCLE_GAP_MS`.) After the last entry the index wraps to 0. This replaces `blinkSOS()` and removes all blocking `delay()` calls, so activity pulses keep working while the status LED shows FAULT.

### 5.4 Pulse rules

- When `pulseTriggerCount` differs from `lastHandledPulseTrigger`: set `lastHandledPulseTrigger`, `isPulseLit = true`, `pulseUntilTimeMs = now + ACTIVITY_PULSE_MS` (restarts the hold if one was running).
- When `isPulseLit` and the deadline has passed: `isPulseLit = false`.

### 5.5 Sleep time

- Candidates: the pattern deadline of each LED that has one, and the pulse deadline of each LED with an active pulse.
- Time until a deadline: `(int32_t)(deadlineMs - nowMs)`. Always compare with this wrap-safe form, never with `>=` on raw `millis()` values.
- No candidates: `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`.
- Otherwise: `ulTaskNotifyTake(pdTRUE, ticks)` with `ticks = pdMS_TO_TICKS(remainingMs)` clamped to at least 1 tick so the task never spins.
- Deadlines come from `millis()`, never from counting wakes, so a late wake cannot accumulate error.
- When `pdMS_TO_TICKS(remainingMs)` is 0 because the deadline is already due, the loop runs again instead of sleeping.

### 5.6 Resulting wake behaviour

| Situation | Wakes |
|---|---|
| All LEDs steady or idle | none |
| `WARNING` | every 250 ms |
| `FAULT` | at each SOS edge (150 ms to 2150 ms apart) |
| RX pulse | at the notification, then once at the end of the hold |
| TX send | once at the start, once at the end |

---

## 6. Integration points

| Where | What |
|---|---|
| `LoRaTransport.cpp`, `loraTransportTask`, `OK` branch of the decode result, before `receiveCallback` | `statusLed.pulse(Led::RX_LED)` |
| `LoRaTransport.cpp`, `loraTransportTask`, decode result is a failure (`BAD_CRC`, `BAD_VER`, `BAD_TYPE`, `BAD_LEN`, `BAD_SYNC`; not `FOREIGN_DST`, which is a valid frame for another node) | `statusLed.pulse(Led::RX_LED)` and `statusLed.pulse(Led::STATUS_LED)`. The status LED flashes for ~100 ms on top of its current pattern. |
| `LoRaTransport.cpp`, `sendRawInternal`, after the AUX-idle wait and the early return, immediately before the first `LoRaSerial.write` | `statusLed.setState(Led::TX_LED, LedState::ON)` |
| `LoRaTransport.cpp`, `sendRawInternal`, after the 50 ms guard delay (last statement) | `statusLed.setState(Led::TX_LED, LedState::OFF)` |
| `LoRaTransport.cpp`, `pause()` | `statusLed.setState(Led::TX_LED, LedState::OFF)`, because `vTaskSuspend` can stop the LoRa task in the middle of a send and the LED would stay on |
| `Application.cpp`, `applicationSetup()` | No change to the `statusLed.begin()` call: it already runs first and now sets up all three pins |
| `Application.cpp` (2 calls: `READY`, `WARNING`), `Diagnostics.cpp` (1 call: `WARNING`), `Server.cpp` (1 call: `FAULT`), `LoRaTransport.cpp` (1 call: `FAULT`), `BeaconTask.cpp` (2 calls: `WARNING`, `OFF` on RX_LED) | Explicit `statusLed.setState(Led, LedState)` |

Notes:

- `LoRaTransport.cpp` already includes `StatusLed.h`; the dependency direction (`lora` -> `util`) stays correct.
- `sendRawInternal` has no early return after the first write; the AUX loops end in `break`. There is exactly one ON and one OFF per send, and the only early return (AUX timeout before the write) comes before the ON.
- The TX LED on-time = write time + AUX handshake + 50 ms guard. At low air rates this can reach `AUX_TRANSMIT_COMPLETE_TIMEOUT_MS` plus the guard.
- No `platformio.ini` or `LoRaIoT.ino` change is needed if the pins are selected with `CONFIG_IDF_TARGET_*`. Verify that the macros are available in both PlatformIO and Arduino IDE builds (expected, because the Arduino-ESP32 core defines them).
- Frames sent or received while Diagnostics mode owns UART2 do not pulse the LEDs, and neither does the E220 reconfiguration.
- The status LED flash for failed frames is only visible in `READY` (off), `WARNING` (during an off phase) and `FAULT` (during an off segment). In `BOOTING` the LED is on anyway. Decode failures are also logged at DEBUG inside `decodeFrame()`.
- A frame that stalls half-received (`RX_FRAME_TIMEOUT_MS`) is not a decode result and does not flash the LED.

---

## 7. Implementation steps (sub-tasks)

Process each step on its own and wait for the developer to check the result before starting the next. After each step, update its status here.

### Step 1: Pin constants and `Led` / `LedState` types

- **Intent:** Introduce the types and pin constants without changing behaviour.
- **Expected outcome:** `StatusLed.h` declares `Led`, `LED_COUNT`, the extended `LedState`, the three pin constants with per-target `#if` selection, and `ACTIVITY_PULSE_MS`. Existing code still compiles unchanged (the old `setState(LedState)` is removed only in Step 2).
- **Todo list:**
  1. Add `enum class Led` and `LED_COUNT`.
  2. Append `OFF` and `ON` to `LedState`.
  3. Add `RX_LED_PIN` and `TX_LED_PIN` selected by `CONFIG_IDF_TARGET_ESP32`, `_ESP32S3`, `_ESP32S2`, `_ESP32C3`, `_ESP32C6`, `_ESP32H2`, each wrapped in `#ifndef` so `-D` flags can override them.
  4. Add `ACTIVITY_PULSE_MS`.
  5. Correct the stale "GPIO 2" and "ISR safe" comments.
- **Relevant context:** Chapters 3.3 and 4.1 and 4.2. The pin values must be confirmed against the datasheets (3.3).
- **Status:** `[x] done`

### Step 2: Rework `ledTask` into the deadline-driven engine

- **Intent:** Replace the blocking pattern code with the deadline loop for all three LEDs, and move the API to the explicit `Led` parameter.
- **Expected outcome:** `StatusLed.cpp` contains the SOS step table, the per-LED runtime handling, the sleep calculation and no `blinkSOS()` / blocking `delay()`. `setState(LedState)` no longer exists and all callers compile with `setState(Led, LedState)`. The status LED shows the same patterns as before (except the documented `NO_DEVICE` start level).
- **Todo list:**
  1. Replace the `volatile LedState` with the per-LED atomic channels and update `begin()` to configure all three pins and start with all three LEDs ON (`BOOTING` visual diagnostics), transitioned off when boot finishes.
  2. Implement `setState(Led, LedState)` (remove the one-argument version); keep the "no change, no wake" check.
  3. Implement `pulse(Led)`.
  4. Implement the loop of chapter 5 (pattern rules, pulse rules, wrap-safe deadlines, tick clamp).
  5. Keep `xTaskCreate` with no affinity and the `TaskRegistry.add` call; do not use `Log_*` inside the task. Keep the per-LED runtime data out of the task stack and raise `LED_TASK_STACK_SIZE` to 1536 B (see 3.7).
  6. Migrate the five existing call sites listed in chapter 6 (`Application.cpp` x2, `Diagnostics.cpp`, `Server.cpp`, `LoRaTransport.cpp`) to `setState(Led::STATUS_LED, ...)`.
- **Relevant context:** `src/util/src/StatusLed.cpp`, chapter 5, `doc/Architecture.md` (statusLed sequence), `AGENTS.md` task conventions.
- **Status:** `[x] done`

### Step 3: LoRa hooks

- **Intent:** Drive the RX and TX LEDs from the transport.
- **Expected outcome:** RX pulses on each received frame, the status LED flashes on each failed frame, TX is on exactly for the duration of each send, and the TX LED can never stay on after `pause()`.
- **Todo list:**
  1. Add `pulse(Led::RX_LED)` in the `OK` branch of the decode result.
  2. Add `pulse(Led::RX_LED)` and `pulse(Led::STATUS_LED)` for the failed results (all except `OK` and `FOREIGN_DST`).
  3. Add the TX `ON` / `OFF` pair in `sendRawInternal`.
  4. Add the TX `OFF` in `pause()`.
- **Relevant context:** `src/lora/src/LoRaTransport.cpp`, chapter 6.
- **Status:** `[x] done`

### Step 4: Documentation

- **Intent:** Keep wiring and architecture docs consistent with the hardware.
- **Expected outcome:** All documents that mention the status LED describe three LEDs per node.
- **Todo list:**
  1. `ReadMe.md` chapter 2 item 5: three LEDs per node (the "2 Units" there refers to the two nodes), and the wiring chapter with the new pins.
  2. `doc/Feasibility.md` chapter 3.1: note the additional GPIOs next to the E220 pin table.
  3. `doc/Architecture.md`: LED section and the `statusLed.begin()` sequence note.
  4. `doc/Glossary.md`: LED entry.
  5. `AGENTS.md`: change the WiFi failure line to `statusLed.setState(Led::STATUS_LED, LedState::FAULT)` and add a short note that `StatusLed` owns all three LED pins.
  6. `FAQ.md`: update the StatusLed row of the task stack table (new stack size, new measured free minimum) and the text below it.
  7. The wiring picture `doc/images/LoRaIoT.png` has to be redrawn by the developer.
- **Relevant context:** `ReadMe.md` lines about the LED hardware list and pin table, `doc/Glossary.md`.
- **Status:** `[x] done`

### Step 5: Developer verification

- **Intent:** Confirm behaviour on hardware (the agent never builds or flashes).
- **Expected outcome:** All checks in chapter 9 pass.
- **Todo list:** Developer builds, flashes and runs the checks in chapter 9.
- **Relevant context:** Chapter 9, `GET /status` for the stack high-water mark.
- **Status:** `[x] done`

---

## 8. Risks, edge cases and decisions

Risks and edge cases:

- **Bursts and retries:** every retry goes through the TX queue to `sendRawInternal` and gets its own ON / OFF. At high air rates, sends about 60 to 80 ms apart look like a nearly steady LED, which reads as "busy".
- **Stuck TX LED:** covered by the `pause()` hook and by the single ON / OFF pair that has no early return in between.
- **Rework risk:** `ledTask` is a working class; the developer must verify all status patterns by eye, because there are no automated tests.
- **Stack margin:** the current `StatusLed` stack is the tightest of all tasks (about 496 B free of 1024 B per `FAQ.md`). A stack overflow here would be hard to diagnose; check `GET /status` after the rework. `FAQ.md` also needs its StatusLed row updated once the new stack size and measurement are known (Step 4).
- **Pins on C3 / C6 / H2:** tentative, see 3.3. A wrong pick can interfere with flash, USB or boot.
- **Timing:** a pulse that starts exactly as the previous hold ends can be shortened by microseconds. Harmless.
- **Dual-core:** distinct pins are independent; all shared state is atomic.
- **Tick rate:** if the tick rate is not 1000 Hz, the minimum sleep granularity changes accordingly.

Decisions (confirmed by the developer):

1. The one-argument `setState(LedState)` is removed. All callers use `setState(Led, LedState)`.
2. The RX LED also pulses for frames that fail to decode.
3. Failed frames are signalled on the existing `STATUS_LED` (red on the developer's breadboard) as a ~100 ms flash. The RX trigger point stays "after a complete frame". The exact set of results that count as failed (all except `OK` and `FOREIGN_DST`) is a proposal; adjust it in Step 3 if only `BAD_CRC` should flash.
4. The TX LED goes off after the 50 ms guard delay (not already when AUX returns HIGH).
5. The class keeps the name `StatusLed`.
6. Target pin constants for `STATUS_LED_PIN`, `RX_LED_PIN`, and `TX_LED_PIN` are defined in `StatusLed.h` with `#ifndef` wrappers for build flag override support, using target defaults (ESP32/S2/S3: RX=13, TX=18; C3/C6: RX=6, TX=7; H2: RX=4, TX=5; STATUS=21).
7. `NO_DEVICE` was replaced with `WARNING` (fast 250 ms ON / 250 ms OFF blink). It is used on `STATUS_LED` for unconfigured gateway tier or hardware diagnostics failure, and on `RX_LED` symmetrically when no remote/gateway peers are connected (REMOTE: LOCAL unreachable; LOCAL: zero online remotes).
8. All 3 LEDs light up simultaneously during `BOOTING` for visual functional diagnostics and are turned off once startup completes.

---

## 9. Manual verification

The developer runs these after building:

1. Boot: all 3 LEDs are on during boot (~1 s visual self-test), then off when ready; on unconfigured node the Status LED blinks 250 ms / 250 ms (`WARNING`).
2. Fault: provoke a fault (for example a WiFi failure) and compare the SOS pattern with the old timing (about 3.5 s per cycle).
3. RX: send a frame from the other node and check that the RX LED lights for about 100 ms; several frames in a row keep it lit.
3a. Failed frame: provoke a decode failure and check that the RX LED lights and the status LED flashes for about 100 ms.
3b. Link loss / isolation: when LOCAL is unpowered, REMOTE's RX LED blinks fast (`WARNING` 250 ms / 250 ms) while TX LED pulses every 5 s with discovery beacons. When LOCAL is powered on without any remotes (or when all remotes go offline), LOCAL's RX LED blinks fast (`WARNING`). As soon as communication is established between them, both nodes switch their RX LEDs to OFF and pulse on receptions.
4. TX: the TX LED is lit while sending, and its on-time grows when the air rate is lowered (`PUT /api/lora/airrate`, change REMOTE first, then LOCAL, see `doc/RestApi.md`).
5. During a fault, send and receive: the activity LEDs still react.
6. Run `GET /status` on both nodes (`http://192.168.1.147`, `http://192.168.1.174`): the `StatusLed` stack high-water mark leaves a comfortable margin, and no unexpected packet loss appears in `GET /api/lora/stats`.
7. Never use `GET /reset` for any of these tests; use `GET /reboot` to restart a node.
