# Glossary of Abbreviations

Terms are grouped by topic. Project-specific terms (`LOCAL`, `REMOTE`, ...) are included because they appear throughout the code and documentation. For the over-the-air protocol terms (frame fields, message types) see also [`doc/LoRaProtocol.md`](doc/LoRaProtocol.md).

## Hardware and Radio

| Term | Meaning | Explanation |
|---|---|---|
| **MCU** | Microcontroller Unit | A small computer on a single chip that runs the firmware. Here: the ESP32. |
| **ESP32** | (product name, Espressif) | The MCU used on both nodes. Has WiFi, two CPU cores (Core 0 and Core 1) and several UARTs. |
| **GPIO** | General Purpose Input/Output | A configurable pin of the MCU, used as digital input or output (for example M0, M1, AUX, the status LED). |
| **UART** | Universal Asynchronous Receiver/Transmitter | Serial interface with a TX and an RX line. UART2 (`Serial2`, pins 16/17) connects the ESP32 to the E220. |
| **TX / RX** | Transmit / Receive | Data direction. The ESP32 TX line is connected to the E220 RXD pin, and vice versa. |
| **TTL** | Transistor-Transistor Logic | The voltage level of the serial lines (3.3 V here, not RS-232). |
| **baud** | symbols per second | UART speed. Here 9600 (E220 configuration mode) or 115200 (normal operation). |
| **8N1** | 8 data bits, No parity, 1 stop bit | The serial frame format used on the UART. |
| **LoRa** | Long Range | A radio modulation (chirp spread spectrum) for long range at low data rates. Not the same as LoRaWAN. |
| **LoRaWAN** | LoRa Wide Area Network | A network protocol stack on top of LoRa. Not used: the E220 is a transparent UART module. |
| **E220** | Ebyte E220-400T22D | The LoRa radio module (433 MHz, 22 dBm). Contains the LLCC68 chip. |
| **LLCC68** | (chip name, Semtech) | The LoRa transceiver chip inside the E220. |
| **RF** | Radio Frequency | Everything related to the radio signal. |
| **UHF** | Ultra High Frequency | The 300 MHz to 3 GHz band. 433 MHz penetrates walls and floors better than 2.4 GHz. |
| **ISM** | Industrial, Scientific and Medical band | License-free frequency band (433 MHz in Europe, with duty-cycle and power rules). |
| **SMA** | SubMiniature version A | The RF connector type of the antenna. |
| **ESR** | Equivalent Series Resistance | Internal resistance of a capacitor. A low-ESR capacitor can deliver the short current peaks of an RF transmission. |
| **VCC / GND** | Supply voltage / Ground | Power pins of the module. |
| **LED** | Light Emitting Diode | The status LED (GPIO 21). |
| **dB** | decibel | Relative power ratio. +6 dB roughly doubles the range in free space. |
| **dBm** | decibel milliwatt | Absolute power level. 22 dBm is about 160 mW. |
| **kbps** | kilobits per second | Data rate, for example the air data rate of 62.5 kbps. |
| **MHz** | Megahertz | Frequency, for example 433.125 MHz. |
| **Air data rate** | | The data rate on the radio link. Lower rate = more sensitivity and range, but longer transmission time. |
| **Link budget** | | Total gain and loss between transmitter and receiver. A higher link budget allows more wall attenuation or distance. |
| **Spreading factor** | | LoRa parameter that trades data rate for sensitivity. Selected indirectly by the air data rate. |
| **RSSI** | Received Signal Strength Indicator | Measure of the received signal level. The E220 can append it to each received packet (disabled here). |
| **P2P** | Point-to-Point | A direct link between two radios, without a gateway network. |
| **Half-duplex** | | A radio can either send or receive at a given time, not both. |

## E220 Module Terms

