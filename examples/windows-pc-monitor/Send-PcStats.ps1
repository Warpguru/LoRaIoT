<#
.SYNOPSIS
  Reports the CPU load, CPU temperature and RAM usage of this Windows PC to a
  LoRaIoT REMOTE node until interrupted with Ctrl+C.

.DESCRIPTION
  Posts to POST /api/lora/sensor/data on the REMOTE node. The node forwards the
  values over LoRa to the LOCAL node (MQTT, WebSocket, REST).

  Two devices are reported, named after the host (shortened to 8 characters,
  because device names are limited to 12 characters):

    <host>-CPU   every 1 s    CpuLoad (%), CpuTemp (C, only if available)
    <host>-MEM   every 10 s   RamUsed (%), RamFree (MB)

  Each device is sent as one LoRa frame, so CPU and RAM are kept in separate
  devices: the slow RAM updates do not add airtime to the fast CPU updates.

  CPU temperature is optional. The script uses the first source that works:
    1. LibreHardwareMonitor (running in the background), WMI root\LibreHardwareMonitor
    2. ACPI thermal zone (root\wmi), usually needs an elevated PowerShell
  If neither works, CpuTemp is simply left out.

.PARAMETER Node
  Hostname or IP address of the REMOTE node, for example 192.168.1.174 or
  192.168.4.1 (SoftAP).

.EXAMPLE
  .\Send-PcStats.ps1 192.168.1.174
#>
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$Node
)

$ErrorActionPreference = 'Stop'

# ---- Constants -----------------------------------------------------------------

$URL                = "http://$Node/api/lora/sensor/data"
$CPU_INTERVAL_MS    = 1000     # CPU device period
$MEM_EVERY_N_TICKS  = 10       # RAM device period = 10 x CPU period
$HTTP_TIMEOUT_S     = 2
$HOST_NAME_MAX      = 8        # device name max 12 chars = host (8) + "-CPU" / "-MEM"
$SCALE_ONE_DECIMAL  = 10       # values are sent as integers: 37.4 -> 374 with scale 10
$KELVIN_OFFSET      = 273.15   # ACPI thermal zone reports tenths of Kelvin

# ---- Device names --------------------------------------------------------------

$shortHost = $env:COMPUTERNAME
if ($shortHost.Length -gt $HOST_NAME_MAX) {
  $shortHost = $shortHost.Substring(0, $HOST_NAME_MAX)
}
$CPU_DEVICE = "$shortHost-CPU"
$MEM_DEVICE = "$shortHost-MEM"

# ---- Sensors -------------------------------------------------------------------

# Returns CPU load in percent (0..100), all cores combined.
function Get-CpuLoad {
  $cpu = Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'"
  return [double]$cpu.PercentProcessorTime
}

# Returns @{ UsedPercent; FreeMB }.
function Get-Memory {
  $os = Get-CimInstance Win32_OperatingSystem
  $totalKB = [double]$os.TotalVisibleMemorySize
  $freeKB  = [double]$os.FreePhysicalMemory
  return @{
    UsedPercent = ($totalKB - $freeKB) * 100.0 / $totalKB
    FreeMB      = $freeKB / 1024.0
  }
}

