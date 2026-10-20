# nanopb: Generating and Testing the Protobuf Sources

This document explains how `src/lora/proto/loraiot.proto` becomes the C sources in
`src/lora/src/nanopb/`, how to regenerate them, and how to test the result.

The schema itself is specified in [`LoRaProtocol.md`](LoRaProtocol.md) section 10. That section is the
source of truth; the `.proto` file is a copy and the generated files are derived from it.

> **Status:** the generated code is part of the `lora` library (so it is compiled by the Arduino IDE and
> PlatformIO), but no firmware code calls it yet. It is used from Milestone 4 on.

## 1. What is in the repository

| Path | Kind | Edit by hand? |
|---|---|---|
| `src/lora/proto/loraiot.proto` | Schema (proto3) | Yes, together with `LoRaProtocol.md` section 10 |
| `src/lora/proto/loraiot.options` | nanopb size limits (no heap) | Yes |
| `src/lora/src/nanopb/loraiot.pb.h`, `loraiot.pb.c` | **Generated** | **No**, regenerate |
| `src/lora/src/nanopb/pb.h`, `pb_common.*`, `pb_encode.*`, `pb_decode.*` | nanopb runtime 0.4.9.1 (zlib license, see `LICENSE.txt` next to it) | No, replace as a set |
| `tools/nanopb_test/check_proto.py` | Wire-size checks (Python) | Yes |
| `tools/nanopb_test/roundtrip.c` | Round trip of the generated C code on the PC | Yes |

The generated and runtime files are in `src/lora/src/nanopb/` because the Arduino IDE compiles only
`src/` of a library and only that folder (recursively) for sources. All of them include each other with
`"quotes"`, so they resolve inside one folder with no extra include path.

## 2. Prerequisites

Python 3 with these packages:

```
python -m pip install nanopb grpcio-tools
```

| Package | Used for |
|---|---|
| `nanopb` | The generator (`nanopb.generator.nanopb_generator`). It does **not** contain the C runtime |
| `grpcio-tools` | Provides `protoc`, so no separate compiler install is needed |
| `protobuf` | Installed automatically as a dependency; also used by `check_proto.py` |

Versions used for the committed files: nanopb 0.4.9.1, protobuf 7.36.2, grpcio-tools 1.84.0.
On this machine, `D:\Development\SetupEnvPython3.cmd` sets up PATH for Python 3.13.3; from PowerShell use
`cmd /c "D:\Development\SetupEnvPython3.cmd >nul 2>&1 & <command>"`.

The generator version and the runtime version must match (both 0.4.9.1). The generated header checks the
format with `#if PB_PROTO_HEADER_VERSION != 40` and fails to compile on a mismatch.

## 3. Regenerating the C sources

Run from the repository root:

```
python -m nanopb.generator.nanopb_generator -I src\lora\proto --error-on-unmatched -L quote -D src\lora\src\nanopb loraiot.proto
```

| Option | Why |
|---|---|
| `-I src\lora\proto` | Search path for `loraiot.proto` and `loraiot.options` (the options file is found by name) |
| `--error-on-unmatched` | Fails if a line in `.options` names a field that does not exist (catches typos and renamed fields) |
| `-L quote` | Writes `#include "pb.h"` instead of `<pb.h>`, so the Arduino IDE finds it in the same folder |
| `-D src\lora\src\nanopb` | Output directory. It must already exist |

Expected output: `Writing to src\lora\src\nanopb\loraiot.pb.h and src\lora\src\nanopb\loraiot.pb.c`.

When to regenerate: after any change to `loraiot.proto` or `loraiot.options`. Never edit `loraiot.pb.*` by hand.

### Changing a limit

A limit exists in three places that must agree:

1. the comment in `loraiot.proto` and in `LoRaProtocol.md` section 10 (character limit, for example `max 12 chars`)
2. `loraiot.options` (`max_size` = character limit **plus 1** for the terminating NUL; `max_count` = number of entries)
3. the constants in `tools/nanopb_test/check_proto.py` (`NAME_MAX`, `UNIT_MAX`, ...)

Current limits:

| Field | Characters / entries | `.options` value |
|---|---|---|
| `Metric.name` | 12 | `max_size:13` |
| `Metric.unit` | 5 | `max_size:6` |
| `Device.name` | 12 | `max_size:13` |
| `Echo.text` | 175 | `max_size:176` |
| `Device.metrics` | 8 entries | `max_count:8` |
| `Data.values` | 16 entries | `max_count:16` |

