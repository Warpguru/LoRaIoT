#include "Diagnostics.h"

#include <Arduino.h>
#include <atomic>
// HardwareSerial.h not needed: LoRaSerial is declared extern in LoRaTransport.h

#include "../../util/src/LogBuffer.h"
#include "../../util/src/StatusLed.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../lora/src/LoRaFrame.h"
#include "../../LoRaIoT/src/LoRaConfig.h"
#include "../../lora/src/nanopb/pb_decode.h"
#include "../../lora/src/nanopb/pb_encode.h"
#include "../../lora/src/nanopb/loraiot.pb.h"

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
 * After the E220 sequence, runPingEncodeCheck() encodes a PING payload with nanopb and
 * compares it with the documented bytes. It is software only and runs even if the E220 failed.
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

// Pins, E220 commands, register layout and config baud come from LoRaTransportClass
// (single source of truth, see LoRaTransport.h). Only diagnostic-specific values live here.

static constexpr size_t REPLY_LEN = LoRaTransportClass::CONFIG_HEADER_LEN + LoRaTransportClass::REG_WRITE_COUNT;  // C1 start len + 8 values
static constexpr uint8_t IDX_ADDH = 0;                  // ADDH position within the 8 values
static constexpr uint8_t IDX_ADDL = 1;                  // ADDL position within the 8 values
static constexpr uint8_t IDX_REG0 = 2;                  // REG0 position within the 8 values
static constexpr uint8_t IDX_REG1 = 3;                  // REG1 position within the 8 values
static constexpr uint8_t IDX_CHANNEL = 4;               // REG2 position within the 8 values
static constexpr uint8_t IDX_REG3 = 5;                  // REG3 position within the 8 values
static constexpr uint8_t IDX_CRYPT_H = 6;               // CRYPT_H position within the 8 values
static constexpr uint8_t IDX_CRYPT_L = 7;               // CRYPT_L position within the 8 values
static constexpr uint8_t VERIFY_BYTES = 6;              // ADDH..REG3; CRYPT bytes are write-only (read as 0x00)
static constexpr uint8_t VAL_CRYPT_DEFAULT = 0x00;      // Default crypt byte value (write-only, read returns 0x00)
static constexpr uint8_t TEST_CHANNEL_A = 0x20;          // First channel value used for diagnostic testing
static constexpr uint8_t TEST_CHANNEL_B = 0x21;          // Alternate channel value used for diagnostic testing
static constexpr uint32_t REPLY_TIMEOUT_MS = 500;
static constexpr size_t HEX_BUFFER_SIZE = LoRaTransportClass::HEX_ENTRY_SIZE * REPLY_LEN;  // Log buffer for one full reply
static constexpr uint32_t POLL_INTERVAL_MS = 1;         // Delay between AUX polls
static constexpr uint32_t MODE_REACT_DELAY_MS = 10;     // Let AUX react to a mode change before polling it
static constexpr uint32_t AUX_STABLE_DELAY_MS = 2;      // AUX must stay HIGH >= 2 ms before UART data is accepted
static constexpr uint32_t WRITE_SETTLE_DELAY_MS = 10;   // Module settle time after a register write (AUX does not signal it)
static constexpr uint32_t MODE0_IDLE_DELAY_MS = 100;    // Wait in Mode 0 before sampling the AUX idle level
static constexpr uint32_t TASK_PAUSE_DELAY_MS = 50;     // Let the suspended transceiver task release UART2

// ---- Protobuf PING self-test constants (doc/LoRaProtocol.md section 4.2) ----

static constexpr uint32_t PING_TEST_BOOT_ID = 0x12345678;
// Expected encoding of Beacon{boot_id = 0x12345678}: tag 0x08 (field 1, varint) + varint of the value.
static constexpr uint8_t PING_EXPECTED_PAYLOAD[] = {0x08, 0xF8, 0xAC, 0xD1, 0x91, 0x01};

// ---- Frame codec self-test constants (doc/LoRaProtocol.md section 4.2) ------

