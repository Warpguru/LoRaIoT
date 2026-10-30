# Describe Crawl - How a Node Learns About New Devices and Metrics

This document explains when a node starts a Describe crawl of its peer, how long it takes
for new sensor or control data to become visible on the other node (dashboard and REST API),
and how the dashboard shows the crawler state.

Related documents: [LoRaProtocol.md](LoRaProtocol.md) (frames and payloads),
[Architecture.md](Architecture.md) (tasks and sequence diagrams), [RestApi.md](RestApi.md).

---

## 1. Why a crawl is needed

Data frames carry only `metric_id` and a value. Names, units and scales are not repeated in
every frame. The receiving node (the cache) must therefore know the schema of the sending
node (the owner) before it can store a value:

| Direction | Owner (sends data) | Cache (crawls) | Registry on the cache |
|---|---|---|---|
| Sensor data | REMOTE | LOCAL | `SensorRegistry` |
| Control data | LOCAL | REMOTE | `ControlRegistry` |

The crawl is a series of `REQUEST(Describe{device=1..n})` frames. The owner answers each with
`RESPONSE(Device)` and ends the list with `STATUS_BAD_REQUEST` for the first unknown device
index. When the list ends, the cache calls `markCrawlComplete()` and the owner pushes all
its current values (see section 5).

## 2. What starts a crawl

Every trigger ends in the same flag, `PeerEntry::crawlPending` (`PeerRegistry.h`). The
`DescribeCrawler` polls it on every `applicationLoop()` pass (`Application.cpp`), so a crawl
starts within milliseconds after the flag is set. The latency is therefore decided only by how
quickly the cache learns that a crawl is needed.

| # | Trigger | On | Sets the flag via | Typical delay |
|---|---|---|---|---|
| 1 | BEACON from the owner with a new `registry_rev` or `boot_id` | LOCAL only (REMOTE beacons) | `updatePeerMeta()` | 5 s while undiscovered, 60 s (+ up to 1 s jitter) once discovered |
| 2 | PING from the owner carrying `registry_rev` | LOCAL and REMOTE | `updatePeerMeta()` | immediate (see section 3) |
| 3 | DATA or DATA_ACK for an unknown device or an unknown metric | LOCAL and REMOTE | `requestCrawl()` | immediate (on receipt) |
| 4 | REST `?refresh=true` on the nodes endpoints | both | `requestCrawl()` | immediate |

Constants (source of truth is the code):

| Constant | Value | File |
|---|---|---|
| `BEACON_INTERVAL_UNDISCOVERED_MS` | 5 000 ms | `BeaconTask.h` |
| `BEACON_INTERVAL_DISCOVERED_MS` | 60 000 ms | `BeaconTask.h` |
| `BEACON_JITTER_MAX_MS` | 1 000 ms | `BeaconTask.h` |
| `PING_IDLE_THRESHOLD_MS` | 90 000 ms | `PingTask.h` |
| `DESCRIBE_RESPONSE_TIMEOUT_MS` | 3 000 ms | `DescribeCrawler.h` |
| `DESCRIBE_MAX_RETRIES` | 3 | `DescribeCrawler.h` |

The 90 s in earlier observations is `PING_IDLE_THRESHOLD_MS`, the keep-alive threshold. It is
not a crawler timer. The slow path that used to delay new data was the 60 s BEACON interval.
A keep-alive PING sent after 90 s of silence carries `registry_rev` too, so it is a
last-resort trigger as well.

`loraiot_Data` has no `registry_rev` field. Only BEACON and PING (the `Beacon` message)
carry the revision.

## 3. Immediate notification after a schema change

A schema change is a new device or a new metric (`registerMetric()` increments `registry_rev`).

**Sensor path (REMOTE `POST /api/lora/sensor/data`):**
If `registry_rev` changed during the request, the handler calls
`pingTaskNotifyControlChange(LORA_DEFAULT_ADDR_LOCAL)` (`RestService.cpp`). `PingTask` sends a
PING to LOCAL immediately. The PING carries `registry_rev` because `LoRaBroker.cpp` adds it to
every unicast PING when the owner registry has devices for that peer. The handler then pushes
the DATA frames.

**Control path (LOCAL `POST /api/lora/nodes/control`):**
The same mechanism in the other direction: `pingTaskNotifyControlChange(addr)` is called when
the schema changed, then the values are pushed reliably (DATA_ACK).

**Receiving a PING with `registry_rev`:**
- On REMOTE, the PING handler calls `ControlRegistry.updatePeerMeta()`.
- On LOCAL, the PING handler calls `SensorRegistry.updatePeerMeta()`, but only for a peer that
  LOCAL already knows from a BEACON. A PING from an unknown address cannot occupy one of the
  few peer slots. A PING without `registry_rev` is a plain keep-alive and changes nothing.

**Air-time cost:** one extra PING and ACK pair per schema change, nothing for value-only
updates.

## 4. Data that arrives before the schema

`PeerRegistry::storeData()` returns a `DeviceTableResult`:

