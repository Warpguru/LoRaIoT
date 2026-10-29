---
name: loraiot-ping
description: >-
  Use when the user wants to verify that the LoRaIoT PING/ACK message exchange
  is working correctly — fetches live logs from LOCAL and REMOTE nodes via curl,
  checks all expected log lines, cross-checks frame seq numbers and RTT, and
  tests the REST ping endpoint.
metadata:
  disable-model-invocation: false
---

# LoRaIoT PING Verification

Follow these steps to verify that the PING/ACK exchange between LOCAL and REMOTE is working
correctly. Do **not** ask the user for information already known below; proceed directly.

## Known constants

| Item | Value |
|---|---|
| LOCAL IP | `http://192.168.1.147` |
| REMOTE IP | `http://192.168.1.174` |
| LOCAL LoRa address | `0x01` |
| REMOTE LoRa address | `0x02` |
| Expected RTT range | 80–200 ms (LoRa air time + 50 ms E220 guard) |
| REST ping endpoint | `GET /api/lora/nodes/ping?addr=2` on LOCAL |

## Step 1 — Reboot both nodes for a clean log

Reboot both nodes via the REST API (no manual intervention needed):

```
curl -s http://192.168.1.147/reboot
curl -s http://192.168.1.174/reboot
```

Wait ~5 seconds for both nodes to come back online, then proceed.

## Step 2 — Fetch both logs

```
curl -s http://192.168.1.147/api/log
curl -s http://192.168.1.174/api/log
```

Parse the JSON `lines` array from each response. Work through the lines in order.

## Step 3 — Expected log patterns

These are the exact strings to look for. No source reading needed.

### LOCAL expected log lines (in order)

| # | Expected substring | Notes |
|---|---|---|
| 1 | `PingTask: started` | confirms PingTask was created |
| 2 | `PingTask: LoRa gate open - starting keep-alive loop` | LoRa ready, keep-alive begins |
| 3 | `LoRaBroker: sent PING seq=N dst=0x02` | first PING sent; note seq N |
| 4 | `PING ACK from 0x02 seq=N rtt=Nms` | ACK received; seq must match #3; RTT 80–200 ms |

### REMOTE expected log lines (in order)

| # | Expected substring | Notes |
|---|---|---|
| 1 | `PING from 0x01 seq=N boot_id=0x` | PING received; seq must match LOCAL #3 |
| 2 | `ACK sent to 0x01 acked_seq=N` | ACK sent; acked_seq must equal the PING seq |

## Step 4 — Verify LOCAL log

Check each item and note the exact log line as evidence:

1. **PingTask started** — look for `PingTask: started` with idle interval and attempt count.
2. **LoRa gate open** — look for `PingTask: LoRa gate open - starting keep-alive loop`.
3. **First PING sent** — look for `LoRaBroker: sent PING seq=N dst=0x02`; note seq N.
4. **ACK received for each PING** — look for `PING ACK from 0x02 seq=N rtt=Nms`.
   - Every PING seq must have a matching ACK log line.
   - RTT must be within the expected range (80–200 ms).
5. **No spurious errors** — scan for `[ERROR]` or `[WARN]` lines unrelated to expected
   boot-time contention (see §Known transients below).

## Step 5 — Verify REMOTE log

1. **PING received** — look for `PING from 0x01 seq=N boot_id=0x...` for each PING seq seen on LOCAL.
2. **ACK sent** — look for `ACK sent to 0x01 acked_seq=N` immediately after each PING.
   - `acked_seq` must equal the PING seq.
3. **No errors or warnings**.

## Step 6 — Cross-check frame seq numbers

Build a table:

| PING seq | LOCAL TX log | REMOTE RX log | REMOTE ACK TX log | LOCAL ACK RX log | RTT |
|---|---|---|---|---|---|

Every row must have an entry in all five columns (or a documented reason why it does not,
e.g. boot-time collision — see §Known transients).

## Step 7 — Test the REST ping endpoint

```
curl -v "http://192.168.1.147/api/lora/nodes/ping?addr=2"
```

Expected:
- HTTP 200
- JSON body: `{"addr":2,"rtt_ms":<N>,"boot_id":"0x..."}` where `rtt_ms` is within 80–200 ms
  and `boot_id` matches the value from REMOTE's BEACON log.

Report the actual status code and body verbatim.

## Step 8 — Report

Write a structured report with:

- **Verdict table** — one row per component (LOCAL, REMOTE, REST endpoint): PASS / FAIL / PARTIAL
- **Evidence** — for each check in Steps 4–7, the exact log line that confirms or contradicts it
- **Bugs** — any unexpected behaviour with the log evidence and a recommended fix
- **Known transients** — mention but do not treat as bugs (see below)

## Known transients (do not flag as bugs)

- **Boot-time collision (first PING times out):** At boot LOCAL queues a Describe REQUEST and a
  PING back-to-back immediately after the first BEACON. On a half-duplex channel these can collide
  with REMOTE's RESPONSE and next BEACON. Self-recovers on the next BEACON. Only flag if the
  pattern persists beyond the second PING.
- **Occasional REST ping HTTP 504:** A single 504 among multiple REST pings is a transient RF
  collision between REMOTE's ACK and its own BEACON. Flag only if it reproduces consistently.

## Offline/recovery path (optional extended test)

If the user wants to verify the offline detection and recovery path:

1. Reboot REMOTE and immediately disable its WiFi by pulling power (or use a long
   `delay` patch). Alternatively, simply leave REMOTE powered off after the initial
   `/reboot` call and do not wait for it to come back.
   A simpler trigger: just wait — LOCAL will detect REMOTE as idle after ~90 s with no ACK.
2. Watch LOCAL log for:
   `PingTask: node 0x02 idle ... did not answer 3 PINGs - marked offline`
   (expect ~90 s + 3 × 1 s = ~93 s after the last successful ACK).
3. Reboot REMOTE back online:
   ```
   curl -s http://192.168.1.174/reboot
   ```
4. Expect LOCAL to log node marked online again after the next BEACON is received.
