# LoRaIoT LoRa Protocol Specification

Facts about the radio module are taken from `E220-400T22D_UserManual_EN_v1.2.pdf`, which
wins over any other document in this repository.

---

## 1. Principles

- Topology: star. All traffic is between LOCAL and one REMOTE. REMOTE-to-REMOTE traffic is not supported.
- Every frame carries SRC and DST in its header. The E220 never delivers the sender address to the MCU.
- The header is fixed binary (parseable without nanopb). The message content is protobuf (nanopb).
- Internally (REST on LOCAL and REMOTE) the data is JSON. Over LoRa it is protobuf.
- One message is exactly one frame. There is no fragmentation and no multi-frame message.
- Half-duplex, one shared channel, LBT off. Collisions are handled by ACK + random backoff + retry.
- Every transmission follows the AUX rule: AUX HIGH, write the frame in one `write()`,
  wait AUX LOW, wait AUX HIGH, then `LORA_RF_GUARD_DELAY_MS` (50 ms).
- Only the `loraTask` touches Serial2. REST/WebSocket handlers post requests to it and wait.

---

## 2. Radio configuration (normative)

| Register | Value | Meaning |
|---|---|---|
| ADDH / ADDL | `0x00` / node address | Module address `0x00NN`. Never `0x0000` or `0xFFFF` (see §3) |
| REG0 | `0xE7` | Bits 7-5 = `111` 115200 baud, bits 4-3 = `00` 8N1, bits 2-0 = `111` 62.5 kbps |
| REG1 | `0x00` | 200 B sub-packet, RSSI ambient noise off, 22 dBm |
| REG2 | channel (default `0x17`) | 410.125 MHz + CH x 1 MHz, CH 0-83 |
| REG3 | `0x40` | Bit 7 RSSI byte OFF, bit 6 fixed transmission, bit 4 LBT OFF, WOR 500 ms |
| CRYPT_H / CRYPT_L | shared 16-bit key | See §16. `0x0000` = off |

- Transmit: the MCU writes `[ADDH][ADDL][CH]` followed by the frame (fixed transmission, manual §7.2 REG3 bit 6).
- Unicast target: `ADDH=0x00`, `ADDL=DST`. Broadcast target: `ADDH=0xFF`, `ADDL=0xFF`.
- Register writes use Mode 3 (M1=1, M0=1) at 9600 8N1, regardless of REG0.
- Response `FF FF FF` means a malformed command.

---

## 3. Addressing

| Address | Meaning |
|---|---|
| `0x00` | Invalid / unprovisioned |
| `0x01` | LOCAL (fixed) |
| `0x02` - `0xFE` | REMOTE (provisioned) |
| `0xFF` | Broadcast |

Manual §5.3 and §5.4: module address `0xFFFF` is the broadcast address (a frame sent to it is
received by all modules on the channel); module address `0x0000` or `0xFFFF` makes a
receiving module monitor the channel without address filtering. Neither is a valid own
address here. LOCAL and the REMOTEs also filter by DST in software.

Broadcast is allowed only for PING. Replies to a broadcast PING are unicast.

---

## 4. Frame format

```
+------+-----+------+-----+-----+--------+-----+---------+--------+
| SYNC | VER | TYPE | SRC | DST |  SEQ   | LEN | PAYLOAD | CRC16  |
| 2 B  | 1 B | 1 B  | 1 B | 1 B |  2 B   | 1 B | 0-186 B |  2 B   |
+------+-----+------+-----+-----+--------+-----+---------+--------+
```

| Field | Size | Notes |
|---|---|---|
| SYNC | 2 | `0xAA 0x55` |
| VER | 1 | Protocol version, `0x01` |
| TYPE | 1 | See §5 |
| SRC | 1 | Sender address |
| DST | 1 | Receiver address |
| SEQ | 2 | Big-endian |
| LEN | 1 | Payload length 0 to 186 |
| PAYLOAD | LEN | Protobuf message |
| CRC16 | 2 | Big-endian |

The fixed overhead is 11 B. The E220 sub-packet is 200 B (REG1). With the 3 routing bytes
the UART write is at most 3 + 11 + 186 = 200 B, so the maximum payload is 186 B. The manual
does not state whether the routing bytes count toward the sub-packet, so the conservative
reading is used. A frame must fit one sub-packet, otherwise the receiving
module outputs it in pieces.

### 4.1 CRC

CRC-16/CCITT-FALSE: polynomial `0x1021`, init `0xFFFF`, no input reflection, no output
reflection, xorout `0x0000`.

- Scope: VER, TYPE, SRC, DST, SEQ, LEN, PAYLOAD (not SYNC, not the CRC itself).
- Check value: CRC of the ASCII string `123456789` is `0x29B1` (use as a unit test).
- Reference bitwise implementation:

```cpp
uint16_t crc = 0xFFFF;
for (size_t i = 0; i < len; i++) {
  crc ^= (uint16_t)data[i] << 8;
  for (int b = 0; b < 8; b++)
    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
}
```

