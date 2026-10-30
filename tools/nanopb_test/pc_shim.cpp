/* pc_shim.cpp - Definitions for the LogBuffer stub used in PC-side tests.
 * See pc_shim.h for the declarations and rationale.
 */
#include "pc_shim.h"

/* LogLevel static constants */
const LogLevel LogLevel::TRACE("TRACE", 0);
const LogLevel LogLevel::DEBUG("DEBUG", 1);
const LogLevel LogLevel::INFO ("INFO",  2);
const LogLevel LogLevel::WARN ("WARN",  3);
const LogLevel LogLevel::ERROR("ERROR", 4);

static const LogLevel* const ALL_LEVELS[] = {
    &LogLevel::TRACE, &LogLevel::DEBUG, &LogLevel::INFO,
    &LogLevel::WARN,  &LogLevel::ERROR
};
const LogLevel* const* LogLevel::values(size_t& count) {
    count = 5;
    return ALL_LEVELS;
}

const LogLevel& LogBufferClass::DEFAULT_LEVEL = LogLevel::INFO;

/* Global singleton */
LogBufferClass LogBuffer;

static void log_dispatch(const char* level, FILE* out,
                         const char* file, int line,
                         const char* fmt, va_list ap) {
    /* Print [LEVEL][basename:line] message matching the firmware format */
    const char* base = file;
    for (const char* p = file; *p; p++) {
        if (*p == '/' || *p == '\\') { base = p + 1; }
    }
    fprintf(out, "[%-5s][%s:%d] ", level, base, line);
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
}

void LogBufferClass::logError(const char* file, int line, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_dispatch("ERROR", stderr, file, line, fmt, ap); va_end(ap);
}
void LogBufferClass::logWarn(const char* file, int line, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_dispatch("WARN",  stderr, file, line, fmt, ap); va_end(ap);
}
void LogBufferClass::logInfo(const char* file, int line, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_dispatch("INFO",  stdout, file, line, fmt, ap); va_end(ap);
}
void LogBufferClass::logDebug(const char* file, int line, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_dispatch("DEBUG", stdout, file, line, fmt, ap); va_end(ap);
}
void LogBufferClass::logTrace(const char* file, int line, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_dispatch("TRACE", stdout, file, line, fmt, ap); va_end(ap);
}
