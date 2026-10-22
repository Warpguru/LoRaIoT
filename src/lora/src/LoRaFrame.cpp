#include "LoRaFrame.h"

#include <atomic>
#include <esp_system.h>

#include "../../util/src/LogBuffer.h"

/**
 * LoRaFrame.cpp - LoRaIoT frame codec (doc/LoRaProtocol.md section 4).
 *
 * Responsibilities:
 *   - computeCRC16()  CRC-16/CCITT-FALSE over VER..PAYLOAD
 *   - encodeFrame()   LoRaFrame struct -> wire bytes (SYNC..CRC16)
 *   - decodeFrame()   wire bytes -> LoRaFrame struct, with all receiver-rule checks
 *   - loraNextSeq()   node-wide frame SEQ counter
 *
 * Not responsibilities (kept elsewhere on purpose):
 *   - The 3-byte E220 routing header [ADDH][ADDL][CH]: added by LoRaTransport::sendRawInternal().
 *   - Finding the frame boundaries in the UART byte stream: done by the framer in loraTransportTask,
 *     which hands exactly one candidate frame (SYNC..CRC16) to decodeFrame().
 *   - Duplicate detection, boot_id handling, ACK/retry logic: later M4 work (LoRaState).
 *
 * This file has no dependency on the application layer. The own node address is a parameter of
 * decodeFrame() so the codec can be unit tested without LoRaConfig or NVS.
 */

// ---- Local constants -------------------------------------------------------

static constexpr uint8_t BITS_PER_BYTE = 8;      // Shift distance for big-endian 16-bit fields
static constexpr uint8_t BYTE_MASK     = 0xFF;   // Selects the low byte of a 16-bit value
static constexpr uint8_t LORA_ADDR_ALL = 0xFF;   // Broadcast DST. Same value as LORA_ADDR_BROADCAST in
                                                 // LoRaConfig.h; duplicated here to keep the codec free of
                                                 // application-layer includes.

// ---- CRC -------------------------------------------------------------------

/**
 * Bitwise CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, MSB first (no reflection), xorout 0.
 *
 * A table-driven version would be faster, but frames are at most 197 B and the LoRa air time
 * (tens of ms) dominates by orders of magnitude, so the table-free bitwise form is preferred
 * (no 512 B lookup table in flash, and it matches the reference in doc/LoRaProtocol.md 4.1).
 * Check value: computeCRC16("123456789", 9) == 0x29B1 (verified at boot by Diagnostics).
 */
