#pragma once

/**
 * index_html.h - PROGMEM wrapper for the LoRaIoT real-time dashboard (GET /).
 *
 * Serve via: request->send_P(200, "text/html", INDEX_HTML);
 * This keeps the HTML in flash and never occupies DRAM at runtime.
 *
 * The page is self-contained (all CSS and JS inline, no external assets) and
 * connects to ws://<host>/ws/data (WS_PATH in Server.h). The JSON payload it
 * renders is produced by WebSocketService.cpp broadcastTask (see the schema in
 * the header comment of that file).
 */

static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>LoRaIoT Dashboard</title>
  <style>
    * { box-sizing: border-box; }
    body {
      font-family: -apple-system, "Segoe UI", system-ui, sans-serif;
      font-size: 14px; line-height: 1.6;
      background: #ffffff; color: #1f2328;
      max-width: 860px; margin: 0 auto; padding: 16px;
    }
    h1 { font-size: 1.3rem; margin: 0; font-weight: 600; }
    h2 {
      font-size: .8rem; margin: 20px 0 8px; color: #57606a;
      text-transform: uppercase; letter-spacing: .05em; font-weight: 600;
    }
    a { color: #3b82d4; text-decoration: none; }
    a:hover { text-decoration: underline; }
    .muted { color: #57606a; }
    .header {
      display: flex; align-items: center; gap: 8px; flex-wrap: wrap;
      padding-bottom: 10px; border-bottom: 1px solid #e5e7eb;
    }
    .badge {
      display: inline-block; padding: 1px 10px; border-radius: 10px;
      font-size: .8rem; background: #f7f8fa; border: 1px solid #e5e7eb; color: #57606a;
    }
    .badge.ok   { background: #e8f5e9; border-color: #a5d6a7; color: #1b5e20; }
    .badge.bad  { background: #fdecea; border-color: #f5b5b0; color: #8a1c14; }
    .badge.tier { background: #eaf2fc; border-color: #b7d3f3; color: #1f5fa8; }
    #wsActivity {
      margin-left: auto; font-size: 1.1rem; color: #3b82d4;
      opacity: .15; transition: opacity .15s;
    }
    #wsActivity.active { opacity: 1; }
    .stats { margin-top: 8px; display: flex; gap: 16px; flex-wrap: wrap; }
    .stat b { font-variant-numeric: tabular-nums; }
    .card {
      background: #f7f8fa; border: 1px solid #e5e7eb; border-radius: 6px;
      padding: 10px 14px; margin-bottom: 10px;
    }
    .node-head { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; font-weight: 600; }
    .device { margin-top: 8px; padding-top: 6px; border-top: 1px solid #e5e7eb; }
    .device-name { font-weight: 600; }
    /* Fixed layout + explicit column widths: columns stay at the same position in every
       table and across updates, regardless of the text length of the current values. */
    table { border-collapse: collapse; width: 100%; margin-top: 4px; table-layout: fixed; }
    td { padding: 2px 8px 2px 0; font-variant-numeric: tabular-nums; }
    td.name { width: 40%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    td.val { width: 30%; text-align: right; font-weight: 600; white-space: nowrap; }
    td.age { width: 30%; text-align: right; color: #57606a; white-space: nowrap; padding-right: 0; }
    hr.rule { border: 0; border-top: 1px solid #e5e7eb; margin: 20px 0 0; }
    .links { margin-top: 20px; padding-top: 10px; border-top: 1px solid #e5e7eb; display: flex; gap: 16px; flex-wrap: wrap; }
  </style>
</head>
<body>
  <div class="header">
    <h1>LoRaIoT Dashboard</h1>
    <span class="badge tier" id="tier">-</span>
    <span class="badge bad" id="wsStatus">Disconnected</span>
    <span class="muted">Uptime <span id="uptime">-</span></span>
    <span id="wsActivity" title="WebSocket activity">&#9889;</span>
  </div>
  <h2>Nodes</h2>
  <div id="nodes"><div class="muted">No nodes discovered yet</div></div>

  <hr class="rule">
  <h2>Communication</h2>
  <div class="stats muted">
    <span class="stat">TX <b id="txFrames">-</b></span>
    <span class="stat">RX <b id="rxFrames">-</b></span>
    <span class="stat">CRC errors <b id="crcErrors">-</b></span>
    <span class="stat">ACK loss <b id="ackLoss">-</b></span>
  </div>

  <div id="echoSection" style="display:none">
    <h2>Last Echo</h2>
    <div class="card" id="echo"></div>
  </div>

  <div class="links">
    <a href="/openapi/ui">API Explorer</a>
    <a href="/status">Status</a>
    <a href="/api/log">Log</a>
  </div>

  <script>
    /* ---- Constants ---- */
    var WS_PATH            = '/ws/data';  /* must match WS_PATH in Server.h */
    var RECONNECT_MIN_MS   = 1000;
    var RECONNECT_MAX_MS   = 30000;
    var ACTIVITY_FLASH_MS  = 250;
    var STALE_TIMEOUT_MS   = 3000;  /* server broadcasts every 1 s: 3 s of silence = link lost */
    var WATCHDOG_INTERVAL_MS = 1000;
    var SCALE_DECIMALS     = 2;
    var MS_PER_SECOND      = 1000;
    var SECONDS_PER_MINUTE = 60;
    var SECONDS_PER_HOUR   = 3600;
    var SECONDS_PER_DAY    = 86400;

    /* ---- DOM references ---- */
    var tierEl       = document.getElementById('tier');
    var wsStatusEl   = document.getElementById('wsStatus');
    var uptimeEl     = document.getElementById('uptime');
    var txEl         = document.getElementById('txFrames');
    var rxEl         = document.getElementById('rxFrames');
    var crcEl        = document.getElementById('crcErrors');
    var ackEl        = document.getElementById('ackLoss');
    var nodesEl      = document.getElementById('nodes');
    var echoSection  = document.getElementById('echoSection');
    var echoEl       = document.getElementById('echo');
    var wsActivityEl = document.getElementById('wsActivity');
    var wsActivityTimer = null;
    var lastData = null;  /* last received payload, re-rendered as stale when the link drops */

    /* Flash the activity symbol briefly - called on every received message. */
    function flashActivity() {
      wsActivityEl.classList.add('active');
      clearTimeout(wsActivityTimer);
      wsActivityTimer = setTimeout(function () { wsActivityEl.classList.remove('active'); }, ACTIVITY_FLASH_MS);
    }

    /* ---- Formatting helpers ---- */
    function formatDuration(totalSeconds) {
      var s = Math.floor(totalSeconds);
      if (s >= SECONDS_PER_DAY) {
        return Math.floor(s / SECONDS_PER_DAY) + 'd ' + Math.floor((s % SECONDS_PER_DAY) / SECONDS_PER_HOUR) + 'h';
      }
      if (s >= SECONDS_PER_HOUR) {
        return Math.floor(s / SECONDS_PER_HOUR) + 'h ' + Math.floor((s % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE) + 'm';
      }
      if (s >= SECONDS_PER_MINUTE) {
        return Math.floor(s / SECONDS_PER_MINUTE) + 'm ' + (s % SECONDS_PER_MINUTE) + 's';
      }
      return s + 's';
    }

    function formatAge(ms) {
      return formatDuration(ms / MS_PER_SECOND) + ' ago';
    }

    function hexAddr(addr) {
      var h = Number(addr).toString(16).toUpperCase();
      return '0x' + (h.length < 2 ? '0' + h : h);
    }

    function formatValue(metric) {
      if (!metric || metric.value === null || metric.value === undefined) {
        return '-';
      }
      if (typeof metric.value !== 'number') {
        return String(metric.value);
      }
      var scale = metric.scale ? metric.scale : 1;
      var scaled = metric.value / scale;
      if (isNaN(scaled) || !isFinite(scaled)) {
        return String(metric.value);
      }
      return scaled.toFixed(SCALE_DECIMALS);
    }

    /* ---- DOM builders (textContent only: node/device names come from the radio link) ---- */
    function el(tag, className, text) {
      var e = document.createElement(tag);
      if (className) {
        e.className = className;
      }
      if (text !== undefined) {
        e.textContent = text;
      }
      return e;
    }

    function renderDevice(dev) {
      var box = el('div', 'device');
      box.appendChild(el('div', 'device-name', dev.name));
      var table = el('table');
      var metrics = dev.metrics || [];
      for (var m = 0; m < metrics.length; m++) {
        var metric = metrics[m];
        var tr = el('tr');
        tr.appendChild(el('td', 'name', metric.name));
        tr.appendChild(el('td', 'val', formatValue(metric) + (metric.unit ? ' ' + metric.unit : '')));
        tr.appendChild(el('td', 'age', dev.has_data && metric.value !== null && metric.value !== undefined ? formatAge(dev.age_ms) : ''));
        table.appendChild(tr);
      }
      box.appendChild(table);
      return box;
    }

    /* stale=true: the WebSocket link is down, so online/last-seen are the last known values only. */
    function renderNodes(nodes, stale) {
      nodesEl.textContent = '';
      if (!nodes || nodes.length === 0) {
        nodesEl.appendChild(el('div', 'muted', 'No nodes discovered yet'));
        return;
      }
      for (var n = 0; n < nodes.length; n++) {
        var node = nodes[n];
        var card = el('div', 'card');
        var head = el('div', 'node-head');
        head.appendChild(el('span', '', 'Node ' + hexAddr(node.addr)));
        if (stale) {
          head.appendChild(el('span', 'badge', 'unknown'));
          head.appendChild(el('span', 'muted', 'link lost, last seen ' + formatAge(node.peer_last_seen_ms_ago) + ' at last update'));
        } else {
          head.appendChild(el('span', node.online ? 'badge ok' : 'badge bad', node.online ? 'online' : 'offline'));
          head.appendChild(el('span', 'muted', 'last seen ' + formatAge(node.peer_last_seen_ms_ago)));
        }
        card.appendChild(head);
        var devices = node.devices || [];
        for (var d = 0; d < devices.length; d++) {
          card.appendChild(renderDevice(devices[d]));
        }
        nodesEl.appendChild(card);
      }
    }

    function renderEcho(echo) {
      if (!echo) {
        echoSection.style.display = 'none';
        return;
      }
      echoSection.style.display = '';
      echoEl.textContent = '';
      var head = el('div', 'node-head');
      head.appendChild(el('span', echo.success ? 'badge ok' : 'badge bad', echo.success ? 'success' : 'failed'));
      head.appendChild(el('span', 'muted', 'RTT ' + echo.round_trip_ms + ' ms, seq ' + echo.seq_no));
      echoEl.appendChild(head);
      echoEl.appendChild(el('div', '', 'Sent: ' + echo.sent));
      echoEl.appendChild(el('div', '', 'Received: ' + echo.received));
    }

    function render(d) {
      lastData = d;
      tierEl.textContent = d.tier;
      uptimeEl.textContent = formatDuration(d.uptime_s);
      if (d.lora) {
        txEl.textContent  = d.lora.tx_frames;
        rxEl.textContent  = d.lora.rx_frames;
        crcEl.textContent = d.lora.crc_errors;
        ackEl.textContent = d.lora.ack_loss;
      }
      renderNodes(d.nodes);
      renderEcho(d.echo);
    }

    /* ---- WebSocket with exponential back-off reconnect ---- */
    var ws;
    var reconnectDelay = RECONNECT_MIN_MS;
    var reconnectTimer = null;
    var linkUp         = false;  /* true between a received open and link loss */
    var lastMessageAt  = 0;      /* Date.now() of the last received message */

    function setStatus(connected) {
      linkUp = connected;
      wsStatusEl.className = connected ? 'badge ok' : 'badge bad';
      wsStatusEl.textContent = connected ? 'Connected' : 'Disconnected';
      if (!connected && lastData) {
        renderNodes(lastData.nodes, true);
      }
    }

    function scheduleReconnect() {
      if (reconnectTimer !== null) {
        return;
      }
      reconnectTimer = setTimeout(function () {
        reconnectTimer = null;
        reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_MAX_MS);
        connect();
      }, reconnectDelay);
    }

    function connect() {
      ws = new WebSocket('ws://' + window.location.host + WS_PATH);

      ws.onopen = function () {
        reconnectDelay = RECONNECT_MIN_MS;
        lastMessageAt = Date.now();
        setStatus(true);
      };

      ws.onmessage = function (event) {
        lastMessageAt = Date.now();
        flashActivity();
        try {
          render(JSON.parse(event.data));
        } catch (e) {
          /* Ignore malformed frames; the next broadcast replaces the view. */
        }
      };

      ws.onclose = function () {
        setStatus(false);
        scheduleReconnect();
      };

      /* On any error close the socket - onclose schedules the reconnect. */
      ws.onerror = function () { ws.close(); };
    }

    /* Watchdog: a powered-off or unplugged node sends no TCP close, so onclose never fires and
       the page would show stale data as live. If the 1 s broadcast stops, drop the socket
       ourselves (handlers detached so the late onclose cannot double-schedule) and reconnect. */
    setInterval(function () {
      if (linkUp && Date.now() - lastMessageAt > STALE_TIMEOUT_MS) {
        ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null;
        ws.close();
        setStatus(false);
        scheduleReconnect();
      }
    }, WATCHDOG_INTERVAL_MS);

    connect();
  </script>
</body>
</html>
)rawhtml";
