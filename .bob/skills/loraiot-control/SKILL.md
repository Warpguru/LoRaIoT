---
name: loraiot-control
description: >-
  Use when the user wants to verify that control device registration and setpoint push
  from LOCAL to REMOTE is working correctly - POSTs a control device on LOCAL, checks
  the owner view on LOCAL, verifies cache arrival on REMOTE, and inspects logs.
metadata:
  disable-model-invocation: false
---

# LoRaIoT Control Push Verification

Follow these steps to verify that control device registration on LOCAL pushes setpoints
over LoRa and updates REMOTE's control cache. Do not ask the user for information already
known below; proceed directly.

## Known constants

| Item | Value |
|---|---|
| LOCAL IP | `http://192.168.1.147` |
| REMOTE IP | `http://192.168.1.174` |
| LOCAL LoRa address | `0x01` |
| REMOTE LoRa address | `0x02` |
| Control POST endpoint | `POST http://192.168.1.147/api/lora/nodes/control?addr=2` |
| LOCAL Control GET endpoint | `GET http://192.168.1.147/api/lora/nodes/control?addr=2` |
| REMOTE Control GET endpoint | `GET http://192.168.1.174/api/lora/nodes/control` |

## Step 1 - Post a control device setpoint on LOCAL

Send a control setpoint update for peer `0x02` (REMOTE) to LOCAL:

```powershell
curl.exe -s -X POST -H "Content-Type: application/json" -d '{\"device\":\"Converter\",\"metrics\":{\"Voltage\":{\"value\":335,\"unit\":\"V\",\"scale\":100},\"Current\":{\"value\":150,\"unit\":\"A\",\"scale\":100}}}' "http://192.168.1.147/api/lora/nodes/control?addr=2"
```

Verify response status is HTTP 200 with `status: "OK"`.

## Step 2 - Verify LOCAL owner view

Read back the control registry from LOCAL:

```powershell
curl.exe -s "http://192.168.1.147/api/lora/nodes/control?addr=2"
```

Verify:
- Device `Converter` is present with metrics `Voltage` (value 335, scale 100) and `Current` (value 150, scale 100).
- `devices` array has valid entries.

## Step 3 - Verify REMOTE cache view

Read the cached control data from REMOTE:

```powershell
curl.exe -s "http://192.168.1.174/api/lora/nodes/control"
```

Verify:
- Device `Converter` is present in REMOTE's cache.
- Metrics `Voltage` and `Current` match the setpoint values.
- `age_ms` is small (e.g. < 5000 ms).
- `peer_last_seen_ms_ago` indicates recent communication with LOCAL.

## Step 4 - Fetch both node logs

Fetch logs to verify over-the-air frame exchange:

```powershell
curl.exe -s "http://192.168.1.147/api/log"
curl.exe -s "http://192.168.1.174/api/log"
```

Verify:
- LOCAL logged reliable push / DATA_ACK transmission to `0x02`.
- REMOTE logged DATA_ACK reception and ACK reply to `0x01`.
- LOCAL logged ACK reception matching the sequence number.
