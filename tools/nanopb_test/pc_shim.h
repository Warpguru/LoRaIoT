/* pc_shim.h - Minimal stubs that let DeviceTable.h/.cpp compile on the PC.
 *
 * USAGE: pass -include tools/nanopb_test/pc_shim.h to the compiler so this
 * header is injected before any other header in every translation unit.
 * That way LogBuffer.h is already guarded out before DeviceTable.cpp
 * reaches its '#include "../../util/src/LogBuffer.h"'.
 *
 * DeviceTable itself has no hardware dependency; it only uses standard C types
 * and nanopb.  The three Arduino/FreeRTOS pull-ins that block a PC build are:
 *
 *   <Arduino.h>      included by DeviceTable.h and LoRaFrame.h
 *                    -> only needed for uint8_t/uint32_t/size_t/bool
 *   LogBuffer.h      included by DeviceTable.cpp
 *                    -> only used via Log_* macros
 *   SemaphoreHandle_t  referenced inside LogBufferClass (not used by DeviceTable)
 *
 * This header is placed on the include path BEFORE the real Arduino headers.
 * It is included first in devicetable_test.cpp, and also satisfies the
 * '#include <Arduino.h>' and '#include "LogBuffer.h"' found in DeviceTable.h
 * and DeviceTable.cpp by being on the same -I path.
 *
 * Do NOT include this file in any firmware translation unit.
 */

#pragma once

/* ---- Standard types that Arduino.h normally provides -------------------- */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- FreeRTOS type stub (used in LogBufferClass but never called on PC) -- */
typedef void* SemaphoreHandle_t;

/* ---- Arduino.h stub ----------------------------------------------------- */
/* Prevent the real Arduino.h from being included by anything downstream.
 * Any translation unit that sees this header first will already have
 * _ARDUINO_WIRING_PRIVATE_H_ and _ARDUINO_H_ defined, so a real Arduino.h
 * include becomes a no-op. */
#ifndef ARDUINO
#define ARDUINO 10819   /* any non-zero value satisfies #if ARDUINO checks */
#endif
/* Guard macros used by the real Arduino.h - define them so re-inclusion is
 * skipped even if the header is found on the path (it won't be on a PC, but
 * be explicit). */
#ifndef _ARDUINO_H_
#define _ARDUINO_H_
#endif
#ifndef Arduino_h
#define Arduino_h
#endif

/* String type used by LogBufferClass::getJson(String&).
 * DeviceTable does not call getJson(), so an incomplete type is enough. */
class String {
public:
    String() {}
    void operator+=(const char*) {}
};

/* ---- LogBuffer.h shim --------------------------------------------------- */
/* Redirect Log_* macros to printf/fprintf so DeviceTable.cpp compiles and
 * produces visible output without any ESP32 infrastructure. */
#ifndef LOGBUFFER_H_SHIM
#define LOGBUFFER_H_SHIM

/* Stub class - DeviceTable.cpp only calls Log_* macros, never the class directly. */
class LogLevel {
public:
    constexpr LogLevel(const char* n, int o) : levelName(n), levelOrdinalValue(o) {}
    int         levelOrdinal() const { return levelOrdinalValue; }
    const char* name()         const { return levelName; }
    static const LogLevel TRACE;
    static const LogLevel DEBUG;
    static const LogLevel INFO;
    static const LogLevel WARN;
    static const LogLevel ERROR;
    static const LogLevel* const* values(size_t& count);
private:
    const char* levelName;
    int         levelOrdinalValue;
};

class LogBufferClass {
public:
    static const LogLevel& DEFAULT_LEVEL;
    void begin() {}
    void saveLevel() const {}
    void logError(const char* file, int line, const char* fmt, ...);
    void logWarn (const char* file, int line, const char* fmt, ...);
    void logInfo (const char* file, int line, const char* fmt, ...);
    void logDebug(const char* file, int line, const char* fmt, ...);
    void logTrace(const char* file, int line, const char* fmt, ...);
    const LogLevel& getLogLevel() const { return LogLevel::INFO; }
    void setLogLevel(const LogLevel&) {}
    bool setLevelFromString(const char*) { return false; }
    const char* getLevelName() const { return "INFO"; }
    void getJson(String&) const {}
    void clear() {}
};

extern LogBufferClass LogBuffer;

/* Log_* macros: [LEVEL][file:line] message, to stdout (INFO) or stderr (WARN/ERROR) */
#define Log_error(fmt, ...) \
    fprintf(stderr, "[ERROR][%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define Log_warn(fmt, ...) \
    fprintf(stderr, "[WARN ][%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define Log_info(fmt, ...) \
    printf( "[INFO ][%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define Log_debug(fmt, ...) \
    printf( "[DEBUG][%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define Log_trace(fmt, ...) \
    printf( "[TRACE][%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

/* Guard against the real LogBuffer.h being included after this shim. */
#define LOGBUFFER_H_INCLUDED

#endif /* LOGBUFFER_H_SHIM */