### Updating the nanopb runtime

Take the release archive of the **same version** as the generator from
`https://github.com/nanopb/nanopb/releases`, and copy `pb.h`, `pb_common.c/.h`, `pb_decode.c/.h`,
`pb_encode.c/.h` and `LICENSE.txt` into `src/lora/src/nanopb/`. Then regenerate (section 3).
Before updating, check that the release is current and maintained.

## 4. Testing the output

All tests run on the PC. None of them needs an ESP32, PlatformIO or the Arduino IDE.

### 4.1 Wire sizes (Python): does the schema fit one frame?

```
python tools\nanopb_test\check_proto.py
```

It compiles the `.proto` with `protoc`, builds worst-case messages, encodes them with the Python protobuf
library and compares the encoded length with the 186-byte payload limit of `LoRaProtocol.md` section 4.
Exit code 0 means all checks passed. Current result:

| Check | Size | Limit 186 B |
|---|---|---|
| PING payload equals the worked example `08 F8 AC D1 91 01` | 6 B | - |
| Optional `registry_rev = 0` is sent, absent is not | - | - |
| `Data`, 16 worst-case values | 171 B | fits |
| `Device`, 4 typical metrics | 100 B | fits |
| `Device`, 8 metrics at maximum name/unit length | 241 B | **does not fit (expected)** |
| `Response(Device)`, 4 metrics at maximum length | 138 B | fits |
| `Response(Data)`, 16 worst-case values | 180 B | fits |
| `Response(Echo)`, 175 characters | 185 B | fits |

The script also prints the largest `Device` that fits when every metric is at maximum length (currently
**5 metrics**). The "does not fit" row is a known limitation, not a failure: the schema bound of 8 metrics is
larger than what one frame can carry, so the REMOTE must reject a registration that would make its `Device`
too large (`LoRaProtocol.md` section 10). The check asserts that behaviour, so it fails if the limits are
ever changed so that 8 maximum-length metrics fit (then the rule can be relaxed).

"Worst case" means: every `sint32` at `-2147483648` (the longest zigzag varint), `uint32` fields at their
maximum, and strings at their maximum length. Measure with these values, not with typical ones; typical
values hide the length problem.

### 4.2 Round trip of the generated C code (PC compiler)

This compiles `loraiot.pb.c` and the nanopb runtime for the PC, encodes and decodes messages with the
**same C code that the ESP32 will run**, and compares the bytes.

A C compiler is needed. If none is installed, the Python package `ziglang` provides one:

```
python -m pip install ziglang
python -m ziglang cc -std=c99 -Wall -Wextra -Isrc\lora\src\nanopb -o roundtrip.exe tools\nanopb_test\roundtrip.c src\lora\src\nanopb\loraiot.pb.c src\lora\src\nanopb\pb_common.c src\lora\src\nanopb\pb_encode.c src\lora\src\nanopb\pb_decode.c
roundtrip.exe
```

(With an installed `gcc` or `clang`, replace `python -m ziglang cc` by `gcc`.) Expected last line:
`ALL CHECKS PASSED`, exit code 0.

A prebuilt `tools\nanopb_test\roundtrip.exe` (Windows x64) is kept in the repository, so the test can be run
without any compiler: `tools\nanopb_test\roundtrip.exe`. It is built from the generated sources as they were
when it was last compiled, so **rebuild it after every regeneration** (section 3), otherwise it tests stale code.
The linker also writes `roundtrip.pdb` (debug symbols); delete it, it is not needed.

Example output of `roundtrip.exe` (a line starting with `OK` is a passed check, a line starting with `FAIL`
is a failed one; the indented lines are information):

```
OK   encode PING
     PING payload: 08 F8 AC D1 91 01
OK   PING payload equals the documented bytes
OK   decode PING
OK   PING decodes with absent optional fields
OK   encode BEACON
OK   BEACON registry_rev=0 is present after decode
OK   encode Data ({"device":"Sensor A","data":{"Voltage":3.35,"Current":-0.12}})
     Data payload: 08 01 12 05 08 01 10 9E 05 12 05 08 02 10 EF 01
OK   Data round trip incl. negative sint32
OK   encode Response(Device)
     Response(Device) size: 98 B
OK   Response(Device) round trip
OK   struct sizes match the .options limits
OK   encode Metric with 12-character name
     loraiot_Device_size   = 300
     loraiot_Response_size = 311

ALL CHECKS PASSED
```

