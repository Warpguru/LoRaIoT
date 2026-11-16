#pragma once

/* On PC builds, pc_shim.h (force-included via -include) defines
 * LOGBUFFER_H_INCLUDED and provides all Log_* stubs before this header
 * is reached. Skip the Arduino-dependent body in that case. */
#ifndef LOGBUFFER_H_INCLUDED

#include <Arduino.h>

/**
 * LogBuffer.h - Levelled logger with in-RAM ring buffer and Serial output.
 *
 * Replaces all ESP_LOG* calls throughout the codebase. Each log call:
 *   1. Prints a formatted line to UART0 via one Serial.write() of text + CRLF (a single
 *      write keeps the line whole when several tasks log at once) - always works,
 *      regardless of Arduino IDE / PlatformIO / build flags.
 *   2. Appends the same line to a fixed-size ring buffer accessible via
 *      GET /api/log, so logs are retrievable without a USB connection.
 *
 * A runtime log level (default INFO) gates both outputs identically.
 *
 * Use the macros - __FILE__ and __LINE__ are captured automatically:
 *   Log_error(fmt, ...)   - level ERROR
 *   Log_warn( fmt, ...)   - level WARN
 *   Log_info( fmt, ...)   - level INFO  (default active level)
 *   Log_debug(fmt, ...)   - level DEBUG
 *   Log_trace(fmt, ...)   - level TRACE
 *
 * Choosing the level (rule of thumb)
 * ----------------------------------
 * At the default INFO level a node in normal operation should log only a trickle:
 * the boot summary and ONE line per transmitted or received message. Everything else
 * is a state change or an anomaly (WARN), a failure that ends the node (ERROR), or
 * technical detail that only matters when researching a problem (DEBUG / TRACE).
 * Ask in this order and take the first match:
 *
 *   ERROR  Fatal, likely unrecoverable: no further functionality is to be expected.
 *          Halt or SOS blink, kernel-panic-like states, a mutex / queue / task that
 *          could not be created, the node has no role (tier not configured), a failed
 *          boot self-test. A recoverable failure (encode or sendFrame failed, NVS save
 *          failed, a rejected MQTT /set message) is NOT an error: use WARN.
 *
 *   WARN   Something changed or is not as expected, but the node keeps working.
 *          - State changes: peer offline / online again / rebooted, WiFi or MQTT
 *            connected or lost, IP address obtained from DHCP.
 *          - Configuration changes at runtime (REST API, captive portal) and
 *            defaults used because nothing was stored.
 *          - Non-default or unexpected behaviour: BAD_REQUEST responses, an unknown
 *            device, a duplicate frame that needs a re-ACK, a retransmit, a timeout,
 *            a full queue or table, dropped or rejected input, a decode failure.
 *
 *   INFO   Business-level activity, readable like a heartbeat:
 *          - Boot and configuration summary (banner, config loaded, "LoRa started").
 *          - Exactly ONE line per TX or RX message (PING, BEACON, ACK, DATA, REQUEST,
 *            RESPONSE) so the activity is visible. Details of that message go to DEBUG.
 *          - The outcome of a multi-step job (crawl complete, push OK, diagnostics PASS).
 *          - An explicit operator action (e.g. GET /api/diag/e220 requested).
 *
 *   DEBUG  Technical detail that is typically only interesting when researching an
 *          issue: REST access logs (GET /...), task start messages, registry
 *          bookkeeping, register values, config commands, routine successes of
 *          internal steps, the step-by-step lines of the boot self-tests, frames that
 *          are dropped by design (foreign DST, duplicates).
 *
 *   TRACE  High-volume dumps that would flood even DEBUG: per-frame hex dumps.
 *
 * Further rules:
 *   - Log an event once, at the place that owns it. If two layers would log the same
 *     event (e.g. the task and the registry both noticing a peer is offline), keep the
 *     one that states the cause at its level and demote the other to DEBUG.
 *   - Every message that leaves the node over the console or GET /api/log goes through
 *     a Log_* macro. Do not use Serial.print*, printf or ESP_LOG*, and do not write a
 *     helper that logs with a hard-wired level: pass the level in (see logHex() in
 *     LoRaTransport.cpp, which also skips formatting when that level is inactive).
 *   - Build expensive log text (hex dumps, JSON) only after checking getLogLevel().
 *
 * Output format (level field is always left-justified in 5 chars via %-5s):
 *   [INFO ][Application.cpp:52] Starting SerialController v1.0.0 ...
 *
 * The active level is readable/writable at runtime via:
 *   LogBuffer.getLogLevel() / LogBuffer.setLogLevel(LogLevel)
 * and over HTTP via PUT /api/log/level {"level":"DEBUG"}.
 *
 * The active log level is persisted in ESP32 NVS (namespace "sc_prefs",
 * key "logLevel") so it survives reboots.  begin() loads it automatically;
 * saveLevel() writes the current level to NVS and is called whenever the
 * level changes (WiFiManager portal submit or PUT /api/log/level).
 *
 * No Java equivalent - ESP32-specific remote diagnostics facility.
 * Java uses Log4j2/SLF4J; this is the direct embedded equivalent.
 */