// Worked-example PING frame (src=0x01, dst=0x02, seq=1, payload=PING_EXPECTED_PAYLOAD).
// All header bytes use named constants; no magic numbers.
static constexpr uint8_t  FRAME_TEST_SRC  = LORA_DEFAULT_ADDR_LOCAL;   // 0x01
static constexpr uint8_t  FRAME_TEST_DST  = LORA_DEFAULT_ADDR_REMOTE;  // 0x02
static constexpr uint16_t FRAME_TEST_SEQ  = 0x0001;
// CRC of the header+payload scope (VER..PAYLOAD) for this worked example.
// Documented result: 0x720D. Named here so any future change is visible as a constant update.
static constexpr uint16_t FRAME_TEST_CRC  = 0x720D;
static constexpr uint8_t FRAME_EXPECTED[] = {
    LORA_SYNC_0,                                              // 0xAA
    LORA_SYNC_1,                                              // 0x55
    LORA_PROTOCOL_VER,                                        // 0x01
    static_cast<uint8_t>(LoRaFrameType::PING),                // 0x01
    FRAME_TEST_SRC,                                           // 0x01
    FRAME_TEST_DST,                                           // 0x02
    static_cast<uint8_t>(FRAME_TEST_SEQ >> 8),                // 0x00  SEQ hi
    static_cast<uint8_t>(FRAME_TEST_SEQ & 0xFF),              // 0x01  SEQ lo
    static_cast<uint8_t>(sizeof(PING_EXPECTED_PAYLOAD)),      // 0x06  LEN
    // payload: Beacon{boot_id=PING_TEST_BOOT_ID}
    0x08, 0xF8, 0xAC, 0xD1, 0x91, 0x01,
    // CRC16 big-endian
    static_cast<uint8_t>(FRAME_TEST_CRC >> 8),                // 0x72
    static_cast<uint8_t>(FRAME_TEST_CRC & 0xFF),              // 0x0D
};
static constexpr size_t FRAME_EXPECTED_LEN = sizeof(FRAME_EXPECTED);   // 17

// UART2 object private to this file. It is closed (end()) before returning so
// the future LoRa transport task can claim UART2 without conflict.
// Use the single shared UART2 instance from LoRaTransport.cpp.
// Diagnostics borrows it between pause() and resume(); never constructs its own.
#define loraSerial LoRaSerial

// ---- Helpers ---------------------------------------------------------------

/** Formats bytes as "AA BB CC" into a small stack buffer for logging. */
static void toHex(const uint8_t* data, size_t len, char* out, size_t outSize) {
  size_t pos = 0;
  out[0] = '\0';
  for (size_t i = 0; i < len && pos + LoRaTransportClass::HEX_ENTRY_SIZE < outSize; i++) {
    pos += snprintf(out + pos, outSize - pos, "%02X ", data[i]);
  }
}

/** Waits until AUX reaches the given level. Returns false on timeout. */
static bool waitAux(bool level, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (digitalRead(LoRaTransportClass::PIN_AUX) != level) {
    if (millis() - start > timeoutMs) {
      return false;
    }
    delay(POLL_INTERVAL_MS);
  }
  return true;
}

/**
 * Sets M1/M0 and waits for the module to signal ready (AUX rising edge).
 * The extra 2 ms after the edge is required before any UART data is sent.
 */
