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
    "description": "REST API for the LoRaIoT dual-role ESP32 LoRa gateway. Nodes bridge a WiFi SoftAP to a local WLAN via a 433 MHz LoRa P2P link. Interactive UI at /openapi/ui."
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
    "/api/wifi/mode": {
      "get": {
        "tags": ["Gateway"],
        "summary": "Active WiFi operational mode and status",
        "description": "Returns the active WiFi mode (STA, AP, AP_STA), IP addresses, SoftAP settings, and connected client count. STA: reachable at staIp. AP: reachable at apIp (default 192.168.4.1). AP_STA: reachable at apIp and, when connected, staIp.",
        "responses": {
          "200": {
            "description": "OK",
            "content": {
              "application/json": {
                "schema": { "$ref": "#/components/schemas/WifiModeResponse" }
              }
            }
          }
        }
      },
      "put": {
        "tags": ["Gateway"],
        "summary": "Set WiFi mode and SoftAP configuration",
        "description": "Sets the operational mode (STA, AP, or AP_STA) and SoftAP parameters. Omitted optional fields keep their stored values. Persisted in NVS; node automatically reboots after 200 ms. The old address becomes invalid when the mode changes (STA to AP: join the SoftAP SSID, use apIp). Provision tier and LoRa settings in STA mode first (no captive portal in AP mode).",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": { "$ref": "#/components/schemas/WifiModePutRequest" }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Settings accepted, rebooting",
            "content": {
              "application/json": {
                "schema": { "$ref": "#/components/schemas/WifiModeResponse" }
              }
            }
          },
          "400": {
            "description": "Invalid parameter (mode, SSID over 32 chars, IP format, channel outside 1-13, or password not empty / 8-64 chars)",
            "content": {
              "application/json": {
                "schema": { "$ref": "#/components/schemas/ErrorResponse" }
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
    "/api/lora/airrate": {
      "get": {
        "tags": ["Gateway"],
        "summary": "Get active LoRa air data rate",
        "description": "Returns the active LoRa air data rate. Available on both tiers.",
        "responses": {
          "200": {
            "description": "OK",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "airRate": { "type": "string", "enum": ["2.4k", "4.8k", "9.6k", "19.2k", "38.4k", "62.5k"], "description": "The active air data rate in kbps" }
                  }
                }
              }
            }
          }
        }
      },
      "put": {
        "tags": ["Gateway"],
        "summary": "Set active LoRa air data rate",
        "description": "Sets the air data rate, persists it in NVS and immediately rewrites the E220 registers (about 100 ms in config mode; frames during that time are lost). Both nodes must use the same rate, so change REMOTE first, then LOCAL. Lower rates give more range but longer air time. Available on both tiers.",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "required": ["airRate"],
                "properties": {
                  "airRate": { "type": "string", "enum": ["2.4k", "4.8k", "9.6k", "19.2k", "38.4k", "62.5k"], "description": "New air data rate in kbps" }
                }
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Air rate changed; the E220 reconfiguration was requested",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "airRate": { "type": "string", "description": "The new active air data rate" }
                  }
                }
              }
            }
          },
          "400": {
            "description": "Body is not valid JSON, field missing, or not one of the supported rates",
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
        "description": "Erases WiFi credentials and all NVS settings (tier, log level, channel, air rate, address, CPU freq), then reboots into the captive portal.",
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
    "/api/lora/nodes/sensor": {
      "get": {
        "tags": ["LoRa-Nodes"],
        "summary": "Unified device schema and metric sensor view",
        "description": "On LOCAL: returns cached device definitions and sensor metric values for one or all nodes. Use `addr` to target one node, `device` to filter to one device, `live=true` (requires `addr`+`device`) to send a REQUEST(Read) before responding, `refresh=true` (requires `addr`) to schedule a Describe crawl and return 202. On REMOTE: returns own sensor device definitions and values (`addr`, `live` and `refresh` return 400). Every node carries `online` and `peer_last_seen_ms_ago`; every device carries `age_ms`.",
        "parameters": [
          { "name": "addr",    "in": "query", "required": false, "schema": { "type": "integer", "minimum": 1, "maximum": 254 }, "description": "Node LoRa address. Omit for all known nodes (LOCAL only)" },
          { "name": "device",  "in": "query", "required": false, "schema": { "type": "integer" }, "description": "Device index filter" },
          { "name": "live",    "in": "query", "required": false, "schema": { "type": "boolean" }, "description": "Set true to send REQUEST(Read) before responding. Requires both addr and device (LOCAL only)" },
          { "name": "timeout", "in": "query", "required": false, "schema": { "type": "integer", "minimum": 200, "maximum": 4000, "default": 3000 }, "description": "Live read timeout in ms (max 4000 - constrained by async_tcp WDT). Only used with live=true" },
          { "name": "refresh", "in": "query", "required": false, "schema": { "type": "boolean" }, "description": "Set true to schedule a Describe crawl of the node and return 202 at once. Requires addr (LOCAL only)" }
        ],
        "responses": {
          "200": {
            "description": "OK",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/NodeSnapshotResponse" } } }
          },
          "202": { "description": "refresh=true: Describe crawl scheduled", "content": { "application/json": { "schema": { "type": "object", "properties": { "status": { "type": "string" }, "addr": { "type": "integer" } } } } } },
          "400": { "description": "Invalid parameter combination", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "404": { "description": "addr specified but node not found", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "503": { "description": "LoRa broker busy (live=true only)", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "504": { "description": "No response from remote node within timeout (live=true only)", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      }
    },
    "/api/lora/nodes/control": {
      "get": {
        "tags": ["LoRa-Nodes"],
        "summary": "Unified device schema and metric control view",
        "description": "On LOCAL: returns own control device definitions and values from ControlRegistry (`live` and `refresh` return 400). Use `addr` to target one node, `device` to filter to one device. On REMOTE: returns cached control device definitions and values from LOCAL (`addr` returns 400); `live=true` (requires `device`) sends a REQUEST(Read) to LOCAL before responding, `refresh=true` schedules a Describe crawl of LOCAL and returns 202. Every node carries `online` and `peer_last_seen_ms_ago`; every device carries `age_ms`.",
        "parameters": [
          { "name": "addr",   "in": "query", "required": false, "schema": { "type": "integer", "minimum": 1, "maximum": 254 }, "description": "Node LoRa address. Omit for all nodes (LOCAL only)" },
          { "name": "device", "in": "query", "required": false, "schema": { "type": "integer" }, "description": "Device index filter" },
          { "name": "live",    "in": "query", "required": false, "schema": { "type": "boolean" }, "description": "Set true to send REQUEST(Read) to LOCAL before responding. Requires device (REMOTE only)" },
          { "name": "timeout", "in": "query", "required": false, "schema": { "type": "integer", "minimum": 200, "maximum": 4000, "default": 3000 }, "description": "Live read timeout in ms. Only used with live=true" },
          { "name": "refresh", "in": "query", "required": false, "schema": { "type": "boolean" }, "description": "Set true to schedule a Describe crawl of LOCAL and return 202 at once (REMOTE only)" }
        ],
        "responses": {
          "200": {
            "description": "OK",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/NodeSnapshotResponse" } } }
          },
          "202": { "description": "refresh=true: Describe crawl scheduled", "content": { "application/json": { "schema": { "type": "object", "properties": { "status": { "type": "string" } } } } } },
          "400": { "description": "Invalid parameter combination", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "404": { "description": "addr specified but node not found, or live=true and LOCAL returned an error", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "503": { "description": "LoRa broker busy (live=true only)", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "504": { "description": "No response from LOCAL within timeout (live=true only)", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
        }
      },
      "post": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Control"],
        "summary": "Register control devices, store setpoint values, and push to remote",
        "description": "Registers devices and metrics in ControlRegistry for the specified node `addr`, stores the setpoint values, and pushes the updated device data to the remote node via DATA_ACK. Returns accepted/rejected metrics, registry revision, and delivery status.",
        "parameters": [
          { "name": "addr", "in": "query", "required": true, "schema": { "type": "integer", "minimum": 2, "maximum": 254 }, "description": "Target remote node LoRa address" }
        ],
        "requestBody": {
          "required": true,
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataPayload" } } }
        },
        "responses": {
          "200": { "description": "All metrics accepted and remote acknowledged with STATUS_OK. Body includes delivery=ok.", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataResponse" } } } },
          "202": { "description": "Metrics accepted but delivery deferred - schema changed (PING sent, REMOTE may not have crawled yet) or REMOTE answered BAD_REQUEST (crawl scheduled). Body includes delivery=after_describe.", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataResponse" } } } },
          "400": { "description": "Invalid JSON, missing fields, or all metrics rejected. No delivery attempted.", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataResponse" } } } },
          "404": { "description": "Target node addr not found", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } },
          "503": { "description": "LoRa broker busy", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataResponse" } } } },
          "504": { "description": "No ACK from remote node after all retries", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/SensorDataResponse" } } } }
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
          { "name": "addr", "in": "query", "required": true, "schema": { "type": "integer", "minimum": 1, "maximum": 254 }, "description": "Node LoRa address" }
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
    "/api/lora/echo/result": {
      "get": {
        "x-tier": "LOCAL",
        "tags": ["LoRa-Control"],
        "summary": "Return the result of the last echo test",
        "description": "Returns the result of the last POST /api/lora/nodes/echo without blocking. The result is stored in RAM and survives until the next echo call replaces it or the node reboots.",
        "responses": {
          "200": { "description": "Result available", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/EchoResultResponse" } } } },
          "404": { "description": "No echo has been performed since boot", "content": { "application/json": { "schema": { "$ref": "#/components/schemas/ErrorResponse" } } } }
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
    "/api/lora/sensor/data": {
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
            "enum": ["LOCAL", "REMOTE (Node 1)", "REMOTE (Node 2)", "REMOTE (Node 3)", "UNSET"],
            "description": "LOCAL: connected to local WLAN, receives data from REMOTE nodes. REMOTE (Node 1..3): forwards data over LoRa; WiFi mode is set via /api/wifi/mode."
          },
          "configured": {
            "type": "boolean",
            "description": "false when tier is UNSET"
          }
        }
      },
      "WifiModeResponse": {
        "type": "object",
        "properties": {
          "status": { "type": "string", "description": "Status text, e.g. 'rebooting' on PUT" },
          "mode": { "type": "string", "enum": ["STA", "AP", "AP_STA"], "description": "Active WiFi mode" },
          "apSsid": { "type": "string", "description": "SoftAP SSID" },
          "apIp": { "type": "string", "description": "SoftAP IP address" },
          "apChannel": { "type": "integer", "description": "SoftAP 2.4 GHz channel" },
          "apClients": { "type": "integer", "description": "Number of connected SoftAP stations (0 in STA mode)" },
          "staIp": { "type": "string", "nullable": true, "description": "Station IP address when connected to external router; null in AP mode or while not connected" }
        }
      },
      "WifiModePutRequest": {
        "type": "object",
        "required": ["mode"],
        "properties": {
          "mode": { "type": "string", "enum": ["STA", "AP", "AP_STA"], "description": "Target operational mode (mandatory)" },
          "ssid": { "type": "string", "maxLength": 32, "description": "Custom SoftAP SSID" },
          "password": { "type": "string", "minLength": 8, "maxLength": 64, "description": "SoftAP WPA2 password (min 8 chars, or empty for open)" },
          "ip": { "type": "string", "description": "Custom SoftAP host IP (e.g. '192.168.4.1')" },
          "channel": { "type": "integer", "minimum": 1, "maximum": 13, "description": "SoftAP 2.4 GHz channel (1-13)" }
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
      "NodeSnapshotResponse": {
        "type": "object",
        "properties": {
          "nodes": { "type": "array", "items": { "$ref": "#/components/schemas/SnapshotNode" } }
        }
      },
      "SnapshotNode": {
        "type": "object",
        "properties": {
          "addr":         { "type": "integer" },
          "boot_id":      { "type": "integer" },
          "registry_rev": { "type": "integer" },
          "online":       { "type": "boolean", "description": "false when the consumer-side liveness tracker presumes the peer dead (3 unanswered keep-alive PINGs); false also when the peer is not yet known" },
          "peer_last_seen_ms_ago": { "type": "integer", "description": "Milliseconds since the last frame from the peer, read from the liveness tracker; 0 when the peer is not yet known. Use together with online and the per-device age_ms for a fail-safe" },
          "devices":      { "type": "array", "items": { "$ref": "#/components/schemas/SnapshotDevice" } }
        }
      },
      "SnapshotDevice": {
        "type": "object",
        "properties": {
          "device":   { "type": "integer" },
          "name":     { "type": "string" },
          "has_data": { "type": "boolean" },
          "age_ms":   { "type": "integer" },
          "alert":    { "type": "boolean" },
          "metrics":  { "type": "array", "items": { "$ref": "#/components/schemas/SnapshotMetric" } }
        }
      },
      "SnapshotMetric": {
        "type": "object",
        "properties": {
          "id":    { "type": "integer" },
          "name":  { "type": "string" },
          "unit":  { "type": "string" },
          "scale": { "type": "integer" },
          "value": { "oneOf": [{ "type": "integer" }, { "type": "string" }], "nullable": true, "description": "Numeric measurement, string text (max 12 bytes UTF-8), or null when no value stored" }
        }
      },
      "LoRaStatsResponse": {
        "type": "object",
        "properties": {
          "tx_frames":  { "type": "integer", "description": "Frames successfully written to UART2 since boot" },
          "rx_frames":  { "type": "integer", "description": "Complete frames delivered to the receive callback since boot" },
          "crc_errors": { "type": "integer", "description": "Frames discarded due to CRC mismatch" },
          "ack_loss":   { "type": "integer", "description": "ACK timeouts / unacknowledged DATA_ACK frames" }
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
      "EchoResultResponse": {
        "type": "object",
        "properties": {
          "sent":         { "type": "string",  "description": "The text that was sent" },
          "received":     { "type": "string",  "description": "The text returned by the remote node (empty string on failure)" },
          "seqNo":        { "type": "integer", "description": "Frame sequence number of the echo request" },
          "roundTripMs":  { "type": "integer", "description": "Round-trip time in milliseconds" },
          "success":      { "type": "boolean", "description": "true when a valid echo response was received" }
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
      "SensorDataPayload": {
        "type": "object",
        "required": ["device", "metrics"],
        "properties": {
          "device": { "type": "string", "maxLength": 12, "description": "Device name, e.g. SensorA" },
          "metrics": {
            "type": "object",
            "description": "Map of metric name to measurement. Each value is an object with value (required, number or string up to 12 bytes UTF-8), unit (optional, max 5 chars), scale (optional, default 1, ignored for strings).",
            "additionalProperties": {
              "type": "object",
              "required": ["value"],
              "properties": {
                "value": { "oneOf": [{ "type": "integer" }, { "type": "string" }, { "type": "boolean" }], "description": "Numeric value (scaled integer), string (max 12 bytes UTF-8; JSON type is discriminator), or boolean (stored as text 'true'/'false'). Strings > 12 bytes or invalid types cause rejection." },
                "unit":  { "type": "string",  "maxLength": 5 },
                "scale": { "type": "integer", "description": "Divisor for real value reconstruction (numbers only; ignored for strings). Default 1." }
              }
            }
          }
        },
        "example": { "device": "SensorA", "metrics": { "Voltage": { "value": 335, "unit": "V", "scale": 100 }, "Date": { "value": "30.10.2026" }, "Enabled": { "value": true } } }
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
 *   %s  LoRa air data rate, e.g. "62.5k"          (banner)
 *   %s  Build timestamp __DATE__ " " __TIME__     (banner)
 *   %s  Gateway tier name for JS TIER variable    (x-tier filter)
 *
 * IP address is not injected server-side. A one-line JS snippet reads
 * location.hostname so the displayed address is always the interface the
 * browser actually used - correct for both STA mode (LAN DHCP IP) and
 * SoftAP mode (192.168.4.1).
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
  " | Air: <b>%s</b>"
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
