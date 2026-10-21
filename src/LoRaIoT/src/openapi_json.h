#pragma once
#include <pgmspace.h>

/**
 * openapi_json.h - OpenAPI 3.0.0 specification for the LoRaIoT REST API.
 *
 * Stored in flash (PROGMEM) and served at GET /openapi via send_P().
 * The Swagger UI at GET /openapi/ui fetches this to render the interactive
 * explorer. The browser needs internet access to load Swagger UI from unpkg;
 * the ESP32 itself does not.
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
    { "name": "LoRa-Nodes",  "description": "Remote node registry and telemetry (LOCAL only)" },
    { "name": "LoRa-Control","description": "Ping, echo, beacon and data dispatch (LOCAL only)" },
    { "name": "Sensor",      "description": "SoftAP sensor data ingestion (REMOTE only)" }
  ],
  "paths": {
    "/api/tier": {
      "get": {
        "tags": ["Gateway"],
        "summary": "Active gateway tier",
        "description": "Returns the currently configured gateway tier (LOCAL or REMOTE). The tier is selected once in the WiFiManager captive portal and persisted in NVS. It survives reboots and is cleared by GET /reset.",
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
        "description": "Returns the currently configured LoRa channel. This endpoint is available on both LOCAL and REMOTE instances.",
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
        "description": "Sets the active LoRa channel. The new channel is persisted in NVS and survives reboots. This endpoint is available on both LOCAL and REMOTE instances.",
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
        "description": "Returns the currently configured LoRa node address. This endpoint is available on all nodes (`1` for LOCAL, `2-254` for REMOTE). `0` is invalid and `255` is the broadcast address.",
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
        "description": "Sets the active LoRa node address. The new address (`2-254`, because `1` is reserved for LOCAL) is persisted in NVS and survives reboots. The E220 module is programmed at boot, so the new address takes effect after the next reboot. This endpoint is only available on REMOTE instances; accessing it on a LOCAL instance returns a bad request.",
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
        "description": "Dynamically scales the ESP32's CPU clock frequency (80, 160, or 240). Frequencies below 80 MHz are rejected because the ESP32 WiFi radio PLL requires at least 80 MHz to function. The value is persisted in NVS and automatically applied on boot.",
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
        "description": "Returns the last 128 log lines from the in-RAM ring buffer as a JSON array. Add ?clear=1 to flush the buffer after reading.",
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
        "description": "Sets the active log level. The new level is persisted in NVS and survives reboots. Valid values: TRACE, DEBUG, INFO, WARN, ERROR.",
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
        "description": "Schedules a hardware check of the E220 LoRa module (UART TX/RX, M0, M1, AUX pins, and register read/write round-trip). The check runs on the next pass of applicationLoop() - this endpoint returns immediately. Read the outcome from GET /api/log a few seconds later; look for lines starting with 'E220 diag:'. Uses only the temporary write command (0xC2) so nothing is stored in the module's flash. The same run then performs a software-only nanopb check (encode a PING payload, compare it with the documented bytes 08 F8 AC D1 91 01, decode it again); its log lines start with 'Protobuf diag:'. Removed once Milestone 3 (LoRaTransport) takes over UART2.",
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
    "/reset": {
      "get": {
        "tags": ["Diagnostics"],
        "summary": "Clear WiFi credentials and all NVS settings, then reboot",
        "description": "Erases the stored WiFi SSID/password and every application setting persisted in NVS (gateway tier, log level, LoRa channel, LoRa address, CPU frequency), then reboots into the WiFiManager captive portal. The device will be unreachable until credentials and tier are re-entered via the portal.",
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
        "tags": ["LoRa-Nodes"],
        "summary": "List all known remote nodes",
        "description": "Returns every remote node address that LOCAL has observed via PING or BEACON, together with their cached boot_id, registry_rev and device count. LOCAL only.",
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
        "tags": ["LoRa-Nodes"],
        "summary": "List or lookup devices on a remote node",
        "description": "Without the `device` parameter returns all cached device definitions for the node. With `device` (index or name) returns only that entry. Add `refresh=true` to schedule a fresh Describe crawl before returning (the crawl runs asynchronously; poll again after ~3 s to see results). LOCAL only.",
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
        "tags": ["LoRa-Nodes"],
        "summary": "Get cached or live metric readings for a device",
        "description": "Returns the most recently received measurement values for the specified device. Add `live=true` to send a REQUEST(Read) to the node and wait for a fresh response (blocks until `timeout` ms or 504). LOCAL only.",
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
        "tags": ["LoRa-Nodes"],
        "summary": "LoRa transport statistics",
        "description": "Returns cumulative TX/RX frame counts and error counters since the last reboot or resetStats() call. CRC error and ACK loss counters are wired in Phase 3 (frame protocol layer); they return 0 until then.",
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
        "tags": ["LoRa-Control"],
        "summary": "Echo test to a remote node",
        "description": "Sends a REQUEST(Echo) to the node. REMOTE reverses the text and returns it in the RESPONSE, proving the frame was processed end-to-end. Blocks until the RESPONSE arrives or timeout. LOCAL only.",
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
        "tags": ["LoRa-Control"],
        "summary": "Ping a remote node (unicast)",
        "description": "Sends a PING frame to the specified remote node and waits for an ACK. Returns round-trip time and boot_id. Also available as POST (identical behaviour). LOCAL only.",
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
        "tags": ["LoRa-Control"],
        "summary": "Broadcast PING",
        "description": "Sends a PING to the broadcast address (0xFF). Returns the first ACK received. Useful for discovering which nodes are reachable. LOCAL only.",
        "responses": {
          "200": { "description": "At least one ACK received", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/PingResponse" } } } },
          "503": { "description": "LoRa broker busy", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "504": { "description": "No response",      "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      }
    },
    "/api/lora/beacon": {
      "post": {
        "tags": ["LoRa-Control"],
        "summary": "Trigger an immediate manual BEACON",
        "description": "Queues a BEACON frame for transmission to the broadcast address. BEACON carries boot_id, fw_version, uptime_s, and registry_rev. Returns 202 Accepted immediately; the frame is sent asynchronously by loraTransportTask. LOCAL only.",
        "responses": {
          "202": { "description": "BEACON queued", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/BeaconResponse" } } } }
        }
      }
    },
    "/api/lora/nodes/data": {
      "post": {
        "tags": ["LoRa-Control"],
        "summary": "Send a Data frame to a remote node",
        "description": "Encodes a loraiot_Data payload and queues it for transmission. Use `ack=true` for DATA_ACK (reliable, waits for ACK from REMOTE) or `ack=false` for unreliable DATA. Returns 202 Accepted immediately. LOCAL only.",
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
        "tags": ["Sensor"],
        "summary": "Ingest sensor data from a SoftAP-connected IoT device",
        "description": "REMOTE only. Accepts a JSON payload from a sensor node connected to REMOTE's WiFi SoftAP. Auto-assigns a stable device index and metric_id for each new key. Increments registry_rev when the schema changes. Rejects metrics that would cause the encoded loraiot_Device proto to exceed 186 bytes.",
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
          "addr":    { "type": "integer", "description": "Node address that replied" },
          "boot_id": { "type": "integer", "description": "boot_id of the responding node; changes on reboot" },
          "rtt_ms":  { "type": "integer", "description": "Round-trip time in milliseconds" }
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