### 4.2 Worked example

PING from LOCAL (`0x01`) to REMOTE (`0x02`), SEQ 1, payload `Beacon{boot_id = 0x12345678}` (the optional fields are absent in a PING):

| Part | Bytes |
|---|---|
| E220 routing header (ADDH, ADDL, CH 0x17) | `00 02 17` |
| SYNC | `AA 55` |
| VER, TYPE (PING = `01`), SRC, DST | `01 01 01 02` |
| SEQ | `00 01` |
| LEN | `06` |
| PAYLOAD (field 1 varint) | `08 F8 AC D1 91 01` |
| CRC16 | `72 0D` |

Full UART write: `00 02 17 AA 55 01 01 01 02 00 01 06 08 F8 AC D1 91 01 72 0D`.
(The CRC value was computed with the reference implementation above.)

### 4.3 Role of the VER byte

The `VER` byte (`0x01`) is part of the fixed binary frame header, checked by the framing layer
before any protobuf decoding. It guards breaking changes of binary header layout, CRC scope/algorithm,
or frame `TYPE` meaning. Protobuf field-number evolution handles compatible payload changes (such
as adding new fields or `oneof` branches); `VER` is intentionally not inside the proto3 payload
because the receiver must know the binary frame layout before it can decode the payload.
Compatible payload changes (such as adding the `text` field to `Measurement` with field number 2
retained for numeric values) are wire-compatible (older receivers ignore unknown protobuf fields)
and therefore do not bump `VER` (which remains `0x01`).

### 4.4 Receiver rules

Drop silently (log at DEBUG, count in the error counter) on: bad CRC, unknown VER, unknown
TYPE, DST that is neither the own address nor `0xFF`. If the SYNC bytes are missing the
parser resynchronises on the next `0xAA 0x55`. A receiver never answers a bad frame with an
error frame (this avoids storms).

---

## 5. Message types

| Code | Name | Reply | Purpose |
|---|---|---|---|
| `0x01` | PING | ACK | Liveness check, unicast or broadcast. Payload is a `Beacon` with `boot_id` and optionally `registry_rev` |
| `0x02` | BEACON | none | "I'm here", REMOTE to LOCAL. Payload is a `Beacon` with all fields |
| `0x03` | ACK | none | Confirms one DATA_ACK or one PING |
| `0x04` | DATA | none | Data, fire and forget |
| `0x05` | DATA_ACK | ACK | Data that must be confirmed |
| `0x06` | REQUEST | RESPONSE | Command or query (e.g. "read sensor A") |
| `0x07` | RESPONSE | none | Answer to a REQUEST, also its implicit acknowledgement |

- Either side may send DATA, DATA_ACK and REQUEST. A REMOTE may send them unsolicited
  (for example a load alert).
- A REQUEST is answered by its RESPONSE, which carries `request_seq`. There is no separate ACK.
- The ACK payload names the acknowledged TYPE and SEQ. The acknowledged message's sender is
  the ACK's DST, so it is not repeated in the payload.
- A `DATA_ACK` received for an unknown device is answered with `ACK{status=BAD_REQUEST}`, and the
  receiver schedules a Describe crawl for that peer.
- Codes `0x08` - `0x7F` are reserved, `0x80` - `0xFF` are free for experiments.

---

## 6. Sequence numbers

- Each node has one counter, incremented for every new frame and wrapping 65535 to 0.
- Retransmissions reuse the SEQ.
- A frame is a duplicate if SEQ and TYPE equal those of the previous frame from the same SRC.
  - A duplicate DATA_ACK is ACKed again but not processed again.
  - A duplicate REQUEST is answered with the cached RESPONSE again (the sensor is not read twice).
- Loss statistic: `gap = (uint16_t)(seq - lastSeq - 1)`. If `gap < 32768`, `gap` frames were lost.
  Otherwise count nothing (reboot or reordering).
- The counter starts at a random value after each boot.

### 6.1 Reboot detection (boot_id)

Every node creates a random 32-bit `boot_id` at boot. It is carried in BEACON and PING and
repeated in every one of them.

The receiver stores the last `boot_id` per SRC. When it changes or is first seen, the receiver
resets all state it holds for that SRC: last SEQ, duplicate detection and the cached RESPONSE.
No acknowledgement is required, because the next BEACON or PING repeats the value.

- REMOTE to LOCAL: the BEACON carries `boot_id`. A booting REMOTE is in UNDISCOVERED state and beacons every 5 s.
- LOCAL to REMOTE: PING carries `boot_id`. LOCAL sends a broadcast PING at boot and repeats it in every keep-alive PING.
- On a BEACON with a new or unknown `boot_id`, LOCAL replies with a PING.
- A REQUEST that was in flight during a reboot is not answered; the requester times out (§7).

---

## 7. Reliability and timing

Named constants:

