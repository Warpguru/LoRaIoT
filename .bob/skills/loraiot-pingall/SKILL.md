---
name: loraiot-pingall
description: Use when the user wants to ping all known LoRaIoT remote nodes and verify each one responds — discovers the node list automatically, pings each node in sequence, and reports RTT and boot_id per node.
---

# LoRaIoT Ping-All Verification

Ping every known remote node and report the result. Nodes are discovered automatically
from the registry — no hardcoded addresses. Do **not** ask the user for information
already known below; proceed directly.

## Known constants

| Item | Value |
|---|---|
| LOCAL IP | `http://192.168.1.147` |
| Expected RTT range | 80–200 ms (LoRa air time + 50 ms E220 guard) |

## Why broadcast ping cannot replace per-node pings

`GET /api/lora/ping` sends to `0xFF` (broadcast) but the broker returns on the **first
ACK only** — subsequent ACKs from other nodes are discarded. More importantly, with
multiple REMOTEs all receiving the same broadcast simultaneously they all transmit their
ACK at the same moment, colliding on the half-duplex RF channel. Sending the broadcast
multiple times does not help: each broadcast triggers the same simultaneous collision
from all REMOTEs — the strongest signal wins every time, typically the same nearest
node. Collecting all responses would require a random ACK backoff on REMOTE plus a
multi-slot broker on LOCAL (see `doc/TechDebt.md §5`).

The broadcast ping is therefore only useful as a quick "is anyone at all alive?" check.
Per-node pings are the only reliable way to verify every node individually.

## Step 1 — Quick broadcast ping (sanity check only)

Send a single broadcast PING as a fast alive-check before querying nodes individually:

```
curl -s http://192.168.1.147/api/lora/ping
```

Expected: HTTP 200, `{"addr":<N>,"rtt_ms":<N>,"boot_id":<N>}`.
HTTP 504 means no node answered at all — stop here and investigate the LoRa link.

This only confirms at least one node is alive. It does **not** confirm all nodes are
reachable. Proceed to Step 2 for the full per-node check.

## Step 2 — Discover all known nodes

```
curl -s http://192.168.1.147/api/lora/nodes
```

Parse the JSON array. For each entry, note:
- `addr` — the LoRa node address (used in Step 3)
- `online` — whether LOCAL currently considers the node reachable
- `boot_id` — expected to match the ping response in Step 3

If the list is empty, LOCAL has not received a BEACON from any node yet.
Wait ~10 s and retry (BEACONs arrive every 5–60 s depending on discovery state).

## Step 3 — Ping each node individually

For every `addr` returned in Step 2, run the unicast ping sequentially:

```
curl -s "http://192.168.1.147/api/lora/nodes/ping?addr=<addr>"
```

Concurrent HTTP calls are serialised automatically by the `async_tcp` web server task —
they do not race each other in the broker. HTTP 503 is only returned when a background
FreeRTOS task (PingTask or applicationLoop keep-alive) holds the broker slot at the
exact moment a REST call arrives. This is rare; if it occurs, wait 2 s and retry.
With N nodes and ~300 ms LoRa round-trip each, N=5 takes about 1.5 s total.

Expected per node: HTTP 200, `{"addr":<N>,"rtt_ms":<N>,"boot_id":<N>}`.

Record the result in a table (see Step 4).

## Step 4 — Build the results table

| addr | online (registry) | ping result | rtt_ms | boot_id match | verdict |
|---|---|---|---|---|---|

**boot_id match:** compare the `boot_id` in the ping response against the `boot_id`
from Step 2. A mismatch means the node rebooted between the last BEACON and the ping —
not a failure, but worth noting.

**Verdict per node:**
- **PASS** — HTTP 200, RTT within 80–200 ms, boot_id matches registry
- **PASS (boot_id mismatch)** — HTTP 200, RTT OK, but boot_id changed (node rebooted)
- **FAIL** — HTTP 504 (no response) or HTTP 503 (broker busy)

## Step 5 — Report

Write a summary with:

- **Broadcast result** (Step 1) — which node answered first, RTT
- **Per-node table** (Step 4) — one row per node
- **Overall verdict** — PASS if all nodes returned HTTP 200; FAIL if any node timed out
- **Any anomalies** — 504s, boot_id mismatches, nodes marked offline in registry but
  responding to ping (or vice versa)

## Known transients (do not flag as bugs)

- **HTTP 504 on broadcast ping (Step 1):** transient RF collision; retry the broadcast
  once before declaring total failure. If it fails twice, skip to Step 2 — the per-node
  pings use unicast and are unaffected.
- **HTTP 503 on per-node ping:** a background FreeRTOS task (PingTask or applicationLoop
  keep-alive) holds the broker slot at the exact moment the REST call enters
  `loraBrokerSend()`. Concurrent REST calls do NOT cause 503 — the web server serialises
  them on a single task. Wait 2 s and retry.
- **RTT slightly above 200 ms:** acceptable if REMOTE was mid-transmission (E220
  half-duplex); flag only if consistently above 300 ms.
- **broadcast returns addr of a different node than expected:** normal — the closest
  node wins the collision. This is expected behaviour, not a bug.
