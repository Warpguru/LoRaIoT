---
name: loraiot-resources
description: Use when the user wants to check resource usage (RAM, heap, flash, task stacks, NVS) on LoRaIoT nodes — fetches live diagnostics via curl and produces a status table with OK/WARNING/CRITICAL ratings and mitigations.
---

# LoRaIoT Resource Check

Fetch live diagnostics from both nodes and produce a concise resource status table.
Do **not** ask the user for information already known below; proceed directly.

## Known constants

| Item | Value |
|---|---|
| LOCAL IP | `http://192.168.1.147` |
| REMOTE IP | `http://192.168.1.174` |

## Command syntax (Windows)

The shell is PowerShell, where `curl` is an alias for `Invoke-WebRequest` and rejects
`-s`. **Always call `curl.exe`** (e.g. `curl.exe -s --max-time 15 <url>`).

## Thresholds

Conventions: 1 KB = 1024 B. Ranges are lower-bound inclusive, so a value exactly on a
boundary takes the better rating of the two adjacent ranges (e.g. exactly 50 KB free heap = OK).

| Resource | OK | WARNING | CRITICAL |
|---|---|---|---|
| Free heap | >= 50 KB | 20–50 KB | < 20 KB |
| Min free heap (ever) | >= 30 KB | 15–30 KB | < 15 KB |
| Max alloc heap (largest contiguous block) | >= 40 KB | 15–40 KB | < 15 KB |
| Heap fragmentation % | < 30% | 30–60% | > 60% |
| Flash image used (`size / freeSpace`) | < 80% | 80–92% | > 92% |
| Flash headroom (`freeSpace - size`) | > 200 KB | 50–200 KB | < 50 KB |
| Task stack free min (per task) | > 1 KB | 400 B–1 KB | < 400 B |
| NVS (qualitative, no API counter) | entries present, no WARN/ERROR NVS lines | WARN line mentioning NVS | ERROR line mentioning NVS / open failure |

## Step 1 — Fetch diagnostics from both nodes

```
curl.exe -s --max-time 15 http://192.168.1.147/status
curl.exe -s --max-time 15 http://192.168.1.174/status
```

Parse each JSON response. Extract the following fields:

**From `memory`:**
- `freeHeap`, `minFreeHeap`, `maxAllocHeap`, `heapFragPct`

**From `sketch`:**
- `size` (image bytes), `freeSpace` (**size of the OTA slot, NOT the remaining bytes**:
  `ESP.getFreeSketchSpace()` returns the whole next-OTA partition, e.g. 1,310,720 B =
  1280 KB `app0`/`app1`)
- Slot size = `freeSpace`
- Used % = `size / freeSpace * 100`
- Headroom bytes = `freeSpace - size`

**From `tasks[]`:**
- For each entry: `name`, `stackBytes`, `freeBytesMin`
- `freeBytesMin` is already in bytes. (Older firmware also emitted an identical
  `freeWordMin`; ignore it if present.)

If the `tasks` array is absent or empty (older firmware without TaskRegistry),
note "task stack data not available" and skip that section.

If `heapFragPct` is absent (older firmware), derive it:
`fragPct = 100 - (maxAllocHeap * 100 / freeHeap)`.
When present, the firmware-reported value takes precedence (it may differ by about 1
point from the derived value due to rounding).

### Tier-specific tasks

The nodes run the same firmware but different tasks per tier. Only evaluate tasks that
belong to the node's tier; do not expect or compare a task on the other tier.

| Task | LOCAL | REMOTE |
|---|---|---|
| `StatusLed` | yes | yes |
| `WSBroadcast` | yes | yes |
| `LoRaTransTask` | yes | yes |
| `Ping` | yes | yes |
| `Push` | yes | yes |
| `Beacon` | no | yes |

If a task name appears that is not in this table, evaluate it and flag it as "not in
skill's task table" so the table can be updated. When a task is missing from a tier
where it is expected, report it as an issue.

## Step 2 — Check the log for NVS errors

```
curl.exe -s --max-time 15 http://192.168.1.147/api/log
curl.exe -s --max-time 15 http://192.168.1.174/api/log
```

The response is a single JSON object `{"level":..., "log":[...]}` containing the last 128
log lines, so do not use line-based `grep` on the raw body; split the `log` array first.

Only consider lines whose level is `[WARN ]` or `[ERROR]` and that contain `NVS`, `nvs`,
`Preferences`, or `could not be opened`:

- `[ERROR]` match → NVS = CRITICAL
- `[WARN ]` match → NVS = WARNING
- No such lines → NVS = OK. INFO lines such as `Gateway tier loaded from NVS: ...` are
  positive evidence that entries are present, not errors.

Caveat: the buffer holds only the last 128 lines, so old boot-time NVS errors may have
rolled out of it. Boot-time lines are present only on a recently booted node.

## Step 3 — Build the status table

Produce one table per node. Columns:

| Resource | Value | Status | Mitigation |
|---|---|---|---|

Populate using the thresholds above. Use this mitigation text verbatim:

| Resource | WARNING mitigation | CRITICAL mitigation |
|---|---|---|
| Free heap | Monitor; avoid dynamic allocations in new code | Move large buffers to `static`; audit `new`/`malloc` calls; reduce task stack sizes |
| Min free heap | Monitor boot sequence for spikes | As above; reduce LogBuffer size (TechDebt §1) |
| Max alloc heap | Heap is fragmenting; monitor | Reboot nodes periodically; avoid repeated malloc/free of varying sizes |
| Heap frag % | Monitor | As above for max alloc heap |
| Flash used | Resize partitions (TechDebt §6) | Resize partitions immediately; compress PROGMEM assets; lower core debug level |
| Flash headroom | Resize partitions (TechDebt §6) | As above; remove unused WebSocket code (TechDebt §3) |
| Task stack free min | Increase task stack constant | Increase stack; move large locals to `static` (TechDebt §4); redesign to reduce call depth |
| NVS | Check log for namespace errors | Ask the developer to re-provision the node (never call `GET /reset` yourself); verify NVS partition not moved |

## Step 4 — Summary verdict

After the per-node tables, add a one-line overall verdict:

- **ALL OK** — no resource is at WARNING or CRITICAL
- **ACTION RECOMMENDED** — at least one WARNING; list the resources
- **ACTION REQUIRED** — at least one CRITICAL; list the resources and the top mitigation

## Notes

- Flash slot size = `sketch.freeSpace` (the OTA partition size). Headroom is
  `freeSpace - size`. Do **not** compute `size + freeSpace`; that double counts the image
  and understates usage by roughly half.
- Task stack `freeBytesMin` is the closest the stack has ever come to its bottom since
  boot, i.e. a high-water mark. The `/status` response has no uptime field, so the
  sustained-operation time is not known from it. Treat the value as an **upper bound on
  the true minimum**: a task that has not yet exercised its deepest code path
  (e.g. a rarely taken error or retry path) will look healthier than it is. A low value is
  always a real finding; a high value after a recent boot is only provisional. If the
  node was recently booted (the log's earliest lines are still present, or REMOTE
  `BeaconTask ... uptime=Ns` lines show a small uptime), say so next to the stack rows.
- `heapFragPct` of 0% means `maxAllocHeap == freeHeap` — one large contiguous block,
  ideal. Values above 30% start to matter for large allocations (ArduinoJson docs,
  nanopb buffers).
- The nodes run identical firmware, so heap and flash figures are expected to be nearly
  identical; show both tables for completeness. Stack figures are **not** comparable
  across tiers because the roles differ (see "Tier-specific tasks"). A low stack on one
  node does not imply the same on the other.