| Message | Attempt timeout | Attempts | Retry logic |
|---|---|---|---|
| DATA_ACK | `LORA_ACK_TIMEOUT_MS` = 200 ms | 4 (1 + 3 retries) | `loraSendDataReliable` retry loop in `LoRaBroker.cpp` |
| PING (unicast, beacon reply) | `PING_ATTEMPT_TIMEOUT_MS` = `LORA_REQUEST_TIMEOUT_MS` = 1000 ms | 1 (no retry) | Single `loraBrokerSend` + `loraBrokerWait`; a missed beacon reply only delays REMOTE's switch to slow beacon rate |
| PING (unicast, keep-alive) | `PING_ATTEMPT_TIMEOUT_MS` = 1000 ms | up to `PING_OFFLINE_ATTEMPTS` = 3 | Application-level loop in `PingTask`; marks peer offline only after 3 real timeouts |
| PING (broadcast) | `PING_ATTEMPT_TIMEOUT_MS` = 1000 ms | 1 (no retry) | `loraBrokerSend` + `loraBrokerWait`; LOCAL may repeat manually |
| REQUEST (Echo / Read) | `LORA_REQUEST_TIMEOUT_MS` = 1000 ms | 1 (no retry) | Single `loraBrokerSend` + `loraBrokerWait`; callers receive `504` on timeout |

- The timeout starts when the transmission, including the AUX guard, has completed.
- Before each DATA_ACK retry, wait a random time between `LORA_BACKOFF_MIN_MS` (20 ms) and `LORA_BACKOFF_MAX_MS` (200 ms).
- Between keep-alive PING attempts, `PingTask` waits `PING_ATTEMPT_GAP_MS` (1000 ms).
- After the last DATA_ACK attempt: call `LoRaTransport.incrementAckLoss()`, log an error, and return timeout. The node does not enter the FAULT state.
- Each REMOTE delays its ACK to a broadcast PING by a random 0 to `LORA_BCAST_REPLY_WINDOW_MS` (1000 ms).
  Lost replies are tolerated; LOCAL may repeat the PING.
- The REQUEST timeout covers REMOTE processing plus the RESPONSE airtime (up to 200 B).

---

## 8. Beacon and discovery

REMOTE state machine:

```
UNDISCOVERED  ->  BEACON every 5 s (+ random jitter 0 to 1 s)
    first frame from LOCAL addressed to this REMOTE
DISCOVERED    ->  BEACON every 60 s
    no frame from LOCAL for 2 min
UNDISCOVERED  ->  (self-healing, no coordination needed)
```

LOCAL:
- Registry entry per REMOTE: address, last-seen timestamp, boot_id, online flag.
- Any frame from a REMOTE updates its last-seen.
- On a BEACON from an unknown REMOTE or with a changed `boot_id`, LOCAL replies with a PING.
- LOCAL's PING reply MAY carry `registry_rev` (the revision of LOCAL's control table for that node).
  LOCAL only includes `registry_rev` when it has at least one control device registered for that
  node (R4).
- When LOCAL's control table for a node changes (new device, new metric, changed unit or scale),
  LOCAL sends an immediate PING to that node, not only on the next beacon.
- LOCAL sends a keep-alive PING to every REMOTE idle for 60 s.
- REMOTE runs the same keep-alive towards LOCAL (the same `PingTask` code on both tiers). LOCAL
  answers every PING with an ACK. After the idle threshold and three unanswered PINGs the peer is
  marked offline in the cache registry (`online:false` in the REST views); any received frame
  from it revives it.
- An entry is purged after 180 s without any frame (3 missed slow beacons). The REMOTE
  re-enters through its fast beacon.

---

## 9. Medium access

There is no scheduler. Either side may transmit at any time (LBT off), and collisions are
resolved by §7. A node that waits for an ACK or RESPONSE sends no new frames except replies.
Values are pushed by their owner (`DATA` or `DATA_ACK`). `REQUEST(Read)` is an on-demand fetch only
(for example `?live=true` in REST); nothing sends it on a timer.

---

## 10. Payloads (protobuf, nanopb)

The same schema is stored in `src/lora/proto/loraiot.proto`. It is a technical design artifact (not compiled, not used
by the firmware yet). Keep the two copies identical; this section is the source of truth.