// Ring buffer dimensions.
// 128 lines × 160 chars = ~20 KB static BSS.
// The fixed-width row layout wastes ~110 B/line on average (typical lines are ~50 chars).
// Improvement alternatives are documented in doc/TechDebt.md §1.
static constexpr int LOG_BUFFER_LINES = 128;
static constexpr int LOG_BUFFER_LINE_LEN = 160;

// NVS namespace and key used to persist the log level across reboots.
// Defined here so any future translation unit (e.g. a settings helper) can
// reference the same strings without hardcoding them.
static constexpr const char* LOG_NVS_NAMESPACE = "sc_prefs";
static constexpr const char* LOG_NVS_KEY_LEVEL = "logLevel";

/**
 * Java-style enum class for log levels.
 *
 * Java equivalent: an enum with a name attribute and levelOrdinal() method,
 * matching Log4j2 integer ordering: higher integer = less severe.
 * A message is emitted when its levelOrdinal() >= the active level's levelOrdinal().
 *
 * All instances are the five public static constants (TRACE…ERROR).
 * No heap allocation - name() returns a string literal in flash; values()
 * returns a pointer to a static array in flash.
 *
 * Passed by const reference (const LogLevel&) everywhere to avoid copying.
 */
class LogLevel {
 public:
  /**
   * Log4j2-compatible integer ordinal.
   * Higher value = less severe (TRACE = 0, ERROR = 4).
   * A message is emitted when its ordinal >= the active level's ordinal.
   */
  int levelOrdinal() const {
    return levelOrdinalValue;
  }

  /**
   * Canonical bare name string, e.g. "INFO".
   * Used for NVS persistence, REST API responses, and log-line formatting.
   */
  const char* name() const {
    return levelName;
  }

  static const LogLevel TRACE;  ///< Most verbose level (ordinal 0).
  static const LogLevel DEBUG;  ///< Debug-level messages (ordinal 1).
  static const LogLevel INFO;   ///< Informational messages - default active level (ordinal 2).
  static const LogLevel WARN;   ///< Warning messages (ordinal 3).
  static const LogLevel ERROR;  ///< Error messages - least verbose (ordinal 4).

  /**
   * All levels in ascending severity order, as a plain C array.
   * Java equivalent: LogLevel.values()
   * No heap allocation - backed by a static array in flash.
   *
   * Usage:
   *   size_t count;
   *   const LogLevel* const* levels = LogLevel::values(count);
   *   for (size_t i = 0; i < count; i++) { ... levels[i]->name() ... }
   */
  static const LogLevel* const* values(size_t& count);

 private:
  // Private constructor - only the five static constants may be created.
  constexpr LogLevel(const char* name, int ordinal)
      : levelName(name), levelOrdinalValue(ordinal) {}

  const char* levelName;
  int         levelOrdinalValue;
};

