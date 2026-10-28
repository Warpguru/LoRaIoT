#pragma once

/**
 * Diagnostics.h - Hardware bring-up checks (E220 wiring and register test).
 *
 * diagnosticsLoop()    Call from applicationLoop() before handleServerRequests().
 *                      Runs the E220 check on the first call (boot) and again
 *                      whenever diagnosticsRequest() has been called. Otherwise
 *                      it costs one atomic flag test.
 *
 * diagnosticsRequest() Thread-safe. Asks for a re-run on the next loop pass.
 *                      Used by GET /api/diag/e220 (see doc/RestApi.md).
 *                      It does NOT touch UART2
 *                      itself, because ESPAsyncWebServer callbacks must never do
 *                      Serial2 I/O (see AGENTS.md threading model).
 *
 * Each run also performs a software-only nanopb check: a PING payload is encoded,
 * compared with the bytes documented in doc/LoRaProtocol.md section 4.2 and decoded
 * again (log lines start with "Protobuf diag:"). It needs no radio.
 *
 * Results are reported only through the Log_* macros: visible on the serial
 * monitor and via GET /api/log.
 *
 * UART2 ownership: the check borrows UART2 via LoRaTransport.pause()/resume()
 * so it can coexist with the loraTransportTask that owns UART2 at runtime.
 */
/**
 * Drive the diagnostics state machine. Call from applicationLoop() on every tick,
 * before handleServerRequests(). Runs the E220 wiring check on the first call (boot)
 * and on any subsequent call after diagnosticsRequest() has been invoked.
 */
void diagnosticsLoop();

/**
 * Request a re-run of the E220 hardware check on the next diagnosticsLoop() tick.
 * Thread-safe: safe to call from any core or task (e.g. from a REST handler).
 * Does not access UART2 directly.
 */
void diagnosticsRequest();
