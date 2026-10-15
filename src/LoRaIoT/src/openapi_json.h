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
    { "name": "Diagnostics", "description": "Logging, hardware status, and E220 wiring check" }
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
        "description": "Returns the currently configured LoRa node address. This endpoint is available on all nodes (returns `0` for LOCAL, and `1-254` for REMOTE).",
        "responses": {
          "200": {
            "description": "OK",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "address": { "type": "integer", "description": "The active LoRa node address (0-254)" }
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
        "description": "Sets the active LoRa node address. The new address (`1-254` for REMOTE) is persisted in NVS and survives reboots. This endpoint is only available on REMOTE instances; accessing it on a LOCAL instance returns a bad request.",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "required": ["address"],
                "properties": {
                  "address": { "type": "integer", "minimum": 1, "maximum": 254, "description": "New LoRa address (1-254)" }
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
                    "address": { "type": "integer", "description": "The new active LoRa address (1-254)" }
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
        "description": "Schedules a hardware check of the E220 LoRa module (UART TX/RX, M0, M1, AUX pins, and register read/write round-trip). The check runs on the next pass of applicationLoop() - this endpoint returns immediately. Read the outcome from GET /api/log a few seconds later; look for lines starting with 'E220 diag:'. Uses only the temporary write command (0xC2) so nothing is stored in the module's flash. Removed once Milestone 3 (LoRaTransport) takes over UART2.",
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
        "summary": "Clear WiFi credentials and NVS settings, then reboot",
        "description": "Erases the stored WiFi SSID/password and the gateway tier from NVS, then immediately reboots into the WiFiManager captive portal. The device will be unreachable until credentials and tier are re-entered via the portal.",
        "responses": {
          "200": {
            "description": "Settings cleared; ESP32 is rebooting",
            "content": { "text/plain": { "schema": { "type": "string" } } }
          }
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
      }
    }
  }
}
)rawjson";