/**
 * LogBufferClass - Singleton that manages the logging ring buffer and active level.
 *
 * This class follows the standard Arduino Singleton Pattern:
 * - 'LogBufferClass' is the underlying class type. A suffix is used here
 *   because C++ prevents a global variable from having the exact same name
 *   as its class type in the same scope (name shadowing collision).
 * - 'Log' is the single globally exported object instance (mirroring 'Serial' / 'HardwareSerial').
 * - Application code should always interact with the 'Log' instance or the 'Log_*' macros.
 */
class LogBufferClass {
 public:
  // Default active level used when no persisted value is found in NVS.
  static const LogLevel& DEFAULT_LEVEL;

  /**
   * Initialise the ring buffer mutex and load the persisted log level from
   * NVS.  Must be called once from applicationSetup() before any Log_* call.
   * If no level has been stored yet the default (INFO) is used.
   */
  void begin();

  /**
   * Persist the current log level to NVS (namespace "sc_prefs", key
   * "logLevel").  Call after every runtime level change so the level
   * survives a reboot.  Safe to call from any task.
   */
  void saveLevel() const;

  /** @name Internal log dispatch methods - do not call directly; use the Log_* macros. */
  ///@{
  void logError(const char* file, int line, const char* format, ...);
  void logWarn (const char* file, int line, const char* format, ...);
  void logInfo (const char* file, int line, const char* format, ...);
  void logDebug(const char* file, int line, const char* format, ...);
  void logTrace(const char* file, int line, const char* format, ...);
  ///@}

  /** Returns the current active log level. */
  const LogLevel& getLogLevel() const;

  /**
   * Set the active log level at runtime.
   * Messages with a lower levelOrdinal() are silently discarded.
   */
  void setLogLevel(const LogLevel& logLevel);

  /**
   * Parse a level name string ("ERROR","WARN","INFO","DEBUG","TRACE").
   * Returns false and leaves the level unchanged if unrecognised.
   */
  bool setLevelFromString(const char* name);

  /**
   * Name of the current log level as a bare C string, e.g. "INFO".
   * Convenience wrapper around getLogLevel().name().
   */
  const char* getLevelName() const;

  /**
   * Serialise the ring buffer to a JSON string.
   * Format: {"log":["line0","line1",...],"level":"INFO"}
   * Built under the mutex - no large stack allocation.
   */
  void getJson(String& out) const;

  /** Clear all entries from the ring buffer. Thread-safe. */
  void clear();

 private:
  void append(const LogLevel& logLevel, const char* file, int line, const char* format, va_list formatArgs);

  char lines[LOG_BUFFER_LINES][LOG_BUFFER_LINE_LEN];
  int head = 0;
  int count = 0;
  const LogLevel* activeLevel = &LogLevel::INFO;
  SemaphoreHandle_t mutex = nullptr;
};

// Global singleton instance exported for direct use across the application (mirroring 'Serial')
extern LogBufferClass LogBuffer;

// ---- Convenience macros ----------------------------------------------------
// __FILE__ and __LINE__ are captured at the call site by the preprocessor.
// ## before __VA_ARGS__ swallows the trailing comma when no extra args are given
// (GNU extension; supported by GCC/Clang/MSVC and all Arduino toolchains).
#define Log_error(fmt, ...) LogBuffer.logError(__FILE__, __LINE__, fmt, ##__VA_ARGS__)  // NOLINT
#define Log_warn(fmt, ...) LogBuffer.logWarn(__FILE__, __LINE__, fmt, ##__VA_ARGS__)    // NOLINT
#define Log_info(fmt, ...) LogBuffer.logInfo(__FILE__, __LINE__, fmt, ##__VA_ARGS__)    // NOLINT
#define Log_debug(fmt, ...) LogBuffer.logDebug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)  // NOLINT
#define Log_trace(fmt, ...) LogBuffer.logTrace(__FILE__, __LINE__, fmt, ##__VA_ARGS__)  // NOLINT

#endif /* LOGBUFFER_H_INCLUDED */
