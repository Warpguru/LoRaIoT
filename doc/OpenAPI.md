# LoRaIoT OpenAPI Implementation

This document describes the OpenAPI 3.0 specification and Swagger UI serving
infrastructure for the LoRaIoT firmware. It covers the design decisions made,
the memory layout chosen, and the tier-filtering mechanism.

---

## 1. Source files

| File | Purpose |
|---|---|
| `src/LoRaIoT/src/openapi_json.h` | Two PROGMEM symbols: `OPENAPI_JSON` (the full spec) and `OPENAPI_UI_TEMPLATE` (the Swagger UI HTML template) |
| `src/LoRaIoT/src/Server.cpp` | `handleGetOpenApiJson()` and `handleGetOpenApiUi()` handlers; routes registered in `setupServer()` |
| `doc/RestApi.md` | Human-readable companion to the spec. Every endpoint must be documented in both files in the same change. When they diverge, `openapi_json.h` is the source of truth. |

---

## 2. Endpoints

| Method | Path | Description |
|---|---|---|
| `GET` | `/openapi` | Returns the full `OPENAPI_JSON` PROGMEM string as `application/json`. Streamed directly from flash via `send_P()` - zero DRAM cost. |
| `GET` | `/openapi/ui` | Returns the Swagger UI HTML page. Tier-aware: operations not applicable to the running tier are hidden in the browser. |

---

## 3. PROGMEM layout

Both symbols live in `openapi_json.h` and are stored in the ESP32 flash
`.rodata` section. Neither occupies DRAM at idle.

### `OPENAPI_JSON`

A hand-authored OpenAPI 3.0.0 JSON document stored as a C raw string literal:

```cpp
static const char OPENAPI_JSON[] PROGMEM = R"rawjson(
{
  "openapi": "3.0.0",
  ...
}
)rawjson";
```

Approximate size: **~33 KB** in flash. Served verbatim by `send_P()` - the
full spec including `x-tier` extension fields is always returned to any
consumer (`curl`, Home Assistant, other tooling).

**Do not minify.** The JSON is hand-edited as endpoints are added. Minifying
saves ~9.5 KB of flash (29%) but makes every future edit error-prone. Flash is
not under pressure at current usage levels; revisit only if flash exceeds ~95%.

### `OPENAPI_UI_TEMPLATE`

An HTML/JS template also stored in PROGMEM. Approximate size: **~1,147 bytes**
in flash. It contains four `%s` placeholders expanded at request time via
`snprintf` into a stack-allocated buffer:

| Placeholder | Value |
|---|---|
| `%s` (1st) | `WiFi.SSID()` - connected network name |
| `%s` (2nd) | `Gateway.getTierName()` - tier shown in the banner |
| `%s` (3rd) | `__DATE__ " " __TIME__` - firmware build timestamp |
| `%s` (4th) | `Gateway.getTierName()` - tier injected as JS `TIER` variable |

The IP address is **not** server-injected. Instead, a one-line JS snippet sets
it from `location.hostname` at page load:

```javascript
document.getElementById('ip').textContent = location.hostname;
```

This is correct for both current STA mode (LAN DHCP IP) and future SoftAP mode
(192.168.4.1) - the browser always used the right address to reach the page.

---

## 4. Swagger UI handler memory model

The original handler built the HTML page using Arduino `String` concatenation
(`+` operator). Each `+` call on a `String` that grows triggers a heap
`realloc`, copying the existing content to a new allocation and freeing the
old one. With six concatenations the peak heap usage during a single
`/openapi/ui` request reached ~1,400 bytes, with multiple intermediate
allocations causing heap fragmentation.

The replacement uses a fixed stack buffer:

```cpp
static void handleGetOpenApiUi(AsyncWebServerRequest* request) {
  String ssid = WiFi.SSID();   // one small heap alloc for the SSID string
  char buf[1250];              // stack - freed when function returns
  snprintf(buf, sizeof(buf), OPENAPI_UI_TEMPLATE,
           ssid.c_str(),
           Gateway.getTierName(),
           __DATE__ " " __TIME__,
           Gateway.getTierName());
  request->send(HTTP_STATUS_OK, "text/html", buf);
}
```

`WiFi.SSID()` is extracted to a named `String` local so that `.c_str()`
is safe for the lifetime of the `snprintf` call. After that the `String`
destructor runs immediately and frees the SSID heap allocation.

### Buffer sizing

| Quantity | Bytes |
|---|---|
| Template static content | ~1,147 |
| Max SSID (32 chars) | 32 |
| Tier name ("REMOTE", 6 chars) × 2 | 12 |
| Build timestamp (`__DATE__ " " __TIME__`, 20 chars) | 20 |
| Worst-case total | ~1,211 |
| `buf` allocated | 1,250 |
| Headroom | ~39 |

The ESPAsyncWebServer request callback task has an 8 KB stack. A 1,250-byte
local buffer consumes 15% of it - well within the safe range.

---

## 5. Tier-aware filtering (`x-tier` extension)

### Motivation

