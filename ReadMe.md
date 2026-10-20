# LoRaIoT - Multi-Floor ESP32 LoRa Gateway Bridge

## 1. Executive Summary

LoRaIoT is an enterprise-grade, half-duplex, point-to-point (P2P) RF bridge designed to transparently extend local IP networks and link remote physical layers across high-attenuation boundaries (such as reinforced concrete floors, steel-grid structures, or long distances). 

By leveraging the high-penetration physics of the 433 MHz UHF spectrum and Semtech LLCC68-based E220 transceivers, LoRaIoT bridges a vertically or horizontally isolated remote WiFi Access Point (SoftAP) directly to a local LAN and downstream Home Automation/MQTT systems. To guarantee non-blocking operations, the firmware is architected around a dual-core FreeRTOS threading model on the ESP32, integrating thread-safe queue-serialized UART2 transactions, persistent NVS state management, and an on-demand 6-step physical wiring diagnostics suite.

> Abbreviations and technical terms used in this document are explained in [Appendix A: Glossary](#appendix-a-glossary-of-abbreviations).

```
       [ Remote SoftAP Node ]                           [ Local Station Node ]
  +-------------------------------+                +-------------------------------+
  |  ESP32 WiFi AP (192.168.4.1)  |                |  ESP32 WiFi STA               |
  |  - IoT Sensor Endpoints       |                |  - Local LAN Connection       |
  |  - Payload Compressor         |                |  - Home Automation Uplink     |
  +-------------------------------+                +-------------------------------+
                 | (UART2)                                        | (UART2)
  +-------------------------------+                +-------------------------------+
  |  E220-400T22D (ADDR 0x02)     | <=== LoRa ===> |  E220-400T22D (ADDR 0x01)     |
  |  Fixed Mode (Channel 0x19)    |    (433 MHz)   |  Fixed Mode (Channel 0x19)    |
  +-------------------------------+                +-------------------------------+
```

---

## 2. System Hardware Requirements

To construct and successfully run the LoRaIoT gateway bridge, the following hardware components are strictly required:

1. **Microcontroller (2 Units):** Dual-core ESP32 (e.g. ESP32-WROOM-32 or ESP32-S3). Single-core ESP32 variants are not recommended due to the strict separation of real-time LoRa transactions on Core 1 and concurrent WiFi/Web stack routines on Core 0.
2. **LoRa Radio Module (2 Units):** Ebyte E220-400T22D (incorporating the Semtech LLCC68 RF chip operating in the 433 MHz UHF ISM band, delivering up to 22 dBm transmission power).
3. **Antennas (2 Units):** High-quality 433 MHz SMA whip antennas (such as $18\text{ cm}$ dipole whips) tuned to the 433 MHz frequency range.
4. **Decoupling Capacitor (2 Units):** A $100\mu\text{F}$ (or larger) low-ESR electrolytic capacitor connected in parallel directly across each E220's `VCC` and `GND` lines. This buffers high-current RF transmission bursts ($\approx 110\text{mA}$ peak) and prevents voltage sags that can cause ESP32 kernel brownout reboots.
5. **On-Demand Status LED (2 Units):** A standard LED connected in series with a $330\,\Omega$ current-limiting resistor, wired between ESP32 GPIO 21 and GND.

---

## 3. Hardware Design & Wiring Specification

The physical interface between the ESP32 (38-pin DevKitC) and the Ebyte E220-400T22D transceiver relies on five dedicated GPIO lines. It is critical to enforce the exact pin mappings below to maintain compatibility with both the production firmware and the hardware diagnostics engine.

### Canonical Pin Mapping

| E220 Pin | Pin Name | ESP32 GPIO | Direction (ESP32) | Functional Description |
|:---:|:---:|:---:|:---:|---|
| **1** | M0 | **GPIO 4** | Output | Mode control (Bit 0) |
| **2** | M1 | **GPIO 2** | Output | Mode control (Bit 1) |
| **3** | RXD | **GPIO 17** | Output (TX2) | UART2 transmit line to E220 RXD |
| **4** | TXD | **GPIO 16** | Input (RX2) | UART2 receive line from E220 TXD |
| **5** | AUX | **GPIO 15** | Input | Hardware status indicator (HIGH = Idle / Ready) |
| **6** | VCC | **3.3V or 5V** | Output Power | Primary module power supply (decoupled with 100uF capacitor) |
| **7** | GND | **GND** | Ground | Common reference ground |

### E220 Hardware Operating Modes

By driving the M0 and M1 pins, the ESP32 dynamically commands the E220 through different operating states:

| M1 Pin | M0 Pin | Mode | Purpose | UART Baud Rate | Description |
|:---:|:---:|:---:|---|:---:|---|
| **LOW** | **LOW** | **Mode 0** | Normal / Transceive | **115200** (Configured) | Active RF transceiving. E220 handles RF-to-UART conversion. |
| **LOW** | **HIGH** | **Mode 1** | WOR (Wake on Radio) | 115200 | Power-saving mode. Receives and transmits WOR preambles. |
| **HIGH** | **LOW** | **Mode 2** | WOR Power Save | 115200 | Puts E220 RX to sleep, waking up periodically to sense preambles. |
| **HIGH** | **HIGH** | **Mode 3** | Config / Deep Sleep | **9600** (Fixed) | Accept registers read/write commands over UART. |

---

## 4. Firmware Architecture & Threading Model

To ensure that real-time RF communication never blocks the TCP/IP stack or HTTP/WebSocket servers, LoRaIoT enforces a strict multi-threaded layout using FreeRTOS.

```
            Core 0 (System Core)                   Core 1 (Application Core)
     +---------------------------------+      +---------------------------------+
     | - WiFi Stack & LwIP             |      | - Main Arduino loop()           |
     | - ESPAsyncWebServer (Callbacks) |      | - Device Polling Task           |
     | - WebSocket Broadcast Task      |      | - loraTransportTask (Priority 2)|
     +---------------------------------+      +---------------------------------+
                      |                                       |
                      +======== [ txQueue (Serialized) ] =====+ ==> UART2 -> E220
```

### Core Assignments
- **Core 0 (System Core):** Handles the high-frequency WiFi stack, TCP/IP processing, and non-blocking asynchronous web callbacks from `ESPAsyncWebServer` (such as REST API and WebSocket events).
- **Core 1 (Application Core):** Pinpoints application-level task logic, including the primary loop, hardware polling, and the low-level **`loraTransportTask`**.

### Concurrency Rules
1. **Single UART2 Ownership:** The `loraTransportTask` running on Core 1 is the **sole owner** of the `Serial2` interface. Web server callbacks on Core 0 must never write directly to `Serial2`.
2. **The `txQueue` Bridge:** Any thread intending to transmit data must call `LoRaTransport.sendRaw()`. This method copies the packet into a `LoRaTxMessage` struct and posts it onto the FreeRTOS `txQueue`. The background `loraTransportTask` then drains the queue, executes the physical hardware handshakes, and writes the bytes atomically to UART2.
3. **Shared State Mutexes:** Any shared variables (such as link diagnostic counters or packet statistics) are protected by a FreeRTOS Mutex (`SemaphoreHandle_t`) to prevent race conditions.

---

## 5. Unified Singleton Design Pattern

The codebase is built around the **Arduino Singleton Pattern** to prevent global variable name-shadowing collisions in C++ while presenting a clean, intuitive API. Every hardware controller or configuration subsystem defines an underlying class with a `-Class` suffix and exports a single global instance with a clean name:

- **`LoRaTransportClass` $\to$ `LoRaTransport`**: Manages low-level UART2 transceiving, the FreeRTOS background task, and the `txQueue`.
- **`LoRaConfigClass` $\to$ `LoRaConfig`**: Manages persistent NVS parameters (such as `loraChannel` and `loraAddr`).
- **`GatewayClass` $\to$ `Gateway`**: Manages the persistent gateway role (`LOCAL` or `REMOTE`).
- **`LogBufferClass` $\to$ `LogBuffer`**: Manages the multi-level (`INFO`/`DEBUG`/etc.) log ring buffer in RAM and provides JSON serialization.
- **`StatusLed` $\to$ `statusLed`**: Manages the asynchronous status LED state machine.

---

## 6. Milestone 0 PoC Waterfall Lifecycle

Milestone 0 validates that both ESP32 boards and their E220 transceivers are physically wired correctly and can establish bidirectional, over-the-air communication.

```
  [ PHASE 1: BUILD ]           [ PHASE 2: DEPLOY ]          [ PHASE 3: RUN ]
+--------------------+      +----------------------+      +------------------+
| - Arduino IDE      |      | - WiFi Captive Portal|      | - Boot & Load    |
| - platformio.ini   | ===> | - Pre-select Tier    | ===> | - Fixed Routing  |
| - lib_deps check   |      | - Commit NVS Params  |      | - Ping-Pong Loop |
+--------------------+      +----------------------+      +------------------+
```

### Phase 1: Build & Compile
- **Dependencies:** The compiler expects the following libraries (downloaded automatically in PlatformIO via `lib_deps`):
  - `WiFiManager` (v2.0.17)
  - `ArduinoJson` (v7.4.3)
  - `ESPAsyncWebServer` (v3.12.1)
  - `AsyncTCP` (v3.5.0)
- **Compilation:**
  - **PlatformIO:** Run `pio run -e esp32dev` from the root directory.
  - **Arduino IDE:** Load the `LoRaIoT.ino` sketch stub, open Library Manager, install the four required dependencies, and click Compile.

### Phase 2: Deploy & Provision
1. **Flash Firmware:** Flash the compiled binary onto both ESP32 boards.
2. **WiFi Provisioning:**
   - Turn on the node. Connect your phone or laptop to the captive WiFi portal access point (e.g. `AutoConnectAP`).
   - Open your browser to `http://192.168.4.1`.
   - In the configuration form, select the node's **Gateway Tier** (`LOCAL` or `REMOTE`) and select the desired **LoRa Channel** (0-31, defaults to Channel 23 / `0x17`).
3. **Automatic Address Assignment:**
   - Selecting `LOCAL` automatically configures the node's address to `0x01` in NVS.
   - Selecting `REMOTE` automatically configures the node's address to `0x02` in NVS.
   - `0x00` is invalid and `0xFF` is the broadcast address (the E220 treats module addresses `0x0000` and `0xFFFF` as monitor/broadcast, see manual §5.3/5.4).
   - Nodes provisioned earlier keep their stored address: call `GET /reset` and provision again.

### Phase 3: Run & Communicate
1. **E220 Initialization:** On boot, the ESP32 switches UART2 to 9600 baud, pulls `M0=1`/`M1=1` (Mode 3), and writes the persistent register configuration (`C0 00 08 ...`). It then switches `M0=0`/`M1=0` (Mode 0) and re-opens UART2 at 115200 baud.
2. **Fixed Mode Routing:**
   - Both modules are configured in **Fixed Transmission Mode** (`REG3_MODE_FIXED = 0x40`).
   - When the `LOCAL` node transmits, the transport layer prepends a 3-byte routing header: `[0x00][0x02][CH]`. The E220 transmits the airframe targeting `0x02` (REMOTE).
   - The `REMOTE` node's E220 matches address `0x02`, strips the 3 routing bytes, and outputs the raw text over UART.
   - When the `REMOTE` node replies, the transport layer prepends `[0x00][0x01][CH]`. The `LOCAL` node matches address `0x01` and outputs the raw reply over UART.
   - The E220 never tells the receiver who sent a frame; the production protocol (`doc/LoRaProtocol.md`) therefore carries SRC and DST in its own frame header.
3. **Ping-Pong Execution:**
   - `LOCAL` transmits plain-text `"Local send: N\n"` (incrementing N) every 2 seconds.
   - `REMOTE` receives it, logs `Received ping: Local send: N`, and immediately replies back with `"Remote reply: N\n"`.
   - `LOCAL` receives the echo and logs `Received reply: Remote reply: N`.

---

## 7. On-Demand Diagnostics & Multi-Node Debugging

Since serial monitors can typically only focus on a single COM port at a time, LoRaIoT integrates tools to trace and isolate physical wiring faults or airframe issues over the local network.

### 🛠️ The 6-Step Hardware Diagnostics Check
If the E220 configuration write fails on boot (logging an `invalid response received` error), it points to a physical wiring or power fault. You can trigger a comprehensive, isolated hardware diagnostic test over the network at any time:

1. **Trigger the Check:** Open your terminal and trigger the diagnostics via a GET request:
   ```bash
   curl -s http://<node_ip>/api/diag/e220
   ```
   *Note: This command safely calls `LoRaTransport.pause()` to suspend the background transceiver task and close its UART2 handle, preventing resource contention.*
2. **Fetch the Diagnostics Log:**
   ```bash
   curl -s http://<node_ip>/api/log
   ```
3. **Interpret the Output:** Look at the bottom of the log lines starting with `E220 diag:`:
   - **Step 1 Fail (AUX is LOW):** The E220 has no power (check VCC/GND wires) or the AUX wire is loose/stuck.
   - **Step 2 Fail (AUX did not return HIGH):** The M0 or M1 lines are disconnected, preventing the module from transitioning modes.
   - **Step 3 Fail (Read reply (0 bytes) / FAIL no reply):** The RXD (GPIO 17) or TXD (GPIO 16) serial lines are loose, disconnected, or swapped.

---

## 8. Range & RF Optimization Rules

To guarantee successful communication across thick structures or vertically separated building floors, follow these critical physics-based adjustments:

### 1. The Parallel-Tilt Antenna Rule (Vertical Floors)
Whip antennas have a **toroidal (doughnut-shaped) radiation pattern**. They radiate maximum energy horizontally, but have **deep axial nulls (zero signal)** extending directly out of the **tip** and **base** of the whip.
- **The Problem:** If `LOCAL` (Floor 4) and `REMOTE` (Floor 0) are aligned vertically, perfectly vertical antennas (`||`) point their blind spots directly at each other, causing a massive $20\text{ dB}$ to $30\text{ dB}$ signal loss.
- **The Solution:** Tilt **both** antennas parallel to each other at a **$30^\circ$ to $45^\circ$ angle** (e.g., `//` or `\\`). This tilts the maximum radiation torus directly along the diagonal line of sight between the floors, while keeping their polarization axes perfectly aligned.

### 2. Dropping Air Data Rate for Link Budget Gains
Receiver sensitivity is mathematically coupled to Spreading Factor and bandwidth. Lowering the air data rate makes the radio receiver orders of magnitude more sensitive:
- By changing the configuration register `REG0` from **62.5 kbps** (`REG0_AIR_DATA_RATE_62K5`) to **2.4 kbps** (`REG0_AIR_DATA_RATE_2K4`), the link budget increases by **approximately $14 \text{ dB}$**.
- In RF physics, **every $6\text{ dB}$ increase in link budget doubles the penetrative capacity through concrete/steel walls or doubles the open-space range.**
- Set the data rate to `2.4 kbps` or `9.6 kbps` inside `LoRaTransport.cpp` to punch through dense concrete building cores.

---

## Appendix A: Glossary of Abbreviations

Please see [Glossary.md](./Glossary.md).
