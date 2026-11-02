# Windows PC monitor

[`Send-PcStats.ps1`](Send-PcStats.ps1) reports the CPU load, CPU temperature and RAM usage of a Windows PC to a LoRaIoT REMOTE node until you press Ctrl+C. The REMOTE forwards the values over LoRa to the LOCAL node, from where they are available via MQTT, WebSocket and REST.

## Usage

```powershell
.\Send-PcStats.ps1 <remote-node>
```

`<remote-node>` is the hostname or IP address of the REMOTE node: its LAN address, or `192.168.4.1` when your PC is connected to its SoftAP. No other parameter exists.

## What is reported

The device names are built from the computer name, shortened to 8 characters, because device names are limited to 12 characters.

| Device | Interval | Metrics |
|---|---|---|
| `<host>-CPU` | 1 s | `CpuLoad` (%), `CpuTemp` (C, only if available) |
| `<host>-MEM` | 10 s | `RamUsed` (%), `RamFree` (MB) |

CPU and RAM are separate devices on purpose: every POST sends the complete device as one LoRa frame, so a combined device would repeat the RAM values every second. The two devices use 2 of the 4 device slots of the REMOTE node.

All values are sent as scaled integers with `scale` 10, for example 37.4 % is `374`.

## CPU temperature

Windows has no reliable built-in CPU temperature. The script probes once at startup and uses the first source that works:

1. [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) running in the background (WMI `root\LibreHardwareMonitor`). This is the recommended source.
2. The ACPI thermal zone (WMI `root\wmi`). It needs an elevated PowerShell and is not a CPU sensor on every machine.

If neither works, `CpuTemp` is left out and everything else is reported as usual.

## Verify

On the LOCAL node, show the received values:

```powershell
curl.exe -s "http://<local-node>/api/lora/nodes/sensor?addr=<remote-lora-addr>"
```

The endpoint is described in [`doc/RestApi.md`](../../doc/RestApi.md#post-apilorasensordata). Values on the LOCAL node can lag by the LoRa transfer time. A lost frame is replaced by the next one.

## Notes

- Only the REMOTE node accepts sensor data. Sending to the LOCAL node returns `404`.
- Slots on the REMOTE node are allocated on the first POST and stay until the node reboots. If you rename the computer, reboot the node or you may run into the limit of 4 devices.
- Failed POSTs print a warning and the script keeps running.
- The 433 MHz band has regional duty-cycle rules. Check them before running the 1 s interval permanently.