```proto
syntax = "proto3";
package loraiot;

// ======================================================================
// Common
// ======================================================================

// Result code. Used in ACK and RESPONSE. OK is 0, so it is omitted on the wire.
enum Status {
  STATUS_OK          = 0;  // Everything worked
  STATUS_BAD_REQUEST = 1;  // Something failed, see the log of the answering node.
                           // Also ends the Describe crawl (no such device)
}

// ======================================================================
// Definitions: sent rarely, cached by LOCAL
// ======================================================================

// Describes ONE kind of value that a device reports.
message Metric {
  uint32 id    = 1;  // Short number used in Measurement.metric_id. Unique within one
                     // device, starts at 1. Sensor A and Sensor B may both have an id 1.
  string name  = 2;  // Human-readable key shown in REST/JSON, e.g. "Voltage" (max 12 chars).
                     // Case-sensitive, used verbatim
  string unit  = 3;  // Unit shown next to the value, e.g. "V". May be empty (max 5 chars)
  uint32 scale = 4;  // Divisor that turns the integer back into the real value:
                     // real = value / scale. Example: scale 100, value 335 -> 3.35.
                     // 0 (not set) is treated as 1, so plain integers need no scale
}

// Describes ONE device and all the metrics it can report.
message Device {
  uint32 device = 1;            // Device index. 0 = the REMOTE node itself,
                                // 1.. = a device attached through the SoftAP
  string name = 2;              // Device name, e.g. "Sensor A" (max 12 chars)
  repeated Metric metrics = 3;  // The metrics this device reports (max 8)
}

// ======================================================================
// Measurements: sent often, as small as possible
// ======================================================================

// One measured value. Its meaning (name, unit, scale) comes from the Metric
// with the same id, found through Data.device.
message Measurement {
  uint32 metric_id = 1;  // Which Metric this is, e.g. 1 = "Voltage" of that device
  oneof val {
    sint32 num = 2;      // The scaled integer. sint32 keeps negative numbers short
                         // (a plain int32 would need 10 bytes for -5)
    string text = 3;     // Opaque text value (max 12 bytes UTF-8). Scale is ignored.
  }
}

// A set of measurements from ONE device. Payload of DATA and DATA_ACK, and the
// result inside the RESPONSE to a Read.
message Data {
  uint32 device = 1;                // Which device the values belong to (0 = the node itself)
  repeated Measurement values = 2;  // The values, normally a few per frame (max 16)
  uint32 age_ms = 3;                // Milliseconds since the values were measured.
                                    // 0 = just now. A SoftAP device pushes its data,
                                    // so a live read of it reports the age of its last post
  bool alert = 4;                   // true = unsolicited event (e.g. a threshold was passed),
                                    // false = normal reading
}

// ======================================================================
// Control messages
// ======================================================================

// Confirms that one frame was received. Payload of the ACK type.
// The ACK's DST is the original sender, so the sender is not repeated here.
message Ack {
  uint32 acked_type = 1;  // TYPE byte of the frame being confirmed.
                          // 5 = a DATA_ACK, 1 = a PING (the only two types that need an ACK)
  uint32 acked_seq  = 2;  // SEQ of that frame. Together with acked_type this tells the
                          // sender exactly which of its pending frames arrived
  Status status = 3;      // OK = accepted. BAD_REQUEST = received, but not accepted
}

// Payload of BOTH the PING type (answered by an ACK) and the BEACON type (no reply).
// A PING sends only boot_id. A BEACON sends all four fields.
message Beacon {
  uint32 boot_id = 1;                // Random number chosen by the sender at boot. If it
                                     // differs from the last one seen from that sender, the
                                     // sender has rebooted and the receiver discards what it
                                     // knew about it
  optional uint32 fw_version = 2;    // Firmware version of the REMOTE (diagnostics).
                                     // Absent in a PING
  optional uint32 uptime_s = 3;      // Seconds since the REMOTE booted. Absent in a PING
  optional uint32 registry_rev = 4;  // Counter that increases whenever a Device or a Metric is
                                     // added or changed. Present in a BEACON (REMOTE to LOCAL)
                                     // and MAY be present in a PING (LOCAL to REMOTE, when LOCAL
                                     // has control devices for that node). A new value triggers a
                                     // Describe crawl. Absent when there are no devices to report
                                     // ("optional" lets the receiver tell "absent" from a real 0)
}

// ======================================================================
// REQUEST / RESPONSE
// ======================================================================

// "Give me the current values of this device", also known as a live read.
// The answer is a Data containing ALL metrics of that device.
message Read {
  uint32 device = 1;  // Device to read (0 = the node itself)
}

// "Tell me what this device is and what it reports." The answer is a Device.
message Describe {
  uint32 device = 1;  // Device index to describe. The asker starts at 1 and goes 1, 2, ... until
                      // it gets STATUS_BAD_REQUEST, which means there are no more devices.
                      // Index 0 is reserved and always answers STATUS_BAD_REQUEST
}

// "Send my text back". Used for link testing and for measuring the payload limit.
message Echo {
  string text = 1;  // The REMOTE returns it REVERSED in the RESPONSE (max 175 chars). Reversing
                    // proves the frame was processed and not merely reflected
}

// Payload of the REQUEST type. Exactly one command is set.
message Request {
  oneof command {
    Read read = 1;          // Live read of one device
    Describe describe = 2;  // Fetch a device definition
    Echo echo = 3;          // Link test
  }
}

// Payload of the RESPONSE type. It answers exactly one REQUEST.
message Response {
  uint32 request_seq = 1;  // SEQ of the REQUEST this answers. This is how LOCAL matches
                           // the answer to the REST call that is waiting for it
  Status status = 2;       // OK, or BAD_REQUEST if the request failed
  oneof result {           // At most one, depending on the request. None on error
    Data data = 3;         // Answer to Read
    Device def = 4;        // Answer to Describe
    Echo echo = 5;         // Answer to Echo
  }
}
```

