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
 * Results are reported only through the Log_* macros: visible on the serial
 * monitor and via GET /api/log.
 *
 * UART2 ownership: the check opens and closes UART2 itself. Remove it (and the
 * calls to it) when Milestone 3 adds the LoRaTransport task that owns UART2.
 */
void diagnosticsLoop();
void diagnosticsRequest();
