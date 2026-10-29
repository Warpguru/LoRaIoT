---
name: loraiot-sensor
description: >-
  Use when the user wants to verify that sensor data ingested on REMOTE is pushed
  over LoRa to LOCAL and cached correctly - POSTs sensor data on REMOTE, verifies
  REMOTE's own device view, checks LOCAL's cache, and inspects logs for the DATA_ACK
  exchange.
metadata:
  disable-model-invocation: false
---

# LoRaIoT Sensor Push Verification

Follow these steps to verify that sensor data posted to REMOTE propagates over LoRa
and arrives in LOCAL's sensor cache. Do not ask the user for information already
known below; proceed directly.

## Known constants

| Item | Value |
|---|---|
| LOCAL IP | `http://192.168.1.147` |
| REMOTE IP | `http://192.168.1.174` |
| LOCAL LoRa address | `0x01` |
| REMOTE LoRa address | determined from REMOTE log (default `0x02`; check `loraAddress=0x` in boot log) |
| Sensor POST endpoint | `POST http://192.168.1.174/api/lora/sensor/data` |
| REMOTE sensor GET endpoint | `GET http://192.168.1.174/api/lora/nodes/sensor` |
| LOCAL sensor GET endpoint | `GET http://192.168.1.147/api/lora/nodes/sensor?addr=<N>` |

## Step 1 - Determine REMOTE's current LoRa address

Fetch REMOTE's log and look for the boot line:

```powershell
curl.exe -s --max-time 15 "http://192.168.1.174/api/log"
```

Find: `LoRa config loaded: ... loraAddress=0x<N>` - note the address `N`. Use `N` as `addr`
in all subsequent LOCAL calls. If the log is not yet available, wait a few seconds and retry.

## Step 2 - Post sensor data to REMOTE

Write the JSON body to a temp file to avoid PowerShell quoting issues, then POST it:

```powershell
[System.IO.File]::WriteAllBytes(
  "$env:TEMP\sensor_body.json",
  [System.Text.Encoding]::UTF8.GetBytes(
    '{"device":"WxStation","metrics":{"Temp":{"value":215,"unit":"C","scale":10},"Humidity":{"value":650,"unit":"%","scale":10}}}'
  )
)
curl.exe -s -v --max-time 15 --data-binary "@$env:TEMP\sensor_body.json" -H "Content-Type: application/json" "http://192.168.1.174/api/lora/sensor/data"
```

Expected response: **HTTP 200** with body containing:
- `"accepted"` array listing `Temp` and `Humidity` with assigned `device_idx` and `metric_id`.
- `"rejected"` array is empty.
- `"registry_rev"` incremented from prior value (or 2 on a fresh boot).

A **HTTP 400** with a non-empty `rejected` array means some metrics were refused (device table
full, payload limit exceeded). A 400 with `"error":"invalid JSON"` means the request body was
mangled - retry using the temp-file approach above.

## Step 3 - Verify REMOTE's own device view

Read REMOTE's sensor registry (REMOTE is the owner of its own sensor devices):

```powershell
curl.exe -s --max-time 15 "http://192.168.1.174/api/lora/nodes/sensor"
```

Verify:
- Device `WxStation` is present with metrics `Temp` (value 215, scale 10) and
  `Humidity` (value 650, scale 10).
- `has_data` is `true` for the device.
- `online` is `true` and `peer_last_seen_ms_ago` is small (REMOTE sees LOCAL's PINGs).

## Step 4 - Wait for DATA_ACK delivery

REMOTE's PushTask pushes sensor devices to LOCAL on every BEACON cycle (~5 s). If the
device schema is new (first POST after boot), REMOTE sends the DATA_ACK frame and LOCAL
will reply with `status=1` (schema unknown) and schedule a Describe crawl. After the crawl
completes LOCAL re-accepts with `status=0`. Allow up to ~15 s for the full exchange.

```powershell
Start-Sleep -Seconds 8
```

## Step 5 - Verify LOCAL's sensor cache

Read LOCAL's cached sensor view for REMOTE's address:

```powershell
curl.exe -s --max-time 15 "http://192.168.1.147/api/lora/nodes/sensor?addr=<N>"
```

Verify:
- Device `WxStation` is present with `Temp` (value 215) and `Humidity` (value 650).
- `has_data` is `true` and `age_ms` is reasonable (e.g. < 30 000 ms).
- `online` is `true` for REMOTE's node entry.
- `peer_last_seen_ms_ago` is small.

## Step 6 - Fetch both node logs and verify DATA_ACK exchange

```powershell
curl.exe -s --max-time 15 "http://192.168.1.174/api/log"
curl.exe -s --max-time 15 "http://192.168.1.147/api/log"
```

### Expected REMOTE log lines (in order)

| # | Expected substring | Notes |
|---|---|---|
| 1 | `PushTask: LoRa gate open - starting push loop` | PushTask ready |
| 2 | `PushTask: pushing 1 devices to 0x01` | outbound push triggered |
| 3 | `LoRa TX: ... AA 55 01 05 ...` | DATA_ACK frame sent (type byte `05`) |
| 4 | `loraSendDataReliable: ACK received seq=N status=0` | LOCAL acknowledged with STATUS_OK |
| 5 | `PushTask: pushed device=1 to 0x01 OK` | delivery confirmed |

On a first-time schema exchange, step 4 will first show `status=1` (LOCAL needs Describe), then
after the crawl a second TX with `status=0`. Both are normal - only flag if `status=0` never
arrives.

### Expected LOCAL log lines (in order)

| # | Expected substring | Notes |
|---|---|---|
| 1 | `DATA_ACK from 0x<N> device=1 values=2 stored` | values written to LOCAL's cache |
| 2 | `DATA_ACK ACK sent to 0x<N> acked_seq=M status=0` | LOCAL replied STATUS_OK |

On first-time exchange LOCAL will first log:
`DATA_ACK from 0x<N> device=1: schema unknown, Describe crawl scheduled`
and reply `status=1`. The `values=2 stored` line and `status=0` reply appear after the crawl.

## Step 7 - Report

Write a structured report with:

- **Verdict table** - one row per component (REMOTE POST, REMOTE device view, LOCAL cache,
  DATA_ACK log exchange): PASS / FAIL / PARTIAL
- **Evidence** - for each check in Steps 2-6, the exact log line or REST response field
- **Bugs** - any unexpected behaviour with log evidence and a recommended fix
- **Known transients** - mention but do not flag as bugs (see below)

## Known transients (do not flag as bugs)

- **First-boot schema negotiation (HTTP 202 / status=1):** On first POST after a reboot REMOTE's
  PushTask sends DATA_ACK and LOCAL replies `status=1` (schema unknown, crawl scheduled). REMOTE
  retries after the crawl completes with `status=0`. Only flag if `status=0` never arrives.
- **PushTask: pushing 0 devices / reason=0:** Logged before any sensor data has been posted.
  Normal startup behaviour.
- **Describe crawl timeout retry:** A single `DescribeCrawler: timeout ... retry 1/2` is a
  half-duplex collision between the crawl REQUEST and an in-flight PING or BEACON. Self-corrects
  on retry. Flag only if the crawl never completes.
- **DATA_ACK duplicate / re-sending ACK:** REMOTE may retransmit if the ACK is delayed by a
  BEACON. LOCAL correctly detects the duplicate and re-sends its ACK with the same status.
  Safe and expected.