static bool setMode(bool m1, bool m0) {
  digitalWrite(LoRaTransportClass::PIN_M1, m1);
  digitalWrite(LoRaTransportClass::PIN_M0, m0);
  delay(MODE_REACT_DELAY_MS);  // let AUX react to the new mode before polling it
  const bool ok = waitAux(HIGH, LoRaTransportClass::AUX_TIMEOUT_MS);
  delay(AUX_STABLE_DELAY_MS);
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
  const uint8_t cmd[] = {LoRaTransportClass::CMD_READ, LoRaTransportClass::REG_START_ADDR, LoRaTransportClass::REG_WRITE_COUNT};
  uint8_t reply[REPLY_LEN];
  char hex[HEX_BUFFER_SIZE];

  const size_t n = transact(cmd, sizeof(cmd), reply, sizeof(reply));
  toHex(reply, n, hex, sizeof(hex));
  Log_info("E220 diag:   read reply (%u bytes): %s", static_cast<unsigned>(n), hex);

  if (n == 0) {
    Log_error("E220 diag:   FAIL no reply. Check ESP32 TX(GPIO17)->E220 RXD, E220 TXD->ESP32 RX(GPIO16), M0/M1.");
    return false;
  }
  if (n == LoRaTransportClass::ERROR_REPLY_LEN && reply[0] == LoRaTransportClass::ERROR_REPLY_BYTE) {
    Log_error("E220 diag:   FAIL FF FF FF. Module is in config mode but rejected the command.");
    return false;
  }
  if (n != REPLY_LEN || reply[0] != LoRaTransportClass::CMD_READ || reply[1] != LoRaTransportClass::REG_START_ADDR ||
      reply[2] != LoRaTransportClass::REG_WRITE_COUNT) {
    Log_error("E220 diag:   FAIL unexpected reply format (expected C1 00 08 + 8 bytes).");
    return false;
  }
  memcpy(regs, reply + LoRaTransportClass::CONFIG_HEADER_LEN, LoRaTransportClass::REG_WRITE_COUNT);
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
  delay(WRITE_SETTLE_DELAY_MS);  // fixed settle time; AUX does not signal it (see above)
  const uint32_t start = millis();
  const bool wasLow = (digitalRead(LoRaTransportClass::PIN_AUX) == LOW);
  const bool ok = waitAux(HIGH, LoRaTransportClass::AUX_TIMEOUT_MS);
  delay(AUX_STABLE_DELAY_MS);  // AUX must stay HIGH >= 2 ms before UART data is accepted
  Log_info("E220 diag:   after write: AUX was %s, settled in %lu ms%s", wasLow ? "LOW" : "HIGH",
           static_cast<unsigned long>(millis() - start), ok ? "" : " (TIMEOUT)");
}

