#pragma once

/**
 * index_html.h - PROGMEM wrapper for the LoRaIoT browser UI.
 *
 * Serve via: request->send_P(200, "text/html", INDEX_HTML);
 * This keeps the HTML in flash and never occupies DRAM at runtime.
 *
 * Replace the placeholder below with the actual HTML once the UI is designed.
 * The HTML should be self-contained (inline CSS + JS); no external assets.
 * The embedded JS should connect to ws://<host>/ws/data for live updates.
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
    code { background: #f4f4f4; padding: 2px 6px; border-radius: 3px; }
    a { color: #007bff; }
  </style>
</head>
<body>
  <h1>LoRaIoT Gateway</h1>
  <p>REST API and WebSocket gateway UI - not yet implemented.</p>
  <p>REST API: <a href="/api/state"><code>/api/state</code></a></p>
  <p>Diagnostics: <a href="/status"><code>/status</code></a> |
     <a href="/api/log"><code>/api/log</code></a></p>
  <p>WebSocket: <code>ws://&lt;host&gt;/ws/data</code></p>
</body>
</html>
)rawhtml";
