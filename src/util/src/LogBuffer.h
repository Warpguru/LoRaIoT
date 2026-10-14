#pragma once

#include <Arduino.h>

/**
 * LogBuffer.h - Levelled logger with in-RAM ring buffer and Serial output.
 *
 * Replaces all ESP_LOG* calls throughout the codebase. Each log call:
 *   1. Prints a formatted line to UART0 via Serial.println() - always works,
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
 * Output format (level field is always left-justified in 5 chars via %-5s):
 *   [INFO ][Application.cpp:52] Starting SerialController v1.0.0 ...
 *
 * The active level is readable/writable at runtime via:
 *   Log.getLogLevel() / Log.setLogLevel(LogLevel)
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
// 128 lines × 160 chars = ~20 KB - well within the ~300 KB free heap budget.
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
  // Log4j2-compatible integer ordering: higher = less severe.
  // levelOrdinal field stores the value; levelOrdinal() method exposes it.
  int levelOrdinal() const {
    return levelOrdinalValue;
  }

  // Canonical bare name, e.g. "INFO". Used for NVS, API, and comparisons.
  // Java equivalent: LogLevel.name()
  const char* name() const {
    return levelName;
  }

  // The five canonical instances - mirrors Java enum constants.
  static const LogLevel TRACE;
  static const LogLevel DEBUG;
  static const LogLevel INFO;
  static const LogLevel WARN;
  static const LogLevel ERROR;

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

  // Internal methods - do not call directly. Use the Log_* macros below.
  void logError(const char* file, int line, const char* format, ...);
  void logWarn(const char* file, int line, const char* format, ...);
  void logInfo(const char* file, int line, const char* format, ...);
  void logDebug(const char* file, int line, const char* format, ...);
  void logTrace(const char* file, int line, const char* format, ...);

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

/** Global singleton. */
extern LogBufferClass Log;

// ---- Convenience macros ----------------------------------------------------
// __FILE__ and __LINE__ are captured at the call site by the preprocessor.
// ## before __VA_ARGS__ swallows the trailing comma when no extra args are given
// (GNU extension; supported by GCC/Clang/MSVC and all Arduino toolchains).
#define Log_error(fmt, ...) Log.logError(__FILE__, __LINE__, fmt, ##__VA_ARGS__)  // NOLINT
#define Log_warn(fmt, ...) Log.logWarn(__FILE__, __LINE__, fmt, ##__VA_ARGS__)    // NOLINT
#define Log_info(fmt, ...) Log.logInfo(__FILE__, __LINE__, fmt, ##__VA_ARGS__)    // NOLINT
#define Log_debug(fmt, ...) Log.logDebug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)  // NOLINT
#define Log_trace(fmt, ...) Log.logTrace(__FILE__, __LINE__, fmt, ##__VA_ARGS__)  // NOLINT