| Result | Meaning | Action in the DATA / DATA_ACK handler |
|---|---|---|
| `OK` | every metric stored | DATA_ACK is answered with `STATUS_OK` |
| `UNKNOWN_METRIC` | device known, at least one `metric_id` unknown (the known metrics were still applied) | `requestCrawl()`; DATA_ACK is answered with `STATUS_BAD_REQUEST` |
| `UNKNOWN_DEVICE` | peer or device unknown | `requestCrawl()`; DATA_ACK is answered with `STATUS_BAD_REQUEST` |

This closes the case that a new metric is added to a device that is already cached: previously
only the next BEACON (up to about 61 s later) revealed it.

The values of the frame that arrived too early are not kept (for an unknown device) or only
partly kept (for an unknown metric). That is acceptable because the owner keeps the values in
its registry and pushes all of them again when the crawl completes (section 5).

A DATA frame does not get an ACK at all (fire-and-forget); a DATA_ACK sender learns about the
missing schema from `STATUS_BAD_REQUEST`. `PushTask` logs this ("peer needs crawl"); the crawl
itself was already started by the receiver.

Possible loop: a sender that keeps sending a `metric_id` that never appears in its own Describe
response would trigger a crawl with every frame. The owner assigns ids and sends the matching
Describe, so this cannot happen with consistent firmware; the cost would be bounded by the data rate.

## 5. After the crawl: values are pushed

When the cache finishes the crawl, the owner's `sendDescribeResponse()` sees the final
`STATUS_BAD_REQUEST` and calls `pushTaskNotifyCrawlComplete()`. `PushTask` sends every device
of the owner registry with `loraSendDataReliable()` (DATA_ACK). This is why a lost first DATA
frame does not matter. `PushTask` also pushes when a node returns online.

## 6. Timelines

**New device with metrics POSTed to REMOTE (sensor path):**

```
T+0        POST on REMOTE: registerMetric() raises registry_rev; values stored.
T+0        PING (registry_rev) queued to PingTask, DATA frame sent to LOCAL.
T+air time LOCAL: PING handler -> updatePeerMeta() -> crawlPending.
           (the DATA frame -> UNKNOWN_DEVICE -> requestCrawl() is a second trigger)
T+~1 ms    DescribeCrawler starts REQUEST(Describe{1}) ... until BAD_REQUEST.
T+crawl    markCrawlComplete(); REMOTE PushTask pushes the values (DATA_ACK).
           LOCAL stores them; the WebSocket broadcast shows them within 1 s.
```

Expected total: a few seconds (air time of 2 frames per device plus the push). Before the
change the worst case was about 66 s.

**New device with metrics POSTed to LOCAL (control path):** the same flow with the roles
swapped (LOCAL PING -> REMOTE crawls LOCAL's ControlRegistry -> LOCAL PushTask pushes).

**New metric on an already cached device:** the PING with the new `registry_rev` starts the
crawl; if the PING is lost, the first DATA with the new `metric_id` does (`UNKNOWN_METRIC`).

## 7. Dashboard crawler icon

`index_html.h` shows a spider glyph (`#crawlerStatus`) directly left of the lightning icon
(`#wsActivity`). Both use the same technique: a text glyph that is faded (`opacity: .15`) and
fully visible with the CSS class `active`.

- The WebSocket JSON contains `crawl_pending` for every node (`appendNodeJson()` in
  `WebSocketService.cpp`), taken from `getNodeInfo()`.
- `render()` sets the icon active when at least one node has `crawl_pending: true`
  (`setCrawlerActive()`), otherwise inactive. There is no timer; the icon follows the state of
  the registry and updates with every 1 s broadcast.
- The flag is true from the moment a trigger sets it until `markCrawlComplete()` clears it.
  With the immediate triggers of section 3 this is normally only the duration of the crawl
  (about 1 to 3 s), so the icon lights up briefly after a POST. It stays on for a long time
  only if a crawl cannot finish (for example when the peer is out of range).
- On REMOTE the flag shows crawls of LOCAL's ControlRegistry; on LOCAL it shows crawls of
  REMOTE's SensorRegistry.

## 8. Test checklist

1. POST a new device with two metrics to REMOTE `/api/lora/sensor/data`. On LOCAL
   `GET /api/log` should show `PING from 0x.. registry_rev=N (crawl scheduled)` (or
   `DATA ... device unknown, Describe crawl scheduled`), then the Describe lines, then the
   values pushed. The LOCAL dashboard shows the values after a few seconds and the spider icon
   lights up briefly.
2. POST a further metric on the same device. Expected: `registry_rev` increases, the PING
   starts a new crawl, the new metric appears without waiting for a BEACON.
3. Repeat in the other direction with `POST /api/lora/nodes/control` on LOCAL and check
   REMOTE.
4. Power off REMOTE, POST nothing, and check that the spider icon does not stay lit on LOCAL
   without a pending crawl (it must only be on while `crawl_pending` is true).
