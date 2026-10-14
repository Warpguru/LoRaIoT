#include "Diagnostics.h"

#include <Arduino.h>
#include <atomic>
#include <HardwareSerial.h>

#include "../../LoRaIoT/src/LogBuffer.h"
#include "../../LoRaIoT/src/StatusLed.h"

/**
 * Diagnostics.cpp - E220 wiring and register read/write check.
 *
 * WHAT IT PROVES
 *   The E220 accepts commands only in config mode (Mode 3: M1=1, M0=1) at a
 *   fixed 9600 8N1. A correct reply to a register read therefore proves that
 *   all of these are wired correctly:
 *     - M0 and M1 reach the module (otherwise it stays in Mode 0 and treats
 *       our command as radio data, so there is no reply)
 *     - ESP32 TX (GPIO17) -> E220 RXD
 *     - E220 TXD -> ESP32 RX (GPIO16)
 *     - AUX -> GPIO15 (it must go HIGH after every mode change)
 *   It does NOT prove the antenna or the RF path. That is Milestone 5.
 *
 * SEQUENCE (each step logs PASS or FAIL)
 *   1. Mode 0 idle: AUX should read HIGH
 *   2. Enter Mode 3 and wait for AUX HIGH
 *   3. Read all 8 registers (C1 00 08)
 *   4. Temporarily write a different channel (C2), read back, compare
 *   5. Temporarily write the original values back (C2), read back, compare
 *   6. Return to Mode 0
 *
 * SAFETY
 *   Only the TEMPORARY write command (0xC2) is used, so nothing is stored in
 *   the module's flash. A power cycle always restores the previous settings.
 *
 * PROTOCOL (doc/Feasibility.md section 3.3)
 *   Read:            C1 + StartAddr + Length            -> C1 + StartAddr + Length + values
 *   Temporary write: C2 + StartAddr + Length + values   -> C1 + StartAddr + Length + values
 *   Malformed:       anything invalid                   -> FF FF FF
 */

// ---- Wiring (canonical pin mapping, see AGENTS.md) -------------------------

static constexpr uint8_t PIN_M0 = 4;    // output, mode control
static constexpr uint8_t PIN_M1 = 2;    // output, mode control
static constexpr uint8_t PIN_AUX = 15;  // input, HIGH = module ready
static constexpr uint8_t PIN_RX = 16;   // ESP32 RX2 <- E220 TXD
static constexpr uint8_t PIN_TX = 17;   // ESP32 TX2 -> E220 RXD

// ---- Protocol constants ----------------------------------------------------

static constexpr uint8_t CMD_READ = 0xC1;
static constexpr uint8_t CMD_WRITE_TEMP = 0xC2;
static constexpr uint8_t REG_START = 0x00;
static constexpr uint8_t REG_COUNT = 8;                 // ADDH ADDL REG0 REG1 REG2 REG3 CRYPT_H CRYPT_L
static constexpr size_t REPLY_LEN = 3 + REG_COUNT;      // C1 start len + 8 values
static constexpr uint8_t IDX_CHANNEL = 4;               // REG2 position within the 8 values
static constexpr uint8_t VERIFY_BYTES = 6;              // ADDH..REG3; CRYPT bytes are write-only (read as 0x00)
static constexpr uint32_t CONFIG_BAUD = 9600;           // fixed in Mode 3 regardless of REG0
static constexpr uint32_t AUX_TIMEOUT_MS = 1000;
static constexpr uint32_t REPLY_TIMEOUT_MS = 500;

// UART2 object private to this file. It is closed (end()) before returning so
// the future LoRa transport task can claim UART2 without conflict.
static HardwareSerial loraSerial(2);

// ---- Helpers ---------------------------------------------------------------

/** Formats bytes as "AA BB CC" into a small stack buffer for logging. */
static void toHex(const uint8_t* data, size_t len, char* out, size_t outSize) {
  size_t pos = 0;
  out[0] = '\0';
  for (size_t i = 0; i < len && pos + 4 < outSize; i++) {
    pos += snprintf(out + pos, outSize - pos, "%02X ", data[i]);
  }
}

/** Waits until AUX reaches the given level. Returns false on timeout. */
static bool waitAux(bool level, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (digitalRead(PIN_AUX) != level) {
    if (millis() - start > timeoutMs) {
      return false;
    }
    delay(1);
  }
  return true;
}

/**
 * Sets M1/M0 and waits for the module to signal ready (AUX rising edge).
 * The extra 2 ms after the edge is required before any UART data is sent.
 */
