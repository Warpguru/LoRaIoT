#pragma once
#include <pgmspace.h>

/**
 * openapi_json.h - OpenAPI 3.0.0 specification and Swagger UI template.
 *
 * Two PROGMEM symbols are defined here; both are served by Server.cpp:
 *
 *   OPENAPI_JSON        - Full OpenAPI 3.0 spec in JSON, served at GET /openapi
 *                         via send_P(). Any HTTP or tooling consumer (curl,
 *                         Home Assistant, ...) receives the complete spec
 *                         including x-tier extension fields.
 *
 *   OPENAPI_UI_TEMPLATE - HTML/JS template for GET /openapi/ui, expanded
 *                         into a stack buffer at request time (see Server.cpp
 *                         handleGetOpenApiUi). Contains the Swagger UI CDN
 *                         loader and the tierPlugin that hides operations
 *                         whose x-tier does not match the running tier.
 *
 * Covers all endpoints registered in RestService.cpp and Server.cpp.
 * Field names, types, and descriptions match doc/RestApi.md exactly.
 * When they diverge, this file is the source of truth.
 *
 * Pattern: identical to SerialController/openapi_json.h - hand-authored spec
 * embedded in flash, served as static JSON. No code generation involved.
 */
static const char OPENAPI_JSON[] PROGMEM = R"rawjson(
{
  "openapi": "3.0.0",
  "info": {
    "title": "LoRaIoT Gateway API",
    "version": "1.0.0",
    "description": "REST API for the LoRaIoT dual-role ESP32 LoRa gateway. Two nodes (LOCAL and REMOTE) bridge a WiFi SoftAP to a local LAN via a 433 MHz LoRa P2P link. Interactive UI at /openapi/ui."
  },
  "servers": [
    { "url": "/", "description": "This device" }
  ],
  "tags": [
    { "name": "Gateway",     "description": "Gateway role and link status" },
    { "name": "Diagnostics", "description": "Logging, hardware status, and E220 wiring check" },
    { "name": "LoRa-Nodes",  "description": "Remote node registry and telemetry" },
    { "name": "LoRa-Control","description": "Ping, echo, beacon and data dispatch" },
    { "name": "Sensor",      "description": "SoftAP sensor data ingestion" }
  ],
  "paths": {
    "/api/tier": {
      "get": {
        "tags": ["Gateway"],
        "summary": "Active gateway tier",
        "description": "Configured gateway tier (LOCAL or REMOTE). Persisted in NVS; cleared by GET /reset.",
        "responses": {
          "200": {
            "description": "OK",
            "content": {
              "application/json": {
                "schema": { "$ref": "#/components/schemas/TierResponse" }
              }
            }
          }
        }
      }
    },
    "/api/lora/channel": {
      "get": {
        "tags": ["Gateway"],
        "summary": "Get active LoRa channel",
        "description": "Returns the active LoRa channel (0-31). Available on both tiers.",
        "responses": {
          "200": {
            "description": "OK",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "channel": { "type": "integer", "description": "The active LoRa channel (0-31)" }
                  }
                }
              }
            }
          }
        }
      },
      "put": {
        "tags": ["Gateway"],
        "summary": "Set active LoRa channel",
        "description": "Sets the LoRa channel (0-31). Persisted in NVS; takes effect on next reboot. Available on both tiers.",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "required": ["channel"],
                "properties": {
                  "channel": { "type": "integer", "minimum": 0, "maximum": 31, "description": "New LoRa channel" }
                }
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Channel changed",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "channel": { "type": "integer", "description": "The new active LoRa channel (0-31)" }
                  }
                }
              }
            }
          },
          "400": {
            "description": "Body is not valid JSON, field missing, or out of range",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } }
          }
        }
      }
    },
    "/api/lora/address": {
      "get": {
        "tags": ["Gateway"],
        "summary": "Get active LoRa node address",
        "description": "Returns the node address. LOCAL is always 1; REMOTE is 2-254. 0 is invalid, 255 is broadcast.",
        "responses": {
          "200": {
            "description": "OK",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "address": { "type": "integer", "description": "The active LoRa node address (1 = LOCAL, 2-254 = REMOTE)" }
                  }
                }
              }
            }
          }
        }
      },
      "put": {
        "tags": ["Gateway"],
        "summary": "Set active LoRa node address",
        "description": "Sets the REMOTE node address (2-254). Persisted in NVS; takes effect on next reboot. REMOTE only.",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "required": ["address"],
                "properties": {
                  "address": { "type": "integer", "minimum": 2, "maximum": 254, "description": "New LoRa address (2-254)" }
                }
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Address changed",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "address": { "type": "integer", "description": "The new active LoRa address (2-254)" }
                  }
                }
              }
            }
          },
          "400": {
            "description": "Body is not valid JSON, field missing, out of range, or not a REMOTE instance",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } }
          }
        }
      }
    },
    "/api/system/cpu": {
      "get": {
        "tags": ["System"],
        "summary": "Get active CPU frequency",
        "description": "Returns the currently active CPU frequency of the ESP32 in MHz.",
        "responses": {
          "200": {
            "description": "OK",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "frequency": { "type": "integer", "description": "The active CPU frequency in MHz (80, 160, or 240)" }
                  }
                }
              }
            }
          }
        }
      },
      "put": {
        "tags": ["System"],
        "summary": "Set active CPU frequency",
        "description": "Sets CPU frequency (80, 160, or 240 MHz). Below 80 MHz is rejected (WiFi PLL minimum). Persisted in NVS.",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "required": ["frequency"],
                "properties": {
                  "frequency": { "type": "integer", "enum": [80, 160, 240], "description": "New CPU frequency in MHz" }
                }
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "CPU frequency changed",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "frequency": { "type": "integer", "description": "The new active CPU frequency in MHz (80, 160, or 240)" }
                  }
                }
              }
            }
          },
          "400": {
            "description": "Body is not valid JSON, field missing, or not a supported value (80, 160, 240)",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } }
          }
        }
      }
    },
    "/api/log": {
      "get": {
        "tags": ["Diagnostics"],
        "summary": "Recent log entries",
        "description": "Last 128 log lines from the ring buffer. Add ?clear=1 to flush after reading.",
        "parameters": [
          {
            "name": "clear",
            "in": "query",
            "required": false,
            "schema": { "type": "integer", "enum": [1] },
            "description": "Pass 1 to clear the log buffer after reading"
          }
        ],
        "responses": {
          "200": {
            "description": "OK",
            "content": {
              "application/json": {
                "schema": { "$ref": "#/components/schemas/LogResponse" }
              }
            }
          }
        }
      }
    },
    "/api/log/level": {
      "put": {
        "tags": ["Diagnostics"],
        "summary": "Change log level at runtime",
        "description": "Sets log level (TRACE/DEBUG/INFO/WARN/ERROR). Persisted in NVS.",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "required": ["level"],
                "properties": {
                  "level": {
                    "type": "string",
                    "enum": ["TRACE", "DEBUG", "INFO", "WARN", "ERROR"],
                    "description": "New log level"
                  }
                }
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Level changed",
            "content": {
              "application/json": {
                "schema": { "$ref": "#/components/schemas/LogLevelResponse" }
              }
            }
          },
          "400": {
            "description": "Body is not valid JSON, field missing, or unknown level name",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } }
          }
        }
      }
    },
    "/api/diag/e220": {
      "get": {
        "tags": ["Diagnostics"],
        "summary": "Request E220 wiring and register check",
        "description": "Schedules an E220 wiring check (UART, M0/M1/AUX pins, register round-trip) and a nanopb encode/decode self-test. Runs on next applicationLoop() pass; returns immediately. Check results appear in GET /api/log (prefix 'E220 diag:' and 'Protobuf diag:').",
        "responses": {
          "202": {
            "description": "Check scheduled - read result from GET /api/log",
            "content": {
              "application/json": {
                "schema": { "$ref": "#/components/schemas/DiagRequestedResponse" }
              }
            }
          }
        }
      }
    },
    "/status": {
      "get": {
        "tags": ["Diagnostics"],
        "summary": "ESP32 hardware and WiFi diagnostics",
        "description": "Returns heap size, flash size, chip model, WiFi RSSI, IP address, and uptime.",
        "responses": {
          "200": {
            "description": "OK",
            "content": { "application/json": { "schema": { "type": "object" } } }
          }
        }
      }
    },
    "/reboot": {
      "get": {
        "tags": ["Diagnostics"],
        "summary": "Reboot the ESP32",
        "description": "Reboots the ESP32 immediately, preserving all NVS settings (WiFi credentials, tier, log level, LoRa config). The node is back online within a few seconds.",
        "responses": {
          "200": {
            "description": "ESP32 is rebooting",
            "content": { "text/plain": { "schema": { "type": "string" } } }
          }
        }
      }
    },
    "/reset": {
      "get": {
        "tags": ["Diagnostics"],
        "summary": "Clear WiFi credentials and all NVS settings, then reboot",
        "description": "Erases WiFi credentials and all NVS settings (tier, log level, channel, address, CPU freq), then reboots into the captive portal.",
        "responses": {
          "200": {
            "description": "Settings cleared; ESP32 is rebooting",
            "content": { "text/plain": { "schema": { "type": "string" } } }
          }
        }
      }
    },
    "/api/lora/nodes": {
      "get": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Nodes"],
        "summary": "List all known remote nodes",
        "description": "Lists all known remote nodes with their cached boot_id, registry_rev and device count.",
        "responses": {
          "200": {
            "description": "OK",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/NodeListResponse" } } }
          }
        }
      }
    },
    "/api/lora/nodes/devices": {
      "get": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Nodes"],
        "summary": "List or lookup devices on a remote node",
        "description": "Returns cached device definitions for the node. Filter by `device` (index or name). Add `refresh=true` to trigger a Describe crawl (async; poll again after ~3 s).",
        "parameters": [
          { "name": "addr",    "in": "query", "required": true,  "schema": { "type": "integer", "minimum": 2, "maximum": 254 }, "description": "Remote node LoRa address" },
          { "name": "device",  "in": "query", "required": false, "schema": { "type": "string" }, "description": "Device index (integer) or device name to filter by" },
          { "name": "refresh", "in": "query", "required": false, "schema": { "type": "boolean" }, "description": "Set true to trigger a fresh Describe crawl" }
        ],
        "responses": {
          "200": {
            "description": "OK",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/DeviceListResponse" } } }
          },
          "400": { "description": "addr missing or out of range", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "404": { "description": "Node not found in registry",  "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      }
    },
    "/api/lora/nodes/metrics": {
      "get": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Nodes"],
        "summary": "Get cached or live metric readings for a device",
        "description": "Returns cached measurements for the device. Add `live=true` to send a REQUEST(Read) and wait for a fresh response (blocks up to `timeout` ms, 504 on no reply).",
        "parameters": [
          { "name": "addr",    "in": "query", "required": true,  "schema": { "type": "integer", "minimum": 2, "maximum": 254 }, "description": "Remote node LoRa address" },
          { "name": "device",  "in": "query", "required": true,  "schema": { "type": "integer" }, "description": "Device index" },
          { "name": "live",    "in": "query", "required": false, "schema": { "type": "boolean" }, "description": "Set true to force a live REQUEST(Read) instead of returning cached data" },
          { "name": "timeout", "in": "query", "required": false, "schema": { "type": "integer", "minimum": 200, "maximum": 10000, "default": 3000 }, "description": "Timeout in ms for live read (only used when live=true)" }
        ],
        "responses": {
          "200": {
            "description": "OK - cached or live measurements",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/MetricsResponse" } } }
          },
          "400": { "description": "Missing or invalid parameters", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "404": { "description": "Device not found",             "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "503": { "description": "LoRa broker busy",             "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "504": { "description": "No response from remote node within timeout", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      }
    },
    "/api/lora/stats": {
      "get": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Nodes"],
        "summary": "LoRa transport statistics",
        "description": "Cumulative TX/RX frame counts and error counters since last reboot. CRC and ACK loss counters are 0 until the frame codec is wired.",
        "responses": {
          "200": {
            "description": "OK",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/LoRaStatsResponse" } } }
          }
        }
      }
    },
    "/api/lora/nodes/echo": {
      "post": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Control"],
        "summary": "Echo test to a remote node",
        "description": "Sends REQUEST(Echo) to the node; REMOTE returns the text reversed. Blocks until RESPONSE arrives or timeout.",
        "parameters": [
          { "name": "addr", "in": "query", "required": true, "schema": { "type": "integer", "minimum": 2, "maximum": 254 }, "description": "Remote node LoRa address" }
        ],
        "requestBody": {
          "required": true,
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/EchoRequest" } } }
        },
        "responses": {
          "200": { "description": "Echo response received", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/EchoResponse" } } } },
          "400": { "description": "Missing addr, invalid body, or text exceeds 175 characters", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "503": { "description": "LoRa broker busy",            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "504": { "description": "No response from remote node", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      }
    },
    "/api/lora/nodes/ping": {
      "get": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Control"],
        "summary": "Ping a remote node (unicast)",
        "description": "Sends a PING to the specified node and waits for an ACK. Returns RTT and, when known from a BEACON, boot_id. Also available as POST.",
        "parameters": [
          { "name": "addr", "in": "query", "required": true, "schema": { "type": "integer", "minimum": 2, "maximum": 254 }, "description": "Remote node LoRa address" }
        ],
        "responses": {
          "200": { "description": "Ping ACK received", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/PingResponse" } } } },
          "400": { "description": "Missing or invalid addr", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "503": { "description": "LoRa broker busy",       "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "504": { "description": "No ping ACK received",   "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      },
      "post": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Control"],
        "summary": "Ping a remote node (unicast, POST form)",
        "description": "Identical to GET /api/lora/nodes/ping. POST form provided for clients that prefer POST for actions.",
        "parameters": [
          { "name": "addr", "in": "query", "required": true, "schema": { "type": "integer", "minimum": 2, "maximum": 254 }, "description": "Remote node LoRa address" }
        ],
        "responses": {
          "200": { "description": "Ping ACK received", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/PingResponse" } } } },
          "400": { "description": "Missing or invalid addr", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "503": { "description": "LoRa broker busy",       "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "504": { "description": "No ping ACK received",   "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      }
    },
    "/api/lora/ping": {
      "get": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Control"],
        "summary": "Broadcast PING",
        "description": "Sends a broadcast PING (0xFF) and returns the first ACK received.",
        "responses": {
          "200": { "description": "At least one ACK received", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/PingResponse" } } } },
          "503": { "description": "LoRa broker busy", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "504": { "description": "No response",      "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      }
    },
    "/api/lora/beacon": {
      "post": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Control"],
        "summary": "Trigger an immediate manual BEACON",
        "description": "Queues a BEACON (boot_id, fw_version, uptime_s, registry_rev) for broadcast. Returns 202 immediately; sent asynchronously.",
        "responses": {
          "202": { "description": "BEACON queued", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/BeaconResponse" } } } }
        }
      }
    },
    "/api/lora/nodes/data": {
      "post": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Control"],
        "summary": "Send a Data frame to a remote node",
        "description": "Queues a Data frame to the node. `ack=true` for reliable DATA_ACK, `ack=false` for fire-and-forget DATA. Returns 202 immediately.",
        "parameters": [
          { "name": "addr", "in": "query", "required": true,  "schema": { "type": "integer", "minimum": 2, "maximum": 254 }, "description": "Remote node LoRa address" },
          { "name": "ack",  "in": "query", "required": false, "schema": { "type": "boolean", "default": false }, "description": "true = reliable DATA_ACK, false = unreliable DATA" }
        ],
        "requestBody": {
          "required": true,
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/DataPayload" } } }
        },
        "responses": {
          "202": { "description": "Frame queued", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/DataDispatchResponse" } } } },
          "400": { "description": "Missing addr or invalid body", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      }
    },
    "/api/sensor/data": {
      "post": {
        "x-tier": "REMOTE",
        "tags": ["Sensor"],
        "summary": "Ingest sensor data from a SoftAP-connected IoT device",
        "description": "Accepts sensor data from a SoftAP-connected device. Auto-assigns device index and metric_id; increments registry_rev on schema changes. Rejects metrics that would cause the Device proto to exceed 186 bytes.",
        "requestBody": {
          "required": true,
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataPayload" } } }
        },
        "responses": {
          "200": { "description": "All metrics accepted", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataResponse" } } } },
          "400": { "description": "Some metrics rejected (payload limit, table full, or not a REMOTE instance)", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataResponse" } } } }
        }
      }
    }
  },
  "components": {
    "schemas": {
      "TierResponse": {
        "type": "object",
        "properties": {
          "tier": {
            "type": "string",
            "enum": ["LOCAL", "REMOTE", "UNSET"],
            "description": "LOCAL: connected to local LAN / Home Assistant, receives data from REMOTE. REMOTE: hosts SoftAP for IoT sensors, forwards data over LoRa. UNSET: portal not yet completed."
          },
          "configured": {
            "type": "boolean",
            "description": "false when tier is UNSET (portal has not been completed)"
          }
        }
      },
      "LogResponse": {
        "type": "object",
        "properties": {
          "log": {
            "type": "array",
            "items": { "type": "string" },
            "description": "Log lines in format [LEVEL][file:line] message"
          },
          "level": {
            "type": "string",
            "enum": ["TRACE", "DEBUG", "INFO", "WARN", "ERROR"],
            "description": "Currently active log level"
          }
        }
      },
      "LogLevelResponse": {
        "type": "object",
        "properties": {
          "level": {
            "type": "string",
            "enum": ["TRACE", "DEBUG", "INFO", "WARN", "ERROR"],
            "description": "The new active log level"
          }
        }
      },
      "DiagRequestedResponse": {
        "type": "object",
        "properties": {
          "status":  { "type": "string", "example": "requested" },
          "result":  { "type": "string", "example": "see GET /api/log" }
        }
      },
      "ErrorResponse": {
        "type": "object",
        "properties": {
          "error": { "type": "string", "description": "Human-readable error description" }
        }
      },
      "NodeEntry": {
        "type": "object",
        "properties": {
          "addr":          { "type": "integer", "description": "LoRa node address (2-254)" },
          "boot_id":       { "type": "integer", "description": "Random number chosen by the node at boot; changes on reboot" },
          "registry_rev":  { "type": "integer", "description": "Last successfully crawled registry revision" },
          "last_seen_ms":  { "type": "integer", "description": "millis() timestamp of the last received frame from this node" },
          "online":        { "type": "boolean", "description": "false after 3 unanswered keep-alive PINGs; the entry and its cache are kept and any received frame sets it back to true" },
          "devices":       { "type": "integer", "description": "Number of cached device definitions" },
          "crawl_pending": { "type": "boolean", "description": "true if a Describe crawl is scheduled or in progress" }
        }
      },
      "NodeListResponse": {
        "type": "object",
        "properties": {
          "nodes": { "type": "array", "items": { "$ref": "#/components/schemas/NodeEntry" } }
        }
      },
      "MetricDefinition": {
        "type": "object",
        "properties": {
          "id":    { "type": "integer", "description": "Metric ID, unique within the device (1-based)" },
          "name":  { "type": "string",  "description": "Human-readable metric name, e.g. Voltage (max 12 chars)" },
          "unit":  { "type": "string",  "description": "Unit string, e.g. V (max 5 chars, may be empty)" },
          "scale": { "type": "integer", "description": "Divisor: real_value = raw_value / scale. 0 treated as 1" }
        }
      },
      "DeviceDefinition": {
        "type": "object",
        "properties": {
          "device":  { "type": "integer", "description": "Device index (0 = the REMOTE node itself, 1-4 = SoftAP sensors)" },
          "name":    { "type": "string",  "description": "Device name, e.g. Sensor A (max 12 chars)" },
          "metrics": { "type": "array",   "items": { "$ref": "#/components/schemas/MetricDefinition" } }
        }
      },
      "DeviceListResponse": {
        "type": "object",
        "properties": {
          "addr":          { "type": "integer" },
          "registry_rev":  { "type": "integer" },
          "crawl_pending": { "type": "boolean" },
          "devices":       { "type": "array", "items": { "$ref": "#/components/schemas/DeviceDefinition" } }
        }
      },
      "MeasurementValue": {
        "type": "object",
        "properties": {
          "metric_id": { "type": "integer", "description": "Matches MetricDefinition.id" },
          "value":     { "type": "integer", "description": "Scaled integer value. Divide by scale for real value" },
          "name":      { "type": "string",  "description": "Metric name (from definition, if available)" },
          "unit":      { "type": "string",  "description": "Unit string (from definition, if available)" },
          "scale":     { "type": "integer", "description": "Scale divisor (from definition, if available)" }
        }
      },
      "MetricsResponse": {
        "type": "object",
        "properties": {
          "addr":     { "type": "integer" },
          "device":   { "type": "integer" },
          "name":     { "type": "string" },
          "has_data": { "type": "boolean", "description": "false if no measurement has ever been received for this device" },
          "age_ms":   { "type": "integer", "description": "Milliseconds since the data was last updated (0 = just now)" },
          "alert":    { "type": "boolean", "description": "true if the last frame carried an alert flag" },
          "live":     { "type": "boolean", "description": "true if this response came from a live REQUEST(Read)" },
          "values":   { "type": "array",   "items": { "$ref": "#/components/schemas/MeasurementValue" } }
        }
      },
      "LoRaStatsResponse": {
        "type": "object",
        "properties": {
          "tx_frames":  { "type": "integer", "description": "Frames successfully written to UART2 since boot" },
          "rx_frames":  { "type": "integer", "description": "Complete frames delivered to the receive callback since boot" },
          "crc_errors": { "type": "integer", "description": "Frames discarded due to CRC mismatch (wired in Phase 3)" },
          "ack_loss":   { "type": "integer", "description": "ACK timeouts / unacknowledged DATA_ACK frames (wired in Phase 3)" }
        }
      },
      "EchoRequest": {
        "type": "object",
        "required": ["text"],
        "properties": {
          "text": { "type": "string", "maxLength": 175, "description": "Text to send; REMOTE reverses it in the RESPONSE to prove end-to-end processing" }
        }
      },
      "EchoResponse": {
        "type": "object",
        "properties": {
          "addr":   { "type": "integer" },
          "seq":    { "type": "integer", "description": "REQUEST SEQ number assigned to this exchange" },
          "status": { "type": "string",  "enum": ["ok", "error"] },
          "echo":   { "type": "string",  "description": "The reversed text returned by the remote node" }
        }
      },
      "PingResponse": {
        "type": "object",
        "properties": {
          "addr":    { "type": "integer", "description": "Node address that replied (for a broadcast ping, the first node that answered)" },
          "rtt_ms":  { "type": "integer", "description": "Round-trip time in milliseconds" },
          "boot_id": { "type": "integer", "description": "Optional. boot_id from the last BEACON of that node; omitted if no BEACON was received yet (the ACK carries no boot_id)" }
        }
      },
      "BeaconResponse": {
        "type": "object",
        "properties": {
          "status": { "type": "string", "example": "queued", "description": "Always queued - frame is sent asynchronously" }
        }
      },
      "DataPayload": {
        "type": "object",
        "required": ["device", "values"],
        "properties": {
          "device": { "type": "integer", "description": "Device index (0 = node itself, 1-4 = SoftAP sensor)" },
          "values": {
            "type": "array",
            "maxItems": 16,
            "items": {
              "type": "object",
              "required": ["metric_id", "value"],
              "properties": {
                "metric_id": { "type": "integer" },
                "value":     { "type": "integer", "description": "Scaled integer (divide by MetricDefinition.scale for real value)" }
              }
            }
          }
        }
      },
      "DataDispatchResponse": {
        "type": "object",
        "properties": {
          "addr":  { "type": "integer" },
          "ack":   { "type": "boolean", "description": "true = DATA_ACK (reliable), false = DATA (unreliable)" },
          "bytes": { "type": "integer", "description": "Encoded nanopb payload length in bytes" }
        }
      },
      "SensorDataPayload": {
        "type": "object",
        "required": ["device", "metrics"],
        "properties": {
          "device": { "type": "string", "maxLength": 12, "description": "Device name, e.g. SensorA" },
          "metrics": {
            "type": "object",
            "description": "Map of metric name to measurement. Each value is an object with value (required), unit (optional), scale (optional).",
            "additionalProperties": {
              "type": "object",
              "required": ["value"],
              "properties": {
                "value": { "type": "integer", "description": "Scaled integer measurement" },
                "unit":  { "type": "string",  "maxLength": 5 },
                "scale": { "type": "integer", "description": "Divisor for real value reconstruction. Default 1." }
              }
            }
          }
        },
        "example": { "device": "SensorA", "metrics": { "Voltage": { "value": 335, "unit": "V", "scale": 100 }, "Temp": { "value": 215, "unit": "C", "scale": 10 } } }
      },
      "SensorDataResponse": {
        "type": "object",
        "properties": {
          "accepted":      { "type": "array",   "items": { "type": "object", "properties": { "metric": { "type": "string" }, "device_idx": { "type": "integer" }, "metric_id": { "type": "integer" } } } },
          "rejected":      { "type": "array",   "items": { "type": "string" }, "description": "Metric names that were rejected (payload limit, table full)" },
          "registry_rev":  { "type": "integer", "description": "Current registry revision after this request" }
        }
      }
    }
  }
}
)rawjson";

/**
 * OPENAPI_UI_TEMPLATE - HTML template for GET /openapi/ui.
 * Stored in flash (PROGMEM); never occupies DRAM at idle.
 *
 * Placeholders (positional, consumed by snprintf in handleGetOpenApiUi):
 *   %s  SSID of the connected network             (banner)
 *   %s  Gateway tier name for display             (banner)
 *   %u  LoRa key, decimal (0 = disabled)          (banner)
 *   %s  LoRa key state "on" / "off"               (banner)
 *   %s  Build timestamp __DATE__ " " __TIME__     (banner)
 *   %s  Gateway tier name for JS TIER variable    (x-tier filter)
 *
 * IP address is not injected server-side. A one-line JS snippet reads
 * location.hostname so the displayed address is always the interface the
 * browser actually used - correct for both STA mode (LAN DHCP IP) and
 * future SoftAP mode (192.168.4.1).
 *
 * The tierPlugin wrapComponents hook inspects the x-tier OpenAPI extension
 * on each operation and returns null (hides the row) when the operation's
 * required tier does not match the running tier. Operations with no x-tier
 * field are always shown on both tiers.
 */
static const char OPENAPI_UI_TEMPLATE[] PROGMEM =
  "<!DOCTYPE html>"
  "<html lang='en'><head>"
  "<meta charset='utf-8'/>"
  "<title>LoRaIoT API</title>"
  "<link rel='stylesheet' href='https://unpkg.com/swagger-ui-dist@5.33.0/swagger-ui.css'/>"
  "</head><body>"
  "<div style='padding:6px 14px;font:13px sans-serif;border-bottom:1px solid #e5e7eb'>"
  "IP: <b><span id='ip'></span></b>"
  " | SSID: <b>%s</b>"
  " | Tier: <b>%s</b>"
  " | Key: <b>%u (%s)</b>"
  " | Built: <b>%s</b>"
  "</div>"
  "<div id='swagger-ui'></div>"
  "<script src='https://unpkg.com/swagger-ui-dist@5.33.0/swagger-ui-bundle.js'></script>"
  "<script>"
  "document.getElementById('ip').textContent=location.hostname;"
  "var TIER='%s';"
  "function tierPlugin(){"
    "return{"
      "statePlugins:{"
        "spec:{"
          "wrapActions:{"
            "updateJsonSpec:function(oriAction){"
              "return function(spec){"
                "Object.keys(spec.paths||{}).forEach(function(path){"
                  "var item=spec.paths[path];"
                  "['get','post'].forEach(function(m){"
                    "if(item[m]&&item[m]['x-tier']&&item[m]['x-tier']!==TIER)"
                      "delete item[m];"
                  "});"
                  "if(Object.keys(item).length===0)delete spec.paths[path];"
                "});"
                "return oriAction(spec);"
              "};"
            "}"
          "}"
        "}"
      "}"
    "};"
  "}"
  "window.onload=function(){"
    "window.ui=SwaggerUIBundle({"
      "url:'/openapi',"
      "dom_id:'#swagger-ui',"
      "presets:[SwaggerUIBundle.presets.apis],"
      "plugins:[tierPlugin],"
      "layout:'BaseLayout'"
    "});"
  "};"
  "</script>"
  "</body></html>";