| Term | Meaning | Explanation |
|---|---|---|
| **M0 / M1** | Mode pins 0 and 1 | Two inputs that select the E220 operating mode (Mode 0 to 3). |
| **AUX** | Auxiliary status pin | Output of the E220: HIGH = idle/ready, LOW = busy. Must be checked before every write. HIGH does not mean that the last radio packet has left the antenna. |
| **Mode 0..3** | Operating modes | 0 = normal transceive, 1 = WOR transmit, 2 = WOR receive, 3 = configuration / deep sleep (9600 8N1 only). |
| **WOR** | Wake on Radio | Power-saving mode: the receiver sleeps and wakes up periodically to listen for a preamble. |
| **LBT** | Listen Before Talk | The module checks that the channel is free before sending. Disabled here (it can add up to 2 s delay). |
| **REG0..REG3** | Configuration registers 0 to 3 | REG0: UART baud, parity, air data rate. REG1: sub-packet size, power. REG2: channel. REG3: RSSI byte, transmission method, LBT, WOR cycle. |
| **ADDH / ADDL** | Address High / Low byte | The 16-bit module address (ADDH is the high byte, ADDL the low byte). This project always uses `ADDH = 0x00`, so the node address is ADDL. |
| **CH** | Channel | Frequency channel, 0 to 83. Frequency = 410.125 MHz + CH x 1 MHz. The default `0x17` (23) is 433.125 MHz. |
| **CRYPT_H / CRYPT_L** | Encryption key high / low byte | 16-bit key that scrambles the radio signal. Write-only; all nodes must use the same key. |
| **Sub-packet** | | Maximum number of bytes the E220 sends in one radio packet (200 B here). Larger writes are split. |
| **Fixed transmission** | REG3 bit 6 = 1 | The first 3 bytes written to the UART are `[ADDH][ADDL][CH]` and select the receiver. The receiver does not learn the sender address. |
| **Transparent mode** | REG3 bit 6 = 0 | Everything written to the UART is sent to all modules with matching address and channel. Not used: the firmware uses fixed transmission. |
| **Broadcast address** | `0xFFFF` | A frame sent to it is received by all modules on the channel. |
| **Monitor address** | `0x0000` / `0xFFFF` as own address | The module receives all frames on the channel without address filtering. |

## Firmware and Operating System

| Term | Meaning | Explanation |
|---|---|---|
| **FreeRTOS** | Free Real-Time Operating System | The scheduler of the ESP32. Runs the tasks (threads) of the firmware. |
| **Task** | | A FreeRTOS thread, for example `loraTransportTask`. |
| **Core 0 / Core 1** | CPU cores | Core 0 runs WiFi and the web server callbacks. Core 1 runs the application and LoRa tasks. |
| **Queue** | FreeRTOS queue | Thread-safe message buffer. `txQueue` carries outgoing LoRa data to the single task that owns the UART. |
| **Mutex** | Mutual Exclusion | Lock that protects data shared between tasks. `volatile` alone is not enough on a dual-core ESP32. |
| **NVS** | Non-Volatile Storage | Flash-based key/value store (`Preferences` API). Keeps tier, channel, address and log level across reboots. |
| **Brownout** | | The supply voltage drops below the safe limit and the MCU resets. The decoupling capacitor prevents it. |
| **PROGMEM** | Program Memory | Flash storage for constant data (for example the HTML page), so it does not use RAM. |
| **Singleton** | | A class with exactly one global instance, for example `LoRaTransportClass` -> `LoRaTransport`. |
| **IDE** | Integrated Development Environment | Arduino IDE or PlatformIO, used to build and flash the firmware. |
| **PlatformIO / `pio`** | | Command-line build system configured by `platformio.ini`. |
| **`lib_deps`** | Library dependencies | The libraries listed in `platformio.ini` that PlatformIO downloads automatically. |
| **DevKitC** | Development Kit C | The 38-pin ESP32 development board. |

## Network and Software Interfaces

| Term | Meaning | Explanation |
|---|---|---|
| **WiFi AP / SoftAP** | (Soft) Access Point | The ESP32 acts as a WiFi base station. The REMOTE node does this for the IoT devices (`192.168.4.1`). |
| **WiFi STA** | Station | The ESP32 acts as a WiFi client of an existing network. The LOCAL node does this. |
| **Captive portal** | | Configuration web page that opens when connecting to the node's setup network (WiFiManager). |
| **LAN** | Local Area Network | The local network (WLAN) in which the LOCAL node is connected. |
| **WLAN** | Wireless LAN | A LAN over WiFi. |
| **IP / TCP / UDP** | Internet Protocol / Transmission Control Protocol / User Datagram Protocol | Standard network protocols. They are not forwarded over LoRa; the nodes terminate them and translate the content. |
| **MTU** | Maximum Transmission Unit | Largest packet size of a link. |
| **HTTP** | Hypertext Transfer Protocol | The protocol of the web server and REST API. |
| **REST** | Representational State Transfer | Style of web API, used under `/api/*`. Can be called with a browser or `curl`. |
| **API** | Application Programming Interface | Here: the HTTP endpoints of the nodes. |
| **JSON** | JavaScript Object Notation | Text data format used on the REST API and internally on the nodes. |
| **WebSocket** | | Persistent browser connection used for the 1-second live updates. |
| **OpenAPI** | | Machine-readable description of the REST API (`openapi_json.h`, served at `GET /openapi`). |
| **MQTT** | Message Queuing Telemetry Transport | Publish/subscribe protocol commonly used by home-automation systems. |
| **`curl`** | Client URL | Command-line tool to call a REST endpoint. |
| **DNS / NTP** | Domain Name System / Network Time Protocol | Name resolution and time synchronisation; mentioned in the feasibility study. |