uint16_t computeCRC16(const uint8_t* data, size_t len) {
  uint16_t crc = LORA_CRC_INIT;
  for (size_t i = 0; i < len; i++) {
    // Feed the next byte into the high half of the register, then shift out 8 bits.
    crc ^= static_cast<uint16_t>(data[i]) << BITS_PER_BYTE;
    for (uint8_t bit = 0; bit < BITS_PER_BYTE; bit++) {
      crc = (crc & LORA_CRC_MSB) ? static_cast<uint16_t>((crc << 1) ^ LORA_CRC_POLY)
                                 : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

// ---- Encode ----------------------------------------------------------------

/**
 * Writes SYNC, header, payload and CRC16 into outBuf in one pass so the caller can hand the
 * result to the E220 as a single atomic UART write (the E220 force-flushes after 2 s of UART
 * inactivity, so a frame must never be written in pieces).
 *
 * The CRC scope starts at VER (LORA_OFFSET_VER) and ends with the last payload byte. The SYNC
 * bytes are excluded so a receiver that resynchronises on 0xAA 0x55 does not depend on them.
 */
bool encodeFrame(const LoRaFrame& frame, uint8_t* outBuf, size_t& outLen) {
  // Reject instead of truncating: a silently shortened payload would still carry a valid CRC.
  if (outBuf == nullptr || frame.len > LORA_MAX_PAYLOAD_LEN) {
    return false;
  }

  outBuf[LORA_OFFSET_SYNC_0] = LORA_SYNC_0;
  outBuf[LORA_OFFSET_SYNC_1] = LORA_SYNC_1;
  outBuf[LORA_OFFSET_VER]    = frame.ver;
  outBuf[LORA_OFFSET_TYPE]   = static_cast<uint8_t>(frame.type);
  outBuf[LORA_OFFSET_SRC]    = frame.src;
  outBuf[LORA_OFFSET_DST]    = frame.dst;
  outBuf[LORA_OFFSET_SEQ_HI] = static_cast<uint8_t>(frame.seq >> BITS_PER_BYTE);   // big-endian
  outBuf[LORA_OFFSET_SEQ_LO] = static_cast<uint8_t>(frame.seq & BYTE_MASK);
  outBuf[LORA_OFFSET_LEN]    = frame.len;
  memcpy(outBuf + LORA_OFFSET_PAYLOAD, frame.payload, frame.len);

  // The CRC directly follows the payload, so its offset depends on the payload length.
  const size_t   crcOffset = LORA_OFFSET_PAYLOAD + frame.len;
  const uint16_t crc       = computeCRC16(outBuf + LORA_OFFSET_VER, crcOffset - LORA_OFFSET_VER);
  outBuf[crcOffset]     = static_cast<uint8_t>(crc >> BITS_PER_BYTE);                // big-endian
  outBuf[crcOffset + 1] = static_cast<uint8_t>(crc & BYTE_MASK);

  outLen = crcOffset + LORA_FRAME_CRC_LEN;
  return true;
}

// ---- Decode ----------------------------------------------------------------

/**
 * Validates one candidate frame. The checks run cheapest first and in a fixed order, and the
 * order is part of the contract because callers act on the result:
 *
 *   1. length / SYNC / LEN consistency   (is this a frame at all?)
 *   2. CRC                               (is it intact? only this failure counts as a CRC error)
 *   3. VER, TYPE                         (do we understand it?)
 *   4. DST                               (is it for us?)
 *
 * The CRC is checked before VER/TYPE/DST on purpose: those fields are only trustworthy once the
 * CRC passed, and a corrupted frame must show up as BAD_CRC in the statistics, not as a foreign
 * DST or unknown TYPE.
 *
 * Per doc/LoRaProtocol.md section 4.3 a bad frame is dropped silently: this function only logs
 * at DEBUG and the caller never answers with an error frame (avoids reply storms).
 *
 * `out` is written only when OK is returned, so the caller can reuse a static LoRaFrame without
 * seeing half-filled data after a failure.
 */
LoRaDecodeResult decodeFrame(const uint8_t* inBuf, size_t len, LoRaFrame& out, uint8_t ownAddr) {
  // 1a. Minimum size: header plus CRC, an empty payload is legal.
  if (inBuf == nullptr || len < LORA_FRAME_HEADER_LEN + LORA_FRAME_CRC_LEN) {
    Log_debug("decodeFrame: too short (%u B)", static_cast<unsigned>(len));
    return LoRaDecodeResult::BAD_LEN;
  }
  // 1b. SYNC. The framer already scans for it; this is a second line of defence for other callers.
  if (inBuf[LORA_OFFSET_SYNC_0] != LORA_SYNC_0 || inBuf[LORA_OFFSET_SYNC_1] != LORA_SYNC_1) {
    Log_debug("decodeFrame: bad SYNC %02X %02X", inBuf[LORA_OFFSET_SYNC_0], inBuf[LORA_OFFSET_SYNC_1]);
    return LoRaDecodeResult::BAD_SYNC;
  }

  // 1c. LEN must be in range and must describe exactly the bytes we were given. Reading the CRC
  // position from an unchecked LEN would otherwise read outside the buffer.
  const uint8_t payloadLen = inBuf[LORA_OFFSET_LEN];
  if (payloadLen > LORA_MAX_PAYLOAD_LEN ||
      len != LORA_FRAME_HEADER_LEN + static_cast<size_t>(payloadLen) + LORA_FRAME_CRC_LEN) {
    Log_debug("decodeFrame: LEN %u does not match buffer of %u B", payloadLen, static_cast<unsigned>(len));
    return LoRaDecodeResult::BAD_LEN;
  }

  // 2. CRC over VER..PAYLOAD, compared with the big-endian value at the end of the frame.
  const size_t   crcOffset = LORA_OFFSET_PAYLOAD + payloadLen;
  const uint16_t rxCrc     = static_cast<uint16_t>((inBuf[crcOffset] << BITS_PER_BYTE) | inBuf[crcOffset + 1]);
  const uint16_t calcCrc   = computeCRC16(inBuf + LORA_OFFSET_VER, crcOffset - LORA_OFFSET_VER);
  if (rxCrc != calcCrc) {
    Log_debug("decodeFrame: CRC mismatch rx=%04X calc=%04X", rxCrc, calcCrc);
    return LoRaDecodeResult::BAD_CRC;
  }

  // 3. Protocol version and frame type (codes 0x08.. are reserved, doc/LoRaProtocol.md section 5).
  if (inBuf[LORA_OFFSET_VER] != LORA_PROTOCOL_VER) {
    Log_debug("decodeFrame: unknown VER 0x%02X", inBuf[LORA_OFFSET_VER]);
    return LoRaDecodeResult::BAD_VER;
  }
  const uint8_t type = inBuf[LORA_OFFSET_TYPE];
  if (type < LORA_TYPE_MIN || type > LORA_TYPE_MAX) {
    Log_debug("decodeFrame: unknown TYPE 0x%02X", type);
    return LoRaDecodeResult::BAD_TYPE;
  }

  // 4. Address filter. The E220 already filters by its module address, but only in software can
  // we also see broadcast frames meant for other roles, so DST is checked here as well.
  const uint8_t dst = inBuf[LORA_OFFSET_DST];
  if (dst != ownAddr && dst != LORA_ADDR_ALL) {
    Log_debug("decodeFrame: foreign DST 0x%02X (own 0x%02X)", dst, ownAddr);
    return LoRaDecodeResult::FOREIGN_DST;
  }

  // All checks passed: copy into the caller's struct (payload is copied, inBuf may be reused).
  out.ver  = inBuf[LORA_OFFSET_VER];
  out.type = static_cast<LoRaFrameType>(type);
  out.src  = inBuf[LORA_OFFSET_SRC];
  out.dst  = dst;
  out.seq  = static_cast<uint16_t>((inBuf[LORA_OFFSET_SEQ_HI] << BITS_PER_BYTE) | inBuf[LORA_OFFSET_SEQ_LO]);
  out.len  = payloadLen;
  memcpy(out.payload, inBuf + LORA_OFFSET_PAYLOAD, payloadLen);
  return LoRaDecodeResult::OK;
}

// ---- SEQ counter -----------------------------------------------------------

/**
 * One counter for all frame types of this node (doc/LoRaProtocol.md section 6): PING, ACK, DATA
 * and so on share it, so a receiver can estimate lost frames from SEQ gaps per SRC.
 *
 * The start value is random per boot (esp_random(), a true RNG once WiFi is active). A rebooted
 * node therefore does not restart at 0, which would make its first frames look like duplicates of
 * the previous run. The std::atomic makes the counter safe to use from the Arduino task, the
 * ESPAsyncWebServer callbacks (Core 0) and loraTransportTask without an extra mutex.
 * Retransmissions must reuse their SEQ: call this only for NEW frames.
 */
uint16_t loraNextSeq() {
  // Function-local static: constructed once on first use (thread-safe in C++11), so no explicit
  // init call is needed during applicationSetup().
  static std::atomic<uint16_t> counter{static_cast<uint16_t>(esp_random())};
  return counter.fetch_add(1);   // uint16_t arithmetic wraps 65535 -> 0 as the protocol requires
}