The LoRaIoT firmware runs on two node types - LOCAL and REMOTE - from a single
shared codebase. Several API endpoints are only meaningful on one tier (e.g.
`GET /api/lora/nodes` only works on LOCAL; `POST /api/sensor/data` only works
on REMOTE). Without filtering, the Swagger UI on a REMOTE node shows LOCAL-only
endpoints that will always return 400 or do nothing.

### OpenAPI extension field

Every tier-specific operation carries an `x-tier` extension field at the
operation level (inside the HTTP method object, not the path item):

```json
"/api/lora/nodes": {
  "get": {
    "x-tier": "LOCAL",
    "tags": ["LoRa-Nodes"],
    ...
  }
}
```

Valid values are `"LOCAL"` and `"REMOTE"`. Operations available on both tiers
carry no `x-tier` field and are always shown.

Current annotations:

| Value | Count | Tag |
|---|---|---|
| `"LOCAL"` | 11 operations | `LoRa-Nodes`, `LoRa-Control` |
| `"REMOTE"` | 1 operation | `Sensor` |
| *(none - universal)* | remaining operations | `Gateway`, `Diagnostics`, `System` |

Tag descriptions in the `"tags"` array do **not** repeat tier information -
that is redundant with `x-tier` on each operation. Tag descriptions are plain
labels only (e.g. `"Remote node registry and telemetry"`, not
`"... (LOCAL only)"`).

`GET /openapi` always returns the **complete** spec including all `x-tier`
fields. Filtering is a presentation concern for the Swagger UI only; any
non-browser consumer receives the full spec and can use `x-tier` to drive its
own logic.

### Swagger UI `tierPlugin`

The filtering is implemented as a Swagger UI plugin embedded in
`OPENAPI_UI_TEMPLATE`. It uses the `statePlugins.spec.wrapActions.updateJsonSpec`
hook, which intercepts the parsed spec object before any rendering occurs.
Operations whose `x-tier` does not match the running `TIER` variable are
deleted from the spec object; path items that become empty are also deleted.

```javascript
var TIER = 'REMOTE';   // injected by snprintf from Gateway.getTierName()

function tierPlugin() {
  return {
    statePlugins: {
      spec: {
        wrapActions: {
          updateJsonSpec: function(oriAction) {
            return function(spec) {
              Object.keys(spec.paths || {}).forEach(function(path) {
                var item = spec.paths[path];
                ['get', 'post'].forEach(function(m) {
                  if (item[m] && item[m]['x-tier'] && item[m]['x-tier'] !== TIER)
                    delete item[m];
                });
                if (Object.keys(item).length === 0) delete spec.paths[path];
              });
              return oriAction(spec);
            };
          }
        }
      }
    }
  };
}
```

The HTTP methods array is limited to `['get', 'post']` because the LoRaIoT
spec uses only these two methods. Extending to other methods in the future
requires adding them here.

This approach was chosen over `wrapComponents.OperationSummary` (which caused
"Could not render OperationContainer" errors in Swagger UI 5.x because
component wrapping fires after the render tree is already being constructed)
and over server-side filtering (which would require DRAM assembly of a
tier-specific JSON string).

---

## 6. Template size optimisations applied

The original Swagger UI HTML was built with Arduino `String` concatenation.
After refactoring to PROGMEM template + stack buffer, several further
reductions were made to keep the stack buffer small as the template grows with
new JS:

| Change | Saving |
|---|---|
| Replaced CSS class-based `.banner` style block with inline `style=` on the `<div>` | -172 bytes |
| `<strong>` → `<b>` (5 occurrences) | -30 bytes |
| Removed `SwaggerUIStandalonePreset` (adds a download toolbar not useful on ESP32) | -42 bytes |
| HTTP methods array reduced from 7 methods to `['get','post']` | -40 bytes |
| **Total** | **-284 bytes** |

---

## 7. Description length guidelines

Path descriptions are stored in PROGMEM and do not affect DRAM. However, they
do affect total flash consumption (~33 KB currently) and read time in Swagger UI.
Keep descriptions concise:

- State **what** the endpoint does, not **how** it is implemented internally.
- Do not repeat information already in the `summary`, `tags`, or `x-tier` field.
- Do not append ` LOCAL only.` or ` REMOTE only.` - the `x-tier` field and tag
  description already carry that information.
- Aim for one or two sentences per operation.

---

## 8. Adding a new endpoint

1. Register the route in `RestService::registerRoutes()` (or `Server.cpp` for
   system-level endpoints).
2. Add the path entry to `OPENAPI_JSON` in `openapi_json.h`:
   - Include `"x-tier": "LOCAL"` or `"x-tier": "REMOTE"` if the endpoint is
     tier-specific; omit the field if it is universal.
   - Add `$ref` component schemas to the `components/schemas` section if the
     response shape is new.
3. Add a reference section to `doc/RestApi.md` with a curl example.
4. Validate the JSON after editing:
   ```
   cmd /c "D:\Development\SetupEnvPython3.cmd && python -c \"import sys,json; f=open('src/LoRaIoT/src/openapi_json.h'); c=f.read(); s=c[c.find('R\\\"rawjson(')+10:c.find(')'+'rawjson\\\"')]; json.loads(s); print('OK')\""
   ```
   A simpler approach: extract the JSON block manually and run it through any
   online JSON validator or `python -m json.tool`.