static bool setMode(bool m1, bool m0) {
  digitalWrite(PIN_M1, m1);
  digitalWrite(PIN_M0, m0);
  delay(10);  // let AUX react to the new mode before polling it
  const bool ok = waitAux(HIGH, AUX_TIMEOUT_MS);
  delay(2);
  return ok;
}

/**
 * Sends one command as a single atomic write and collects the reply.
 * Returns the number of reply bytes received (0 means no reply at all).
 */
static size_t transact(const uint8_t* cmd, size_t cmdLen, uint8_t* reply, size_t replyMax) {
  while (loraSerial.available()) {
    loraSerial.read();  // discard stale bytes so they cannot be mistaken for the reply
  }
  loraSerial.write(cmd, cmdLen);
  loraSerial.flush();

  size_t n = 0;
  uint32_t last = millis();
  while (millis() - last < REPLY_TIMEOUT_MS && n < replyMax) {
    if (loraSerial.available()) {
      reply[n++] = static_cast<uint8_t>(loraSerial.read());
      last = millis();  // restart the timeout after every received byte
    }
  }
  return n;
}

/** Reads the 8 registers into regs[]. Logs the reason on failure. */
static bool readRegisters(uint8_t* regs) {
  const uint8_t cmd[] = {CMD_READ, REG_START, REG_COUNT};
  uint8_t reply[REPLY_LEN];
  char hex[48];

  const size_t n = transact(cmd, sizeof(cmd), reply, sizeof(reply));
  toHex(reply, n, hex, sizeof(hex));
  Log_info("E220 diag:   read reply (%u bytes): %s", static_cast<unsigned>(n), hex);

  if (n == 0) {
    Log_error("E220 diag:   FAIL no reply. Check ESP32 TX(GPIO17)->E220 RXD, E220 TXD->ESP32 RX(GPIO16), M0/M1.");
    return false;
  }
  if (n == 3 && reply[0] == 0xFF) {
    Log_error("E220 diag:   FAIL FF FF FF. Module is in config mode but rejected the command.");
    return false;
  }
  if (n != REPLY_LEN || reply[0] != CMD_READ || reply[1] != REG_START || reply[2] != REG_COUNT) {
    Log_error("E220 diag:   FAIL unexpected reply format (expected C1 00 08 + 8 bytes).");
    return false;
  }
  memcpy(regs, reply + 3, REG_COUNT);
  return true;
}

/**
 * Pauses after a register write before the next command is sent. Hardware
 * result: AUX does NOT drop after a write (it stays HIGH), yet a read sent
 * immediately after the write echo was rejected with FF FF FF. The module
 * needs a short settle time that AUX does not signal, so a fixed delay is
 * used. The AUX log line is kept to confirm this on every run.
 */
static void waitForWriteToSettle() {
  delay(10);  // fixed settle time; AUX does not signal it (see above)
  const uint32_t start = millis();
  const bool wasLow = (digitalRead(PIN_AUX) == LOW);
  const bool ok = waitAux(HIGH, AUX_TIMEOUT_MS);
  delay(2);  // AUX must stay HIGH >= 2 ms before UART data is accepted
  Log_info("E220 diag:   after write: AUX was %s, settled in %lu ms%s", wasLow ? "LOW" : "HIGH",
           static_cast<unsigned long>(millis() - start), ok ? "" : " (TIMEOUT)");
}

/** Temporarily writes regs[], checks the module's echo, reads back and compares. */
static bool writeTempAndVerify(const uint8_t* regs) {
  uint8_t cmd[3 + REG_COUNT] = {CMD_WRITE_TEMP, REG_START, REG_COUNT};
  memcpy(cmd + 3, regs, REG_COUNT);

  uint8_t reply[REPLY_LEN];
  char hex[48];
  const size_t n = transact(cmd, sizeof(cmd), reply, sizeof(reply));
  toHex(reply, n, hex, sizeof(hex));
  Log_info("E220 diag:   write echo (%u bytes): %s", static_cast<unsigned>(n), hex);

  if (n != REPLY_LEN || reply[0] != CMD_READ || memcmp(reply + 3, regs, REG_COUNT) != 0) {
    Log_error("E220 diag:   FAIL write echo does not match what was written.");
    return false;
  }

  waitForWriteToSettle();

  uint8_t readBack[REG_COUNT];
  if (!readRegisters(readBack)) {
    return false;
  }
  if (memcmp(readBack, regs, VERIFY_BYTES) != 0) {
    Log_error("E220 diag:   FAIL register read-back differs from the value written.");
    return false;
  }
  return true;
}

