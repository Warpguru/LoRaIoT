#include "LogBuffer.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/**
 * LogBuffer.cpp - Levelled logger with in-RAM ring buffer and Serial output.
 *
 * Level ordering matches Log4j2: higher levelOrdinal() = less severe.
 * A message is emitted when its levelOrdinal() >= the active level's levelOrdinal().
 * Default active level: INFO (200). TRACE (0) is most verbose.
 *
 * Each Log_info/warn/error/debug/trace(fmt, ...) macro call:
 *   1. Expands to logInfo(__FILE__, __LINE__, fmt, ...) - call site captured.
 *   2. Checks the active level; discards if below threshold.
 *   3. Formats "[LEVEL][file:line] message" onto the stack.
 *   4. Prints to Serial (UART0) - no hooks, always works.
 *   5. Appends to the ring buffer under a FreeRTOS mutex.
 *
 * No ESP_LOG* macros, no linker wraps, no vprintf hooks.
 *
 * No Java equivalent - ESP32-specific remote diagnostics facility.
 */

// Global singleton defined here; declared extern in LogBuffer.h.
LogBufferClass Log;

// ---- LogLevel static constants ---------------------------------------------
// String literals are stored in flash (PROGMEM-compatible); no heap used.

const LogLevel LogLevel::TRACE("TRACE", 0);
const LogLevel LogLevel::DEBUG("DEBUG", 100);
const LogLevel LogLevel::INFO ("INFO",  200);
const LogLevel LogLevel::WARN ("WARN",  300);
const LogLevel LogLevel::ERROR("ERROR", 400);

// values() - returns a pointer to a static array of const LogLevel pointers.
// The array is a static local, initialised exactly once, no heap allocation.
const LogLevel* const* LogLevel::values(size_t& count) {
  static const LogLevel* const all[] = {
    &LogLevel::TRACE,
    &LogLevel::DEBUG,
    &LogLevel::INFO,
    &LogLevel::WARN,
    &LogLevel::ERROR
  };
  count = sizeof(all) / sizeof(all[0]);
  return all;
}

// ---- LogBufferClass static members -----------------------------------------

const LogLevel& LogBufferClass::DEFAULT_LEVEL = LogLevel::INFO;

// ---- Helpers ---------------------------------------------------------------

// Strip directory prefix from __FILE__ so only the filename appears in output.
static const char* fileBasename(const char* path) {
  const char* result = path;
  while (*path) {
    if (*path == '/' || *path == '\\') {
      result = path + 1;
    }
    path++;
  }
  return result;
}

// ---- LogBufferClass --------------------------------------------------------

void LogBufferClass::begin() {
  mutex = xSemaphoreCreateMutex();

  // Load persisted log level from NVS; keep the compile-time default on error.
  // If nothing is stored yet (fresh device or pre-persistence firmware), write
  // the default level now so it is present in NVS from the next boot onward.
  bool found = false;
  Preferences prefs;
  if (prefs.begin(LOG_NVS_NAMESPACE, /* readOnly= */ true)) {
    String stored = prefs.getString(LOG_NVS_KEY_LEVEL, "");
    prefs.end();
    if (stored.length() > 0 && setLevelFromString(stored.c_str())) {
      // Valid level found in NVS - applied successfully.
      found = true;
    }
    // If stored.length() > 0 but setLevelFromString returned false the stored
    // string is unrecognised (e.g. corrupted NVS entry).  Fall through to the
    // !found branch so the compile-time default is written over it.
  }
  if (!found) {
    // Nothing stored, or the stored value was invalid: persist the default
    // so NVS is correct from the next boot onward.
    activeLevel = &DEFAULT_LEVEL;
    saveLevel();
  }
}

void LogBufferClass::saveLevel() const {
  Preferences prefs;
  if (prefs.begin(LOG_NVS_NAMESPACE, /* readOnly= */ false)) {
    prefs.putString(LOG_NVS_KEY_LEVEL, activeLevel->name());
    prefs.end();
  }
}

void LogBufferClass::logError(const char* file, int line, const char* format, ...) {
  va_list formatArgs;
  va_start(formatArgs, format);
  append(LogLevel::ERROR, file, line, format, formatArgs);
  va_end(formatArgs);
}