How to read the interesting lines:

- `PING payload: 08 F8 AC D1 91 01` is the protobuf encoding of `Beacon{boot_id = 0x12345678}`:
  `08` is the field tag (field 1, varint), and `F8 AC D1 91 01` is `0x12345678` as a varint (7 bits per byte,
  low bits first, high bit set on every byte except the last). The same 6 bytes appear in the frame of
  `LoRaProtocol.md` section 4.2, and `check_proto.py` produces them with the Python library.
- `Data payload: 08 01 12 05 08 01 10 9E 05 12 05 08 02 10 EF 01` is `Data{device=1, values=[{1,335},{2,-120}]}`:
  `08 01` is `device = 1`; each `12 05 ...` is one `Measurement` of 5 bytes (tag `12`, length `05`), whose
  `08 xx` is `metric_id` and `10 yy..` is the zigzag-encoded `sint32 value` (`9E 05` = 335 and `EF 01` = -120).
  No `age_ms` or `alert` bytes appear because they are 0/false (proto3 omits default values).
- `Response(Device) size: 98 B` is the encoded length of the definition of `Sensor A` with four metrics, well
  inside the 186-byte payload limit.
- `loraiot_Device_size` and `loraiot_Response_size` are the generated worst-case buffer sizes (section 5), not
  measurements.

The same check as an invocation from PowerShell, showing the exit code:

```
PS> .\roundtrip.exe | Select-Object -Last 2
PS> $LASTEXITCODE
0
```

What it verifies:

- the PING payload is `08 F8 AC D1 91 01`, identical to the Python result and to `LoRaProtocol.md` section 4.2
- optional fields: absent in a PING (`has_registry_rev == false`), present when set to 0
- `Data` with a negative `sint32` value survives a round trip
- `Response(Device)` with four metrics (`Sensor A`: Voltage, Current, Temperature, Humidity) round-trips; its
  size is 98 B
- the struct sizes match the limits (`name[13]`, `unit[6]`, `Device.name[13]`)
- the code compiles without warnings under `-Wall -Wextra`

### 4.3 Compile in the Arduino IDE or PlatformIO

Both builds compile the files in `src/lora/src/nanopb/` as part of the `lora` library. A successful build
shows that the files are found and compile for the ESP32.

**Verified in the Arduino IDE on an ESP32:** `runPingEncodeCheck()` in `src/diagnostics/src/Diagnostics.cpp`
calls the generated code at every boot and on `GET /api/diag/e220`. The device log (`GET /api/log`) shows:

```
Protobuf diag: ===== PING encode/decode check =====
Protobuf diag:   encoded PING payload (6 B): 08 F8 AC D1 91 01
Protobuf diag:   PASS payload equals the documented bytes
Protobuf diag:   PASS decode returns boot_id 12345678, optional fields absent
Protobuf diag: ===== RESULT: PASS. nanopb encode and decode verified =====
```

These are the same bytes as in the PC tests (sections 4.1 and 4.2) and in `LoRaProtocol.md` section 4.2, so the
firmware build and the PC build agree. A PlatformIO build has not been run.

If the build fails, check in this order:

1. `fatal error: pb.h: No such file` - the files are not in `src/lora/src/nanopb/` (or `-L quote` was not used).
2. `PB_PROTO_HEADER_VERSION` error - generator and runtime versions differ (section 2).
3. Multiple definition errors - the same runtime was also added as a library (`lib_deps` or the Library
   Manager); keep only one copy.

## 5. Worst-case sizes generated by nanopb

nanopb writes the maximum encoded size of every message into `loraiot.pb.h` (`loraiot_Device_size` and so on).
These are upper bounds that assume the longest possible encoding of every field, and are larger than the
values measured in section 4.1. Use them only to size buffers:

| Macro | Value |
|---|---|
| `loraiot_Data_size` | 238 |
| `loraiot_Device_size` | 300 |
| `loraiot_Response_size` | 311 |

`loraiot_Response_size` (311 B) is larger than the 186-byte frame payload. This is consistent with the
rule above: size the encode buffer from the macro, but check the **actual** encoded length
(`pb_ostream_t.bytes_written`) against 186 before sending.
