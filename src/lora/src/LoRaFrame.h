#pragma once

#include <Arduino.h>

/**
 * LoRaFrame.h - LoRaIoT frame format, CRC and codec (doc/LoRaProtocol.md section 4).
 *
 *   +------+-----+------+-----+-----+--------+-----+---------+--------+
 *   | SYNC | VER | TYPE | SRC | DST |  SEQ   | LEN | PAYLOAD | CRC16  |
 *   | 2 B  | 1 B | 1 B  | 1 B | 1 B |  2 B   | 1 B | 0-186 B |  2 B   |
 *   +------+-----+------+-----+-----+--------+-----+---------+--------+
 *
 * SEQ and CRC16 are big-endian. The CRC covers VER..PAYLOAD (not SYNC).
 * The 3-byte E220 routing header is NOT part of the frame; LoRaTransport prepends it.
 *
 * This file is pure software: it needs no UART and no knowledge of the application.
 * The own node address is therefore passed to decodeFrame() as a parameter.
 */

// ---- Size constants --------------------------------------------------------

// Max E220 sub-packet / UART write size (REG1_SUB_PACKET_200B)
static constexpr size_t LORA_MAX_FRAME_LEN    = 200;

// Fixed-transmission routing prefix prepended by the MCU before every frame:
// [ADDH 1B][ADDL 1B][CH 1B] (doc/LoRaProtocol.md section 2)
static constexpr size_t LORA_ROUTING_HDR_LEN  = 3;

// LoRaIoT frame header: [0xAA 0x55][VER 1B][TYPE 1B][SRC 1B][DST 1B][SEQ 2B][LEN 1B]
// CRC16 [2B] trails the payload and is also counted against the sub-packet budget.
static constexpr size_t LORA_FRAME_SYNC_LEN   = 2;
static constexpr size_t LORA_FRAME_HEADER_LEN = 9;   // SYNC(2)+VER+TYPE+SRC+DST+SEQ(2)+LEN
static constexpr size_t LORA_FRAME_CRC_LEN    = 2;
static constexpr size_t LORA_FRAME_OVERHEAD   = LORA_ROUTING_HDR_LEN + LORA_FRAME_HEADER_LEN + LORA_FRAME_CRC_LEN;

// Maximum protobuf payload bytes that fit inside one LoRa frame.
// 200 - 3 (routing) - 9 (header) - 2 (CRC) = 186 B  (doc/LoRaProtocol.md section 4)
static constexpr size_t LORA_MAX_PAYLOAD_LEN  = LORA_MAX_FRAME_LEN - LORA_FRAME_OVERHEAD;

// Largest encoded frame (SYNC..CRC16) without the routing header: 11 + 186 = 197 B
static constexpr size_t LORA_MAX_ENCODED_LEN  = LORA_FRAME_HEADER_LEN + LORA_MAX_PAYLOAD_LEN + LORA_FRAME_CRC_LEN;

// ---- Header field values and byte offsets ----------------------------------

static constexpr uint8_t LORA_SYNC_0          = 0xAA;
static constexpr uint8_t LORA_SYNC_1          = 0x55;
static constexpr uint8_t LORA_PROTOCOL_VER    = 0x01;

static constexpr size_t  LORA_OFFSET_SYNC_0   = 0;
static constexpr size_t  LORA_OFFSET_SYNC_1   = 1;
static constexpr size_t  LORA_OFFSET_VER      = 2;   // CRC scope starts here
static constexpr size_t  LORA_OFFSET_TYPE     = 3;
static constexpr size_t  LORA_OFFSET_SRC      = 4;
static constexpr size_t  LORA_OFFSET_DST      = 5;
static constexpr size_t  LORA_OFFSET_SEQ_HI   = 6;
static constexpr size_t  LORA_OFFSET_SEQ_LO   = 7;
static constexpr size_t  LORA_OFFSET_LEN      = 8;
static constexpr size_t  LORA_OFFSET_PAYLOAD  = 9;

// ---- CRC-16/CCITT-FALSE (doc/LoRaProtocol.md section 4.1) ------------------