## Protocol Terms (see `doc/LoRaProtocol.md`)

| Term | Meaning | Explanation |
|---|---|---|
| **LOCAL** | Gateway tier | The node on the WLAN. It serves the REST API and talks to the REMOTE nodes. Address `0x01`. |
| **REMOTE** | Gateway tier | A node near the sensors. It offers a WiFi access point and talks to the LOCAL node over LoRa. Addresses `0x02` to `0xFE`. |
| **Tier** | Gateway role | The persisted choice `LOCAL` or `REMOTE`. |
| **Frame** | | One message on the air: header, payload and CRC. One message is always exactly one frame. |
| **Payload** | | The user data inside a frame (a protobuf message). |
| **SYNC** | Synchronisation bytes | `0xAA 0x55` at the start of each frame. |
| **VER** | Protocol version | One byte in the frame header. |
| **SRC / DST** | Source / Destination address | Sender and receiver address in the frame header. Needed because the E220 never delivers the sender address. |
| **SEQ / SeqNo** | Sequence number | 16-bit counter per sender that detects duplicates and lost frames. |
| **LEN** | Payload length | Number of payload bytes. |
| **CRC16** | Cyclic Redundancy Check, 16 bit | Checksum that detects corrupted frames. Variant CRC-16/CCITT-FALSE (polynomial `0x1021`, start value `0xFFFF`). |
| **ACK** | Acknowledgement | Reply that confirms the reception of a message. |
| **DATA / DATA_ACK** | Message types | Data without / with required acknowledgement. |
| **REQUEST / RESPONSE** | Message types | A command or query and its answer (for example "read sensor A"). The RESPONSE is also the acknowledgement. |
| **BEACON** | Message type | "I'm here" message that a REMOTE sends to the LOCAL node, without reply. Carries `boot_id`, firmware version, uptime and `registry_rev`. |
| **PING** | Message type | Liveness check; answered with an ACK. Uses the same `Beacon` payload as BEACON, but only `boot_id` is set. |
| **Describe** | Request command | Asks a REMOTE for the definition (name, metrics, units, scales) of one device. LOCAL caches the answer. |
| **registry_rev** | Registry revision | Counter in the BEACON that increases whenever a device or metric definition changes; tells LOCAL to run `Describe` again. |
| **boot_id** | Boot identifier | Random number chosen at each reboot; lets the receiver detect that a node has restarted. |
| **Backoff** | | Random waiting time before a retransmission, to avoid repeated collisions. |
| **Collision** | | Two nodes send at the same time and both frames are lost. |
| **RTT** | Round-Trip Time | Time from sending a request until its answer arrives. |
| **LRU** | Least Recently Used | Registry of REMOTE nodes ordered by last-seen time; stale entries are purged. |
| **Metric** | | Definition of one kind of value that a device reports: numeric id (unique per device), name, unit and scale. Defined by the REMOTE, so any sensor value can be sent. |
| **Measurement** | | One measured value on the wire: a metric id and a scaled integer. Real value = value / scale. |
| **Protobuf** | Protocol Buffers | Compact binary data format. 4 to 5 times smaller than JSON. |
| **nanopb** | | Protobuf library for microcontrollers (no heap, small code). |
| **gRPC** | (Google) Remote Procedure Call | Framework that also uses protobuf. Only the protobuf encoding is used here, not the HTTP/2 transport. |
| **CBOR / MessagePack** | | Alternative compact binary formats that were considered. |
| **IoT** | Internet of Things | Sensors and devices that report data over a network. |
| **Modbus** | | Industrial serial protocol; typical way to read power meters. |
| **Home Automation** | | Smart-home systems that receive the data, for example via MQTT. |

## Logging and Diagnostics

| Term | Meaning | Explanation |
|---|---|---|
| **TRACE / DEBUG / INFO / WARN / ERROR** | Log levels | From most to least verbose. Default `INFO`. Persisted in NVS. |
| **SOS** | Morse distress signal | The status LED pattern shown after a fatal fault. |
| **FF FF FF** | Error response | The E220 reply to a malformed configuration command. |
| **PoC** | Proof of Concept | Milestone 0, the plain-text ping-pong test. |
| **M0 .. M7** | Milestones 0 to 7 | Steps of the implementation plan in `loraiot-plan.md`. Not to be confused with the E220 pins M0/M1. |