| TYPE | Payload message |
|---|---|
| PING, BEACON | `Beacon` |
| ACK | `Ack` |
| DATA, DATA_ACK | `Data` |
| REQUEST | `Request` |
| RESPONSE | `Response` |

Rules:
- Sender, receiver and version are never in the payload (header only). `boot_id` is the single exception
  because it describes the sender's lifetime, not its identity.
- A BEACON must set all four `Beacon` fields. A PING sets `boot_id` and MAY set `registry_rev` (the revision
  of the sender's control table for the target node). The firmware checks this by TYPE, not by the schema.
  nanopb generates `has_fw_version`, `has_uptime_s` and `has_registry_rev` for the `optional` fields.
- Unknown fields are ignored (forward compatibility).
- A `Measurement` whose `metric_id` or `Data.device` is not in the LOCAL cache is shown in REST as
  `device_<n>/metric_<id>` and triggers a `Describe` (§12).
- A device that has no value yet answers `Read` with `STATUS_OK` and an empty `values` list.
- The REMOTE rejects a registration (a new device or metric from the SoftAP REST API, answered with HTTP 400)
  if the resulting encoded `Device` plus the `Response` overhead would exceed 186 B, or if worst-case `Data` / `Response(Data)`
  would exceed 186 B. A `Describe` or `Read` answer and a `Data` push therefore always fit one frame.
- Text values are opaque strings of at most 12 bytes UTF-8 (`loraiot.Measurement.text max_size:13` in `.options`).
  Scale is ignored for text values (scale applies to `num` only). A string longer than 12 bytes is rejected
  on ingestion (never truncated, so multi-byte UTF-8 sequences are never cut). Empty strings are allowed.
- The nanopb `.options` file (`src/lora/proto/loraiot.options`) bounds all strings and repeated fields
  (`Metric.name` 12, `Metric.unit` 5, `Device.name` 12, `Device.metrics` 8, `Measurement.text` 12, `Data.values` 16, `Echo.text` 175)
  so no heap is used. Measured encoded sizes against the 186 B payload limit: `Data` with 16 worst-case numeric
  values 171 B (fits); `Response(Echo)` with 175 characters 185 B (fits); `Response(Device)` with 4
  maximum-length metrics 138 B (fits); `Device` with 8 maximum-length metrics 241 B (does NOT fit); `Data` with 8 worst-case
  12-byte text metrics 165 B (fits); `Response(Data)` with 8 worst-case 12-byte text metrics 165 B (fits). At most 5
  maximum-length metrics fit in `Device` and all 8 full-width text metrics fit in `Response(Data)`, so the schema bound alone
  does not guarantee that all Device frames fit and the registration check in `registerMetric()` is mandatory. How to regenerate
  the C sources and repeat the measurements: `doc/NanopbGeneration.md`.
- New message fields use new numbers; existing numbers are never reused.

---

## 11. Naming conventions

There is no metric registry in the protocol. Every REMOTE defines its own metrics (§12), so any sensor
value can be transported without changing the LOCAL firmware, for example `{id=1, name="CPU Frequency",
unit="MHz", scale=1}`.

Suggested but not enforced:
- `name` is a short label in title case: `Voltage`, `Current`, `Power`, `Energy`, `Temperature`, `Humidity`.
  It is case-sensitive, so `Voltage` and `voltage` are two different metrics.
- Use SI base units with a `scale` that keeps the value inside `sint32` (about +/-2.1e9). Example:
  230.4 W is `value=2304, scale=10`. For quantities that would overflow (uptime in ms), choose a coarser unit.
- Values can be numeric (scaled integers) or opaque text strings (up to 12 bytes UTF-8).
- For text values (e.g. "30.10.2026", "enabled", "auto"), scale is ignored (no division).
- Booleans sent over REST as JSON `true`/`false` are stored as text `"true"`/`"false"`.

---

## 12. Device identity and definitions

The wire format supports both deployments:
- Custom code on the REMOTE itself (for example a Modbus meter on the GPIO pins) reports as `device = 0`.
- An external device that posts to the REMOTE's SoftAP REST API is addressed as `device = 1..N`.

The owner of a device table defines devices, assigns indices, and pushes values. The other node (cache
side) learns the definitions via `Describe` and receives values via pushes. For sensor data the REMOTE is
the owner and LOCAL is the cache; for control data LOCAL is the owner and the REMOTE is the cache. The
full design is in `TechDebt.md`.

The owner assigns each new device name the next free index (indices are contiguous, starting at 1). A new
JSON key from a known device is assigned the next free metric id (starting at 1) with the unit and scale
taken from the post.

`Beacon.registry_rev` (and `registry_rev` in a PING, §8) increments whenever either table changes. The
cache side keeps a cache per peer and device, and fills it with the `Describe` crawl:

1. The cache side sees a new `boot_id` or a new `registry_rev` in a BEACON or PING.
2. It sends `REQUEST(Describe{1})`, then `Describe{2}`, and so on. Index 0 is reserved and answers
   `STATUS_BAD_REQUEST`.
3. The owner answers each with `RESPONSE(Device)`. It answers `STATUS_BAD_REQUEST` for a device
   that does not exist, which ends the crawl.
4. A lost frame is only a timeout, so the cache side retries (§7) and does not end the crawl early.

After the owner answers `STATUS_BAD_REQUEST` for the first nonexistent device index (end of crawl), it
pushes the current values of all its devices as `DATA` or `DATA_ACK` frames. This ensures the cache side
has definitions and values together.

The cache side drops the whole cache of a peer when its `boot_id` changes. It also runs `Describe` for a
single device when a `Data` frame mentions a device or metric it does not know.

An AP device pushes its data, so a live read of such a device returns the last posted values with
`age_ms` set. Data produced by code on the REMOTE itself can be sampled on demand (`age_ms = 0`).

---

## 13. JSON and protobuf mapping

With the definition `Device{device=1, name="Sensor A", metrics=[{1,"Voltage","V",100}, {2,"Current","A",1000},
{3,"Temperature","degC",10}]}` cached on LOCAL, this JSON:

```json
{"device":"Sensor A","data":{"Voltage":3.35,"Current":0.12,"Temperature":22.5}}
```

corresponds to `Data{device=1, values=[{1,335},{2,120},{3,225}]}` inside a frame with SRC = the REMOTE
and DST = `0x01`. The REST layer computes `value / scale` and takes the key from `Metric.name`.

Address and version are not part of the JSON data object. The REST layer adds the node address
(and `age`) around it when answering a client.

---

## 14. Channel change (not supported)

The protocol has no command to change the radio channel. A channel is changed only through the
configuration of each node (REST or captive portal). A REMOTE configured to a different channel than LOCAL
is unreachable over the air until it is reconfigured locally, for example through its SoftAP REST API.

---

## 15. Exchange diagrams

Reliable data (REMOTE alert):

```mermaid
sequenceDiagram
    participant L as LOCAL
    participant R as REMOTE
    R->>L: DATA_ACK(Data alert)
    L-->>R: ACK(acked_type=5, acked_seq)
```

Live read:

```mermaid
sequenceDiagram
    participant C as REST client
    participant L as LOCAL
    participant R as REMOTE
    C->>L: GET /api/lora/nodes/sensor?addr=2&device=1&live=true
    L->>R: REQUEST(Read device)
    R-->>L: RESPONSE(request_seq, Data)
    L-->>C: 200 JSON (504 on timeout)
```

Discovery of the definitions:

```mermaid
sequenceDiagram
    participant L as LOCAL
    participant R as REMOTE
    R->>L: BEACON(boot_id, registry_rev=3)
    Note over L: new boot_id or registry_rev - start crawl
    L->>R: PING(boot_id, registry_rev)
    R-->>L: ACK(acked_type=1, acked_seq)
    Note over R: PING received - switch to 60s beacon interval
    L->>R: REQUEST(Describe 1)
    R-->>L: RESPONSE(Device 1 "Sensor A", metrics)
    L->>R: REQUEST(Describe 2)
    R-->>L: RESPONSE(BAD_REQUEST)
    Note over R: BAD_REQUEST triggers PushTask
    Note over L: cache complete
    R->>L: DATA_ACK(device=1, values)
    Note over L: PushTask pushes current values after crawl
    L-->>R: ACK(acked_type=5, acked_seq)
```

Push after crawl (R1) - the owner pushes values after the crawl ends:

```mermaid
sequenceDiagram
    participant owner as Owner node
    participant cache as Cache node
    Note over cache: new boot_id or registry_rev from owner
    cache->>owner: REQUEST(Describe 1)
    owner-->>cache: RESPONSE(Device 1 "Sensor A", metrics)
    cache->>owner: REQUEST(Describe 2)
    owner-->>cache: RESPONSE(BAD_REQUEST)
    Note over owner: BAD_REQUEST triggers PushTask - push all current values
    owner->>cache: DATA_ACK(device=1, values)
    cache-->>owner: ACK(acked_type=5, acked_seq, status=OK)
```

Control data flow (LOCAL to REMOTE):

```mermaid
sequenceDiagram
    participant C as REST client
    participant L as LOCAL (owner)
    participant R as REMOTE (cache)
    C->>L: POST /api/lora/nodes/control?addr=2
    L->>R: DATA_ACK(device=1, values)
    R-->>L: ACK(acked_type=5, acked_seq, status=OK)
    L-->>C: 200 OK
    Note over R: control cache updated
```

PING with registry_rev (§8):

```mermaid
sequenceDiagram
    participant L as LOCAL
    participant R as REMOTE
    R->>L: BEACON(boot_id, registry_rev=3)
    Note over L: new boot_id
    L->>R: PING(boot_id, registry_rev=5)
    R-->>L: ACK(acked_type=1, acked_seq)
    Note over R: registry_rev=5 is new - crawl LOCAL
    R->>L: REQUEST(Describe 1)
    L-->>R: RESPONSE(Device 1 "Output V", metrics)
    R->>L: REQUEST(Describe 2)
    L-->>R: RESPONSE(BAD_REQUEST)
    Note over L: crawl complete - push control values
    L->>R: DATA_ACK(device=1, values)
    R-->>L: ACK(status=OK)
```

Liveness (both nodes run PingTask and send keep-alive PINGs independently):

```mermaid
sequenceDiagram
    participant L as LOCAL
    participant R as REMOTE
    L->>R: PING(boot_id)
    R-->>L: ACK(acked_type=1, acked_seq)
    R->>L: PING(boot_id)
    L-->>R: ACK(acked_type=1, acked_seq)
```

---

### End-to-end scenario A - IoT sensor on REMOTE SoftAP pushes data to LOCAL

An IoT sensor (e.g. a voltage meter) connects to REMOTE's SoftAP WiFi and POSTs its readings. LOCAL learns the definitions over LoRa and makes them available through its own REST API.

```mermaid
sequenceDiagram
    participant Sensor as IoT Sensor (SoftAP client)
    participant R as REMOTE (SoftAP 192.168.4.1)
    participant L as LOCAL (LAN)
    participant Client as LAN client / Home Automation

    Sensor->>R: POST /api/lora/sensor/data {"device":"SensorA","metrics":{...}}
    Note over R: SensorRegistry auto-assigns device_idx and metric_ids
    Note over R: registry_rev increments if schema is new
    R-->>Sensor: 200 {"accepted":[...],"registry_rev":2}

    R->>L: BEACON(boot_id, registry_rev=2)
    Note over L: registry_rev changed - start Describe crawl
    L->>R: PING(boot_id, registry_rev)
    R-->>L: ACK(acked_type=1, acked_seq)
    L->>R: REQUEST(Describe 1)
    R-->>L: RESPONSE(Device 1 "SensorA", metrics)
    L->>R: REQUEST(Describe 2)
    R-->>L: RESPONSE(BAD_REQUEST)
    Note over R: BAD_REQUEST triggers PushTask
    R->>L: DATA_ACK(device=1, Voltage=3.35 V, Temp=21.5 C)
    L-->>R: ACK(acked_type=5, acked_seq)
    Note over L: SensorRegistry updated with definitions and values

    Client->>L: GET /api/lora/nodes/sensor?addr=2&device=1
    L-->>Client: 200 {"nodes":[{"addr":2,"devices":[{"name":"SensorA","metrics":[...]}]}]}
```

Subsequent pushes by the sensor (same schema, same `registry_rev`) skip the crawl: REMOTE stores the new value and PushTask sends a `DATA_ACK` to LOCAL directly.

---

### End-to-end scenario B - Controller on LOCAL LAN pushes a setpoint to REMOTE; IoT sensor retrieves it

A controller (e.g. a home automation system) on the local LAN sets a target voltage on REMOTE. An IoT device on REMOTE's SoftAP polls the cached setpoint and applies it.

```mermaid
sequenceDiagram
    participant HA as Controller (LAN client)
    participant L as LOCAL (LAN)
    participant R as REMOTE (SoftAP 192.168.4.1)
    participant Device as IoT device (SoftAP client)

    HA->>L: POST /api/lora/nodes/control?addr=2 {"device":"Charger","metrics":{"VoltageSet":{"value":333,"unit":"V","scale":100}}}
    Note over L: ControlRegistry updated, registry_rev increments
    L->>R: DATA_ACK(device=1, VoltageSet=3.33 V)
    R-->>L: ACK(acked_type=5, acked_seq, status=OK)
    Note over R: ControlRegistry cache updated
    L-->>HA: 200 {"accepted":[...],"registry_rev":1,"delivery":"ok"}

    Device->>R: GET /api/lora/nodes/control
    R-->>Device: 200 {"nodes":[{"addr":1,"devices":[{"name":"Charger","metrics":[{"name":"VoltageSet","value":333,"unit":"V","scale":100,"age_ms":120}]}]}]}
    Note over Device: Device applies VoltageSet = 333 / 100 = 3.33 V
```

If REMOTE does not yet have the definitions when the `DATA_ACK` arrives (e.g. after a reboot), it answers `ACK{status=BAD_REQUEST}` and schedules a Describe crawl of LOCAL. LOCAL returns `202 {"delivery":"after_describe"}` and the values are pushed again once the crawl completes.

---

## 16. Security (v1)

There is no authentication: SRC is not protected and can be spoofed. The only protection is the
E220 communication key (CRYPT_H / CRYPT_L, registers 06H / 07H), which scrambles the radio signal
against similar modules. It is write-only and must be identical on all nodes. `0x0000` disables it.
This is a deterrent, not security. The key is stored in NVS and must not be hardcoded in source.

---

## 17. REST mapping

The source of truth for the implemented API is `src/LoRaIoT/src/openapi_json.h` and `doc/RestApi.md`.

### LOCAL endpoints

| Method | Endpoint | Role | LoRa action |
|---|---|---|---|
| `GET` | `/api/lora/nodes` | - | Registry read (no LoRa) |
| `GET` | `/api/lora/nodes/sensor` | cache | All or one REMOTE: cached schema + values (`?addr=`, `?device=`); `?live=true` sends `REQUEST(Read)`; `?refresh=true` re-runs Describe crawl |
| `GET` | `/api/lora/nodes/control` | owner | Content of LOCAL's `ControlRegistry`; `?addr=` selects the target node |
| `POST` | `/api/lora/nodes/control?addr=` | owner | Create/update control device and push values to node `addr` via `DATA_ACK`; `200` after `ACK{OK}`, `202` when deferred (peer unknown - crawl triggered), `504` on timeout |
| `GET,POST` | `/api/lora/nodes/ping?addr=` | - | `PING` -> wait for `ACK`; returns RTT and `boot_id` |
| `GET` | `/api/lora/ping` | - | Broadcast `PING` (dst=0xFF) -> first `ACK` |
| `POST` | `/api/lora/nodes/echo?addr=` | - | `REQUEST(Echo)` -> `RESPONSE(Echo)` |
| `GET` | `/api/lora/stats` | - | TX/RX/CRC/ACK counters (no LoRa) |

### REMOTE endpoints

| Method | Endpoint | Role | Action |
|---|---|---|---|
| `POST` | `/api/lora/sensor/data` | owner | Sensor ingestion; updates `SensorRegistry`; increments `registry_rev` on schema change |
| `GET` | `/api/lora/nodes/sensor` | owner | Content of REMOTE's `SensorRegistry` (own devices and values); `?device=` filters to one device |
| `GET` | `/api/lora/nodes/control` | cache | Content of LOCAL's control cache: definitions, values, `age_ms`, `online`, `peer_last_seen_ms_ago`; `?live=true` sends `REQUEST(Read)` to LOCAL; `?refresh=true` re-runs Describe crawl |

### Behaviour notes

- All blocking calls return `503` when the broker is busy (one request already in flight).
- All blocking calls return `504` when no response arrives within the timeout (default 3 s).
- `?device=` accepts the device index (integer).
- Handlers never touch Serial2 directly; blocking ones post to `LoRaBroker` and wait on a semaphore.
- `?live=true` and `?refresh=true` are only valid on cache views (LOCAL `.../sensor`, REMOTE `.../control`). On owner views they return `400`.
- REMOTE has exactly one peer (LOCAL) and does not accept `?addr=`; a request that sends it returns `400`.
- `GET /api/lora/nodes/control` on REMOTE includes `online` (false after 90 s with no ACK from LOCAL) and `peer_last_seen_ms_ago`.
- The source of truth for the implemented API is `src/LoRaIoT/src/openapi_json.h` and `doc/RestApi.md`.


---

## 18. Air Rate Validation

### Tested rates

The table below summarises measured round-trip times at the two extreme air rates on an
ESP32-WROOM-32 node pair (LOCAL addr `0x01`, REMOTE addr `0x03`, channel `0x17`, fixed mode).
All other rates between 4.8k and 38.4k are expected to fall within the same timeout budget as
62.5k.

| Air rate | PING RTT (typical) | DATA_ACK delivery | Notes |
|---|---|---|---|
| 62.5k (default) | 117-137 ms | first attempt, well within 1 s | Normal operating rate |
| 2.4k (slowest) | 698-700 ms | retry 2 of 3 on first push after boot; subsequent pushes ACKed on attempt 1 | See timeout note below |

### 2.4k feasibility result

A full end-to-end test at 2.4 kbps confirmed:

- PING/ACK exchange: PASS (RTT ~700 ms, ~6x the 62.5k baseline, consistent with air time ratio).
- Describe crawl (definition fetch): PASS, completes within normal retry budget.
- DATA_ACK push (control setpoints): PASS, delivered correctly on retry; duplicate ACK handling
  exercised correctly.
- Live reconfiguration via `PUT /api/lora/airrate` without reboot: PASS on both nodes; E220 REG0
  confirmed as `0xE2` (UART 115200, 8N1, air rate 2.4k).
- Self-healing after rate switch: PASS, normal beacon/ping cycle resumed automatically.

### Timeout constraint at 2.4k

The default `LORA_REQUEST_TIMEOUT_MS` (1000 ms, defined in `src/lora/src/LoRaTransport.h`) is
tight for the 2.4k DATA_ACK round-trip (~700 ms air time + 50 ms guard + processing). On the
first push attempt the ACK arrives just after the timeout window, triggering retry 1. The retry
succeeds because the ACK from the first attempt arrives and is correctly matched.

**If 2.4k is used in production, increase `LORA_REQUEST_TIMEOUT_MS` to 2000 ms** to avoid the
unnecessary retry and the associated half-duplex channel occupancy.

All other parameters (retry count, beacon intervals, ping idle threshold) work correctly at 2.4k
without change.