static constexpr uint16_t LORA_CRC_POLY       = 0x1021;
static constexpr uint16_t LORA_CRC_INIT       = 0xFFFF;
static constexpr uint16_t LORA_CRC_MSB        = 0x8000;
static constexpr uint16_t LORA_CRC_CHECK_VALUE = 0x29B1;   // CRC of the ASCII string "123456789"

// ---- Types -----------------------------------------------------------------

/** Frame TYPE byte (doc/LoRaProtocol.md section 5). 0x08..0xFF are reserved or experimental. */
enum class LoRaFrameType : uint8_t {
  PING     = 0x01,
  BEACON   = 0x02,
  ACK      = 0x03,
  DATA     = 0x04,
  DATA_ACK = 0x05,
  REQUEST  = 0x06,
  RESPONSE = 0x07,
};

static constexpr uint8_t LORA_TYPE_MIN = static_cast<uint8_t>(LoRaFrameType::PING);
static constexpr uint8_t LORA_TYPE_MAX = static_cast<uint8_t>(LoRaFrameType::RESPONSE);

/** One decoded (or to be encoded) frame. */
struct LoRaFrame {
  uint8_t       ver;                              ///< Protocol version byte (LORA_PROTOCOL_VER = 0x01).
  LoRaFrameType type;                             ///< Frame type (PING, BEACON, ACK, DATA, DATA_ACK, REQUEST, RESPONSE).
  uint8_t       src;                              ///< Source node address (0x01-0xFE).
  uint8_t       dst;                              ///< Destination node address (0x01-0xFE unicast, 0xFF broadcast).
  uint16_t      seq;                              ///< Frame sequence number (big-endian on wire, host order here).
  uint8_t       len;                              ///< Payload length in bytes (0..LORA_MAX_PAYLOAD_LEN).
  uint8_t       payload[LORA_MAX_PAYLOAD_LEN];    ///< Protobuf-encoded payload bytes.
};

/** Outcome of decodeFrame(). Only BAD_CRC is counted as a CRC error by the transport. */
enum class LoRaDecodeResult : uint8_t {
  OK          = 0,
  BAD_SYNC    = 1,   // buffer does not start with 0xAA 0x55
  BAD_LEN     = 2,   // buffer shorter than a minimal frame, or LEN does not match the buffer size
  BAD_CRC     = 3,
  BAD_VER     = 4,
  BAD_TYPE    = 5,
  FOREIGN_DST = 6,   // DST is neither the own address nor broadcast
};

// ---- Functions -------------------------------------------------------------

/**
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0).
 * computeCRC16("123456789", 9) must equal LORA_CRC_CHECK_VALUE (0x29B1).
 */
uint16_t computeCRC16(const uint8_t* data, size_t len);

/**
 * Serialises a frame into outBuf (SYNC..CRC16, without the E220 routing header).
 *
 * Parameters:
 *   frame  - Frame to encode; frame.len must be <= LORA_MAX_PAYLOAD_LEN.
 *   outBuf - Destination buffer of at least LORA_MAX_ENCODED_LEN bytes.
 *   outLen - Set to the number of bytes written on success.
 *
 * Returns false (and writes nothing) if frame.len is too large.
 */
bool encodeFrame(const LoRaFrame& frame, uint8_t* outBuf, size_t& outLen);

/**
 * Parses one complete frame. inBuf must start at the SYNC bytes and contain exactly one frame.
 * Every failure is logged at DEBUG and never answered (doc/LoRaProtocol.md section 4.3).
 *
 * Parameters:
 *   inBuf   - Received bytes, starting with 0xAA 0x55.
 *   len     - Number of bytes in inBuf.
 *   out     - Filled only when OK is returned.
 *   ownAddr - Own node address; frames with another DST (other than broadcast 0xFF) are rejected.
 */
LoRaDecodeResult decodeFrame(const uint8_t* inBuf, size_t len, LoRaFrame& out, uint8_t ownAddr);

/**
 * Returns the next frame SEQ of this node. One counter serves all frame types;
 * it starts at a random value after each boot and wraps from 65535 to 0 (doc/LoRaProtocol.md section 6).
 * Thread-safe.
 */
uint16_t loraNextSeq();