/** Temporarily writes regs[], checks the module's echo, reads back and compares. */
static bool writeTempAndVerify(const uint8_t* regs) {
  uint8_t cmd[LoRaTransportClass::CONFIG_HEADER_LEN + LoRaTransportClass::REG_WRITE_COUNT] = {
      LoRaTransportClass::CMD_WRITE_TEMP, LoRaTransportClass::REG_START_ADDR, LoRaTransportClass::REG_WRITE_COUNT};
  memcpy(cmd + LoRaTransportClass::CONFIG_HEADER_LEN, regs, LoRaTransportClass::REG_WRITE_COUNT);

  uint8_t reply[REPLY_LEN];
  char hex[HEX_BUFFER_SIZE];
  const size_t n = transact(cmd, sizeof(cmd), reply, sizeof(reply));
  toHex(reply, n, hex, sizeof(hex));
  Log_info("E220 diag:   write echo (%u bytes): %s", static_cast<unsigned>(n), hex);

  if (n != REPLY_LEN || reply[0] != LoRaTransportClass::CMD_READ ||
      memcmp(reply + LoRaTransportClass::CONFIG_HEADER_LEN, regs, LoRaTransportClass::REG_WRITE_COUNT) != 0) {
    Log_error("E220 diag:   FAIL write echo does not match what was written.");
    return false;
  }

  waitForWriteToSettle();

  uint8_t readBack[LoRaTransportClass::REG_WRITE_COUNT];
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
  // Suspend the background transceiver task and close its UART handle
  LoRaTransport.pause();
  delay(TASK_PAUSE_DELAY_MS);

  Log_info("E220 diag: ===== start (M0=GPIO%u M1=GPIO%u AUX=GPIO%u RX=GPIO%u TX=GPIO%u) =====",
           LoRaTransportClass::PIN_M0, LoRaTransportClass::PIN_M1, LoRaTransportClass::PIN_AUX,
           LoRaTransportClass::PIN_RX, LoRaTransportClass::PIN_TX);

  pinMode(LoRaTransportClass::PIN_M0, OUTPUT);
  pinMode(LoRaTransportClass::PIN_M1, OUTPUT);
  pinMode(LoRaTransportClass::PIN_AUX, INPUT);
  loraSerial.begin(LoRaTransportClass::CONFIG_BAUD, SERIAL_8N1, LoRaTransportClass::PIN_RX, LoRaTransportClass::PIN_TX);

  bool ok = false;

  do {
    // Step 1: Mode 0 idle level of AUX.
    Log_info("E220 diag: [1/6] Mode 0, checking AUX idle level");
    digitalWrite(LoRaTransportClass::PIN_M0, LOW);
    digitalWrite(LoRaTransportClass::PIN_M1, LOW);
    delay(MODE0_IDLE_DELAY_MS);
    if (digitalRead(LoRaTransportClass::PIN_AUX) != HIGH) {
      Log_error("E220 diag:   FAIL AUX is LOW. AUX wire open, wrong pin, or module has no power.");
      break;
    }
    Log_info("E220 diag:   PASS AUX is HIGH");

    // Step 2: enter config mode.
    Log_info("E220 diag: [2/6] Entering Mode 3 (M1=1, M0=1) at %u baud", static_cast<unsigned>(LoRaTransportClass::CONFIG_BAUD));
    if (!setMode(HIGH, HIGH)) {
      Log_error("E220 diag:   FAIL AUX did not return HIGH after the mode change. Check M0/M1/AUX wiring.");
      break;
    }
    Log_info("E220 diag:   PASS AUX went HIGH");

    // Step 3: read registers. This single reply validates UART TX, UART RX, M0 and M1.
    Log_info("E220 diag: [3/6] Reading registers");
    uint8_t original[LoRaTransportClass::REG_WRITE_COUNT];
    if (!readRegisters(original)) {
      break;
    }
    Log_info("E220 diag:   PASS ADDH=%02X ADDL=%02X REG0=%02X REG1=%02X REG2(ch)=%02X REG3=%02X",
             original[IDX_ADDH], original[IDX_ADDL], original[IDX_REG0], original[IDX_REG1], original[IDX_CHANNEL],
             original[IDX_REG3]);
    Log_info("E220 diag:   factory default (manual) is 00 00 62 00 17; REG3 is not specified there");

    // Step 4: temporary write of a different channel.
    const uint8_t testChannel = (original[IDX_CHANNEL] == TEST_CHANNEL_A) ? TEST_CHANNEL_B : TEST_CHANNEL_A;
    Log_info("E220 diag: [4/6] Temporary write: channel %02X -> %02X", original[IDX_CHANNEL], testChannel);
    uint8_t changed[LoRaTransportClass::REG_WRITE_COUNT];
    memcpy(changed, original, LoRaTransportClass::REG_WRITE_COUNT);
    changed[IDX_CHANNEL] = testChannel;
    changed[IDX_CRYPT_H] = VAL_CRYPT_DEFAULT;  // CRYPT_H/L always read back as 0x00; write 0 to keep the echo comparable
    changed[IDX_CRYPT_L] = VAL_CRYPT_DEFAULT;
    if (!writeTempAndVerify(changed)) {
      break;
    }
    Log_info("E220 diag:   PASS write and read-back match");

    // Step 5: put the original values back.
    Log_info("E220 diag: [5/6] Temporary write: restoring original values");
    original[IDX_CRYPT_H] = VAL_CRYPT_DEFAULT;
    original[IDX_CRYPT_L] = VAL_CRYPT_DEFAULT;
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

  // Resume the background transceiver task and reopen its UART handle
  LoRaTransport.resume();

  return ok;
}

// ---- Protobuf PING self-test -----------------------------------------------

/**
 * Encodes a PING payload (Beacon with only boot_id) with nanopb, compares it with the
 * bytes documented in doc/LoRaProtocol.md section 4.2, then decodes it again.
 * Pure software: needs no UART and no radio. Returns true only if every check passed.
 */
static bool runPingEncodeCheck() {
  Log_info("Protobuf diag: ===== PING encode/decode check =====");

  loraiot_Beacon ping = loraiot_Beacon_init_zero;
  ping.boot_id = PING_TEST_BOOT_ID;  // fw_version, uptime_s and registry_rev stay absent in a PING

  uint8_t payload[loraiot_Beacon_size];
  pb_ostream_t out = pb_ostream_from_buffer(payload, sizeof(payload));
  if (!pb_encode(&out, loraiot_Beacon_fields, &ping)) {
    Log_error("Protobuf diag:   FAIL encode: %s", PB_GET_ERROR(&out));
    return false;
  }

  char hex[LoRaTransportClass::HEX_ENTRY_SIZE * sizeof(payload)];
  toHex(payload, out.bytes_written, hex, sizeof(hex));
  Log_info("Protobuf diag:   encoded PING payload (%uB): %s", static_cast<unsigned>(out.bytes_written), hex);

  if (out.bytes_written != sizeof(PING_EXPECTED_PAYLOAD) ||
      memcmp(payload, PING_EXPECTED_PAYLOAD, sizeof(PING_EXPECTED_PAYLOAD)) != 0) {
    toHex(PING_EXPECTED_PAYLOAD, sizeof(PING_EXPECTED_PAYLOAD), hex, sizeof(hex));
    Log_error("Protobuf diag:   FAIL payload differs from the documented bytes: %s", hex);
    return false;
  }
  Log_info("Protobuf diag:   PASS payload equals the documented bytes");

  loraiot_Beacon decoded = loraiot_Beacon_init_zero;
  pb_istream_t in = pb_istream_from_buffer(payload, out.bytes_written);
  if (!pb_decode(&in, loraiot_Beacon_fields, &decoded)) {
    Log_error("Protobuf diag:   FAIL decode: %s", PB_GET_ERROR(&in));
    return false;
  }
  if (decoded.boot_id != PING_TEST_BOOT_ID) {
    Log_error("Protobuf diag:   FAIL decoded boot_id %08X, expected %08X",
              static_cast<unsigned>(decoded.boot_id), static_cast<unsigned>(PING_TEST_BOOT_ID));
    return false;
  }
  if (decoded.has_fw_version || decoded.has_uptime_s || decoded.has_registry_rev) {
    Log_error("Protobuf diag:   FAIL optional fields present in a PING (has_fw_version=%d has_uptime_s=%d has_registry_rev=%d)",
              decoded.has_fw_version, decoded.has_uptime_s, decoded.has_registry_rev);
    return false;
  }
  Log_info("Protobuf diag:   PASS decode returns boot_id %08X, optional fields absent",
           static_cast<unsigned>(decoded.boot_id));
  Log_info("Protobuf diag: ===== RESULT: PASS. nanopb encode and decode verified =====");
  return true;
}

// ---- Frame codec self-test -------------------------------------------------

/**
 * Verifies computeCRC16, encodeFrame, decodeFrame and the CRC-error path using the worked
 * example from doc/LoRaProtocol.md section 4.2. Pure software: no UART, no radio.
 * Returns true only if every check passed.
 */
static bool runFrameCodecCheck() {
  Log_info("Frame diag: ===== frame codec check =====");

  // Check 1: CRC check value for the ASCII string "123456789".
  {
    const uint8_t check[] = {'1','2','3','4','5','6','7','8','9'};
    const uint16_t crc = computeCRC16(check, sizeof(check));
    if (crc != LORA_CRC_CHECK_VALUE) {
      Log_error("Frame diag:   FAIL CRC check value: got 0x%04X, expected 0x%04X", crc, LORA_CRC_CHECK_VALUE);
      return false;
    }
    Log_info("Frame diag:   PASS CRC of \"123456789\" = 0x%04X", crc);
  }

  // Check 2: encodeFrame produces the 17-byte worked example exactly.
  {
    LoRaFrame frame;
    frame.ver  = LORA_PROTOCOL_VER;
    frame.type = LoRaFrameType::PING;
    frame.src  = FRAME_TEST_SRC;
    frame.dst  = FRAME_TEST_DST;
    frame.seq  = FRAME_TEST_SEQ;
    frame.len  = static_cast<uint8_t>(sizeof(PING_EXPECTED_PAYLOAD));
    memcpy(frame.payload, PING_EXPECTED_PAYLOAD, frame.len);

    uint8_t encoded[LORA_MAX_ENCODED_LEN];
    size_t  encodedLen = 0;
    if (!encodeFrame(frame, encoded, encodedLen)) {
      Log_error("Frame diag:   FAIL encodeFrame returned false");
      return false;
    }
    if (encodedLen != FRAME_EXPECTED_LEN || memcmp(encoded, FRAME_EXPECTED, FRAME_EXPECTED_LEN) != 0) {
      char got[LoRaTransportClass::HEX_ENTRY_SIZE * FRAME_EXPECTED_LEN + 1];
      char exp[LoRaTransportClass::HEX_ENTRY_SIZE * FRAME_EXPECTED_LEN + 1];
      toHex(encoded, encodedLen, got, sizeof(got));
      toHex(FRAME_EXPECTED, FRAME_EXPECTED_LEN, exp, sizeof(exp));
      Log_error("Frame diag:   FAIL encoded bytes differ");
      Log_error("Frame diag:     got: %s", got);
      Log_error("Frame diag:     exp: %s", exp);
      return false;
    }
    Log_info("Frame diag:   PASS worked example encodes to %u documented bytes", (unsigned)encodedLen);

    // Check 3: decodeFrame round-trip (ownAddr = FRAME_TEST_DST so the DST filter passes).
    LoRaFrame decoded;
    const LoRaDecodeResult result = decodeFrame(encoded, encodedLen, decoded, FRAME_TEST_DST);
    if (result != LoRaDecodeResult::OK) {
      Log_error("Frame diag:   FAIL decodeFrame returned %u (expected OK=0)", (unsigned)result);
      return false;
    }
    if (decoded.type != LoRaFrameType::PING ||
        decoded.src  != FRAME_TEST_SRC      ||
        decoded.dst  != FRAME_TEST_DST      ||
        decoded.seq  != FRAME_TEST_SEQ      ||
        decoded.len  != sizeof(PING_EXPECTED_PAYLOAD) ||
        memcmp(decoded.payload, PING_EXPECTED_PAYLOAD, decoded.len) != 0) {
      Log_error("Frame diag:   FAIL decoded fields do not match the original frame");
      return false;
    }
    Log_info("Frame diag:   PASS decode round-trip: type=%u src=0x%02X dst=0x%02X seq=%u len=%u",
             (unsigned)decoded.type, decoded.src, decoded.dst, decoded.seq, decoded.len);

    // Check 4: a single flipped payload bit must be caught and reported as BAD_CRC.
    uint8_t corrupted[FRAME_EXPECTED_LEN];
    memcpy(corrupted, FRAME_EXPECTED, FRAME_EXPECTED_LEN);
    corrupted[LORA_OFFSET_PAYLOAD] ^= 0x01;   // flip LSB of the first payload byte
    LoRaFrame dummy;
    const LoRaDecodeResult badResult = decodeFrame(corrupted, sizeof(corrupted), dummy, FRAME_TEST_DST);
    if (badResult != LoRaDecodeResult::BAD_CRC) {
      Log_error("Frame diag:   FAIL corrupted frame returned %u, expected BAD_CRC (%u)",
                (unsigned)badResult, (unsigned)LoRaDecodeResult::BAD_CRC);
      return false;
    }
    Log_info("Frame diag:   PASS corrupted frame correctly rejected as BAD_CRC");
  }

  Log_info("Frame diag: ===== RESULT: PASS. CRC, encode, decode and error detection verified =====");
  return true;
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
  // Software-only checks: independent of the radio, run even if the E220 check failed.
  if (!runPingEncodeCheck()) {
    Log_error("Protobuf diag: ===== RESULT: FAIL. See the FAIL line above =====");
  }
  if (!runFrameCodecCheck()) {
    Log_error("Frame diag: ===== RESULT: FAIL. See the FAIL line above =====");
  }
}