void LogBufferClass::logWarn(const char* file, int line, const char* format, ...) {
  va_list formatArgs;
  va_start(formatArgs, format);
  append(LogLevel::WARN, file, line, format, formatArgs);
  va_end(formatArgs);
}

void LogBufferClass::logInfo(const char* file, int line, const char* format, ...) {
  va_list formatArgs;
  va_start(formatArgs, format);
  append(LogLevel::INFO, file, line, format, formatArgs);
  va_end(formatArgs);
}

void LogBufferClass::logDebug(const char* file, int line, const char* format, ...) {
  va_list formatArgs;
  va_start(formatArgs, format);
  append(LogLevel::DEBUG, file, line, format, formatArgs);
  va_end(formatArgs);
}

void LogBufferClass::logTrace(const char* file, int line, const char* format, ...) {
  va_list formatArgs;
  va_start(formatArgs, format);
  append(LogLevel::TRACE, file, line, format, formatArgs);
  va_end(formatArgs);
}

const LogLevel& LogBufferClass::getLogLevel() const {
  return *activeLevel;
}

void LogBufferClass::setLogLevel(const LogLevel& logLevel) {
  activeLevel = &logLevel;
}

bool LogBufferClass::setLevelFromString(const char* name) {
  size_t count;
  const LogLevel* const* levels = LogLevel::values(count);
  for (size_t i = 0; i < count; i++) {
    if (strcasecmp(name, levels[i]->name()) == 0) {
      activeLevel = levels[i];
      return true;
    }
  }
  return false;
}

const char* LogBufferClass::getLevelName() const {
  return activeLevel->name();
}

void LogBufferClass::getJson(String& out) const {
  // Build JSON directly from the ring buffer under the mutex.
  // No large stack allocation - ArduinoJson allocates on the heap.
  xSemaphoreTake(mutex, portMAX_DELAY);

  int oldestLineIndex = (count < LOG_BUFFER_LINES) ? 0 : head;
  int lineCount = count;

  JsonDocument doc;
  doc["level"] = activeLevel->name();
  JsonArray arr = doc["log"].to<JsonArray>();
  for (int lineOffset = 0; lineOffset < lineCount; lineOffset++) {
    int bufferIndex = (oldestLineIndex + lineOffset) % LOG_BUFFER_LINES;
    arr.add(lines[bufferIndex]);
  }

  xSemaphoreGive(mutex);

  serializeJson(doc, out);
}

void LogBufferClass::clear() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  head = 0;
  count = 0;
  xSemaphoreGive(mutex);
}

// ---- Private ---------------------------------------------------------------

void LogBufferClass::append(const LogLevel& logLevel, const char* file, int line, const char* format, va_list formatArgs) {
  // Log4j2 semantics: emit when message levelOrdinal() >= active levelOrdinal().
  // e.g. active=INFO(200): emit ERROR(400) and WARN(300), suppress DEBUG(100) and TRACE(0).
  if (logLevel.levelOrdinal() < activeLevel->levelOrdinal()) {
    return;
  }

  // Format the message, then the full log line with location prefix.
  // %-5s left-justifies the level name in a 5-char field so all prefixes
  // align: "[ERROR]", "[WARN ]", "[INFO ]", "[DEBUG]", "[TRACE]".
  char message[LOG_BUFFER_LINE_LEN];
  vsnprintf(message, sizeof(message), format, formatArgs);

  char logLine[LOG_BUFFER_LINE_LEN];
  snprintf(logLine, sizeof(logLine), "[%-5s][%s:%d] %s",
           logLevel.name(), fileBasename(file), line, message);

  // Print to Serial (UART0) - no hooks, no build flags required.
  Serial.println(logLine);

  // Append to ring buffer under mutex.
  if (mutex == nullptr) {
    return;
  }

  xSemaphoreTake(mutex, portMAX_DELAY);

  strncpy(lines[head], logLine, LOG_BUFFER_LINE_LEN - 1);
  lines[head][LOG_BUFFER_LINE_LEN - 1] = '\0';
  head = (head + 1) % LOG_BUFFER_LINES;
  if (count < LOG_BUFFER_LINES) {
    count++;
  }

  xSemaphoreGive(mutex);
}