# Temperature source readers. Each returns degrees Celsius or throws.
function Get-TempLibreHardwareMonitor {
  $sensors = Get-CimInstance -Namespace 'root\LibreHardwareMonitor' -ClassName Sensor `
    -Filter "SensorType='Temperature'" | Where-Object { $_.Name -like '*CPU*' -or $_.Name -like '*Core*' }
  $best = $sensors | Where-Object { $_.Name -like '*Package*' } | Select-Object -First 1
  if ($null -eq $best) {
    $best = $sensors | Select-Object -First 1
  }
  if ($null -eq $best) {
    throw 'no CPU temperature sensor'
  }
  return [double]$best.Value
}

function Get-TempAcpi {
  $zone = Get-CimInstance -Namespace 'root\wmi' -ClassName MSAcpi_ThermalZoneTemperature |
    Select-Object -First 1
  if ($null -eq $zone) {
    throw 'no thermal zone'
  }
  return ([double]$zone.CurrentTemperature / 10.0) - $KELVIN_OFFSET
}

# Probe once at startup and remember the first working source.
$tempReader = $null
foreach ($candidate in @('Get-TempLibreHardwareMonitor', 'Get-TempAcpi')) {
  try {
    $t = & $candidate
    if ($t -gt 0 -and $t -lt 150) {
      $tempReader = $candidate
      break
    }
  } catch {
    # try the next source
  }
}

# ---- HTTP ----------------------------------------------------------------------

# Builds one metric: value is the real reading, sent as scaled integer.
function New-Metric([double]$Real, [string]$Unit) {
  return @{
    value = [int][math]::Round($Real * $SCALE_ONE_DECIMAL)
    unit  = $Unit
    scale = $SCALE_ONE_DECIMAL
  }
}

# Posts one device. Returns $true on success; problems are reported, never thrown,
# so a node that is rebooting does not stop the script.
function Send-Device([string]$Device, [hashtable]$Metrics) {
  $body = @{ device = $Device; metrics = $Metrics } | ConvertTo-Json -Depth 5 -Compress
  try {
    Invoke-RestMethod -Uri $URL -Method Post -ContentType 'application/json' `
      -Body $body -TimeoutSec $HTTP_TIMEOUT_S | Out-Null
    return $true
  } catch {
    Write-Warning "POST $Device failed: $($_.Exception.Message)"
    return $false
  }
}

# ---- Main loop -----------------------------------------------------------------

Write-Host "Reporting to $URL"
Write-Host "Devices: $CPU_DEVICE (every $($CPU_INTERVAL_MS / 1000) s), $MEM_DEVICE (every $($CPU_INTERVAL_MS * $MEM_EVERY_N_TICKS / 1000) s)"
if ($null -ne $tempReader) {
  Write-Host "CPU temperature source: $tempReader"
} else {
  Write-Host 'CPU temperature: not available (start LibreHardwareMonitor or run elevated)'
}
Write-Host 'Press Ctrl+C to stop.'

$clock = [System.Diagnostics.Stopwatch]::StartNew()
$tick  = 0
try {
  while ($true) {
    $cpuMetrics = @{ CpuLoad = New-Metric (Get-CpuLoad) '%' }
    if ($null -ne $tempReader) {
      try {
        $cpuMetrics['CpuTemp'] = New-Metric (& $tempReader) 'C'
      } catch {
        # temperature is optional, skip this round
      }
    }
    $line = "{0:HH:mm:ss} cpu {1:N1}%" -f (Get-Date), ($cpuMetrics.CpuLoad.value / $SCALE_ONE_DECIMAL)
    if ($cpuMetrics.ContainsKey('CpuTemp')) {
      $line += " {0:N1}C" -f ($cpuMetrics.CpuTemp.value / $SCALE_ONE_DECIMAL)
    }
    Send-Device $CPU_DEVICE $cpuMetrics | Out-Null

    if (($tick % $MEM_EVERY_N_TICKS) -eq 0) {
      $mem = Get-Memory
      $memMetrics = @{
        RamUsed = New-Metric $mem.UsedPercent '%'
        RamFree = New-Metric $mem.FreeMB 'MB'
      }
      $line += " | ram {0:N1}% used, {1:N0} MB free" -f $mem.UsedPercent, $mem.FreeMB
      Send-Device $MEM_DEVICE $memMetrics | Out-Null
    }
    Write-Host $line

    # Fixed schedule (no drift): sleep until the next full CPU interval.
    $tick++
    $wait = ($tick * $CPU_INTERVAL_MS) - $clock.ElapsedMilliseconds
    if ($wait -gt 0) {
      Start-Sleep -Milliseconds $wait
    }
  }
} finally {
  Write-Host 'Stopped.'
}
