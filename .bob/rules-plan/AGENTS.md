# Project Architecture Rules (Non-Obvious Only)

- **Raw IP tunneling is architecturally forbidden** - TCP RTT and FIFO overflow make it unworkable. All designs must use application-level proxy/bridge patterns.
- The LoRa link is **half-duplex**: Remote ESP32 and Local ESP32 cannot transmit simultaneously. Protocol designs must account for turn-based transmission (request/ACK sequencing).
- Max reliable payload per LoRa burst: **190 bytes** (200-byte sub-packet minus 8-byte frame header + CRC). Designs that exceed this must implement multi-frame reassembly.
- At 62.5 kbps air rate, practical range drops to 500 m–1.5 km. At 2.4 kbps, range is up to 5 km but throughput is ~200 bytes/s. Architecture must expose air rate as a tunable tradeoff.
- The E220's 400-byte onboard FIFO means burst writes larger than 400 bytes without AUX polling will silently drop data - streaming designs must chunk writes and wait for AUX.
- Two separate WiFi roles per node: Remote = SoftAP (192.168.4.1), Local = STA client. Both roles run simultaneously on the same ESP32 - no external router required on the remote side.
- FreeRTOS task separation is required: UART RX/TX handling must run in a dedicated task to avoid blocking the WiFi stack.