// ---- The check itself ------------------------------------------------------

/** Runs the whole sequence. Returns true only if every step passed. */
static bool runE220Check() {
  Log_info("E220 diag: ===== start (M0=GPIO%u M1=GPIO%u AUX=GPIO%u RX=GPIO%u TX=GPIO%u) =====",
           PIN_M0, PIN_M1, PIN_AUX, PIN_RX, PIN_TX);

  pinMode(PIN_M0, OUTPUT);
  pinMode(PIN_M1, OUTPUT);
  pinMode(PIN_AUX, INPUT);
  loraSerial.begin(CONFIG_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  bool ok = false;

  do {
    // Step 1: Mode 0 idle level of AUX.
    Log_info("E220 diag: [1/6] Mode 0, checking AUX idle level");
    digitalWrite(PIN_M0, LOW);
    digitalWrite(PIN_M1, LOW);
    delay(100);
    if (digitalRead(PIN_AUX) != HIGH) {
      Log_error("E220 diag:   FAIL AUX is LOW. AUX wire open, wrong pin, or module has no power.");
      break;
    }
    Log_info("E220 diag:   PASS AUX is HIGH");

    // Step 2: enter config mode.
    Log_info("E220 diag: [2/6] Entering Mode 3 (M1=1, M0=1) at %u baud", static_cast<unsigned>(CONFIG_BAUD));
    if (!setMode(HIGH, HIGH)) {
      Log_error("E220 diag:   FAIL AUX did not return HIGH after the mode change. Check M0/M1/AUX wiring.");
      break;
    }
    Log_info("E220 diag:   PASS AUX went HIGH");

    // Step 3: read registers. This single reply validates UART TX, UART RX, M0 and M1.
    Log_info("E220 diag: [3/6] Reading registers");
    uint8_t original[REG_COUNT];
    if (!readRegisters(original)) {
      break;
    }
    Log_info("E220 diag:   PASS ADDH=%02X ADDL=%02X REG0=%02X REG1=%02X REG2(ch)=%02X REG3=%02X",
             original[0], original[1], original[2], original[3], original[4], original[5]);
    Log_info("E220 diag:   factory default (manual) is 00 00 62 00 17; REG3 is not specified there");

    // Step 4: temporary write of a different channel.
    const uint8_t testChannel = (original[IDX_CHANNEL] == 0x20) ? 0x21 : 0x20;
    Log_info("E220 diag: [4/6] Temporary write: channel %02X -> %02X", original[IDX_CHANNEL], testChannel);
    uint8_t changed[REG_COUNT];
    memcpy(changed, original, REG_COUNT);
    changed[IDX_CHANNEL] = testChannel;
    changed[6] = 0x00;  // CRYPT_H/L always read back as 0x00; write 0 to keep the echo comparable
    changed[7] = 0x00;
    if (!writeTempAndVerify(changed)) {
      break;
    }
    Log_info("E220 diag:   PASS write and read-back match");

    // Step 5: put the original values back.
    Log_info("E220 diag: [5/6] Temporary write: restoring original values");
    original[6] = 0x00;
    original[7] = 0x00;
    if (!writeTempAndVerify(original)) {
      break;
    }
    Log_info("E220 diag:   PASS original values restored");

    ok = true;
  } while (false);

  // Step 6: always leave the module in normal mode and release UART2.
  Log_info("E220 diag: [6/6] Returning to Mode 0 and releasing UART2");
  setMode(LOW, LOW);
  loraSerial.end();

  if (ok) {
    Log_info("E220 diag: ===== RESULT: PASS. UART, M0, M1 and AUX wiring verified =====");
  } else {
    Log_error("E220 diag: ===== RESULT: FAIL. See the FAIL line above for the likely cause =====");
  }
  return ok;
}

// ---- Public entry points ---------------------------------------------------

// Set at boot so the first diagnosticsLoop() call runs the check. Atomic because
// diagnosticsRequest() is called from the web server task, the loop from the Arduino task.
static std::atomic<bool> runRequested{true};

void diagnosticsRequest() {
  runRequested.store(true);
}

void diagnosticsLoop() {
  // exchange() reads and clears the flag in one step, so a request arriving
  // while the check is running is kept for the next pass instead of lost.
  if (!runRequested.exchange(false)) {
    return;
  }
  if (!runE220Check()) {
    // NO_DEVICE = slow blink: server is up but the LoRa module is not responding.
    statusLed.setState(LedState::NO_DEVICE);
  }
}
