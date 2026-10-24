#pragma once

/**
 * index_html.h - PROGMEM wrapper for the LoRaIoT browser UI (GET /).
 *
 * Serve via: request->send_P(200, "text/html", INDEX_HTML);
 * This keeps the HTML in flash and never occupies DRAM at runtime.
 *
 * **DEPRECATION NOTICE:** This file is a convenience landing page carried
 * over from the pre-OpenAPI phase of the project. It will be removed once
 * a real browser UI is implemented, because:
 *   - GET /openapi/ui (Swagger UI) already provides a fully interactive,
 *     self-documenting explorer for every endpoint.
 *   - GET /openapi serves the machine-readable OpenAPI 3.0 spec.
 * Do not invest further in this file.
 */

static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>LoRaIoT Gateway</title>
  <style>
    body { font-family: sans-serif; max-width: 800px; margin: 40px auto; padding: 0 20px; color: #333; }
    h1 { border-bottom: 2px solid #eee; padding-bottom: 8px; }
    h2 { font-size: 1em; margin-top: 1.4em; color: #555; text-transform: uppercase; letter-spacing: .05em; }
    code { background: #f4f4f4; padding: 2px 6px; border-radius: 3px; font-size: .9em; }
    a { color: #007bff; text-decoration: none; }
    a:hover { text-decoration: underline; }
    ul { margin: 4px 0; padding-left: 1.4em; }
    li { margin: 3px 0; }
    .note { color: #888; font-size: .85em; }
  </style>
</head>
<body>
  <h1>LoRaIoT Gateway</h1>

  <h2>API Explorer</h2>
  <ul>
    <li><a href="/openapi/ui"><strong>Swagger UI</strong></a> - interactive API browser</li>
    <li><a href="/openapi"><code>GET /openapi</code></a> - raw OpenAPI 3.0 JSON spec</li>
  </ul>

  <h2>Diagnostics</h2>
  <ul>
    <li><a href="/status"><code>GET /status</code></a> - ESP32 hardware and WiFi info</li>
    <li><a href="/api/log"><code>GET /api/log</code></a> - last 128 log lines</li>
    <li><a href="/api/log?clear=1"><code>GET /api/log?clear=1</code></a> - clear log buffer</li>
    <li><a href="/api/tier"><code>GET /api/tier</code></a> - active gateway tier (LOCAL / REMOTE)</li>
    <li><a href="/api/diag/e220"><code>GET /api/diag/e220</code></a> - request E220 wiring check
        <span class="note">(result appears in /api/log after ~2 s)</span></li>
    <li><a href="/api/lora/channel"><code>GET /api/lora/channel</code></a> - active LoRa channel</li>
    <li><a href="/api/lora/airrate"><code>GET /api/lora/airrate</code></a> - active LoRa air data rate</li>
    <li><a href="/api/lora/address"><code>GET /api/lora/address</code></a> - active LoRa node address</li>
    <li><a href="/api/system/cpu"><code>GET /api/system/cpu</code></a> - active CPU frequency</li>
  </ul>

  <h2>LoRa Nodes <span class="note">(LOCAL only)</span></h2>
  <ul>
    <li><a href="/api/lora/nodes"><code>GET /api/lora/nodes</code></a> - list all known remote nodes</li>
    <li><a href="/api/lora/nodes/snapshot?addr=2"><code>GET /api/lora/nodes/snapshot?addr=2</code></a> - full device + metric snapshot for node 0x02</li>
    <li><a href="/api/lora/nodes/ping?addr=2"><code>GET /api/lora/nodes/ping?addr=2</code></a> - ping node 0x02</li>
    <li><a href="/api/lora/ping"><code>GET /api/lora/ping</code></a> - broadcast ping</li>
    <li><a href="/api/lora/stats"><code>GET /api/lora/stats</code></a> - TX/RX/CRC/ACK counters</li>
  </ul>

  <h2>Settings</h2>
  <ul>
    <li><a href="/reboot"><code>GET /reboot</code></a> - reboot ESP32 (settings preserved)</li>
    <li><a href="/reset"><code>GET /reset</code></a> - clear WiFi + NVS settings and reboot to captive portal
        <span class="note">(caution: requires re-provisioning)</span></li>
  </ul>
</body>
</html>
)rawhtml";
