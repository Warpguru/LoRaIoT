#include "LoRaBroker.h"

#include <esp_system.h>

#include "../../util/src/LogBuffer.h"
#include "../../LoRaIoT/src/LoRaConfig.h"
#include "../../lora/src/LoRaFrame.h"
#include "../../lora/src/nanopb/pb_encode.h"

/**
 * LoRaBroker.cpp - Request/response broker implementation.
 *
 * Single in-flight slot design: only one REQUEST/RESPONSE exchange may be
 * active at a time, matching the half-duplex LoRa channel constraint.
 *
 * The pending slot is protected by brokerMutex (guards state changes).
 * The HTTP handler blocks on responseSem, which is given by the receive
 * callback path when the matching RESPONSE (or PING ACK) arrives.
 *
 * PING frames are fully framed: Beacon{boot_id} -> pb_encode -> LoRaFrame ->
 * LoRaTransport.sendFrame(). REQUEST frames (ECHO, READ, DESCRIBE) keep the
 * Phase 2 unframed stub pending full M4 integration.
 */

// ---- Broker state -----------------------------------------------------------

static SemaphoreHandle_t brokerMutex      = nullptr;
static SemaphoreHandle_t responseSem      = nullptr;

static volatile bool     pendingActive    = false;
static volatile uint16_t pendingSeq       = 0;      // frame-level SEQ (uint16_t, from loraNextSeq)
static volatile uint32_t pendingSentMs    = 0;      // millis() when the PING was enqueued
static volatile bool     pendingIsPing    = false;

static loraiot_Response  lastResponse     = loraiot_Response_init_zero;
static PingResult        lastPingResult   = {0, 0};

// Boot identity - random non-zero value generated once at loraBrokerBegin().
// Carried in every outgoing PING Beacon payload so REMOTE can log it.
static uint32_t bootId = 0;

// ---- REQUEST frame sender ---------------------------------------------------

/**
 * Encode a loraiot_Request protobuf into a full LoRaFrame and enqueue for TX.
 * Uses encodeFrame() + LoRaTransport.sendFrame() — same path as PING.
 */
static BrokerResult encodeAndSend(const loraiot_Request& req, uint8_t dstAddr, uint16_t seq) {
  uint8_t payloadBuf[LORA_MAX_PAYLOAD_LEN];
  pb_ostream_t stream = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&stream, loraiot_Request_fields, &req)) {
    Log_error("LoRaBroker: REQUEST nanopb encode failed: %s", PB_GET_ERROR(&stream));
    return BrokerResult::ENCODE_ERR;
  }

  LoRaFrame frame;
  frame.ver  = LORA_PROTOCOL_VER;
  frame.type = LoRaFrameType::REQUEST;
  frame.src  = LoRaConfig.getAddress();
  frame.dst  = dstAddr;
  frame.seq  = seq;
  frame.len  = static_cast<uint8_t>(stream.bytes_written);
  memcpy(frame.payload, payloadBuf, frame.len);

  if (!LoRaTransport.sendFrame(frame)) {
    Log_error("LoRaBroker: REQUEST sendFrame failed seq=%u dst=0x%02X", seq, dstAddr);
    return BrokerResult::ENCODE_ERR;
  }
  Log_info("LoRaBroker: sent REQUEST seq=%u cmd=%u dst=0x%02X payload=%u bytes",
           seq, req.which_command, dstAddr, (unsigned)stream.bytes_written);
  return BrokerResult::OK;
}

// ---- Public API -------------------------------------------------------------

void loraBrokerBegin() {
  brokerMutex = xSemaphoreCreateMutex();
  responseSem = xSemaphoreCreateBinary();
  if (brokerMutex == nullptr || responseSem == nullptr) {
    Log_error("LoRaBroker::begin - failed to create synchronisation primitives");
  }

  // Generate a non-zero random boot_id (0 is the sentinel DEVICE_REGISTRY_BOOT_ID_NONE).
  do {
    bootId = esp_random();
  } while (bootId == 0);
  Log_info("LoRaBroker::begin - boot_id=0x%08X", bootId);
}

uint32_t loraBrokerBootId() {
  return bootId;
}

bool loraBrokerPendingPing(uint16_t& seq, uint32_t& sentMs) {
  if (brokerMutex == nullptr) return false;
  xSemaphoreTake(brokerMutex, portMAX_DELAY);
  const bool active = pendingActive && pendingIsPing;
  if (active) {
    seq    = pendingSeq;
    sentMs = pendingSentMs;
  }
  xSemaphoreGive(brokerMutex);
  return active;
}

BrokerResult loraBrokerSend(BrokerRequestType type, uint8_t dstAddr,
                             const void* payload, uint32_t& outSeq) {
  if (brokerMutex == nullptr) return BrokerResult::BUSY;

  // Try-acquire: if the mutex is held, another send is actively building/sending a
  // frame. Return BUSY immediately rather than blocking — callers must not queue
  // silently behind an in-flight LoRa request (ESPAsyncWebServer handler would appear
  // to hang for up to BROKER_DEFAULT_TIMEOUT_MS with no indication).
  if (xSemaphoreTake(brokerMutex, 0) != pdTRUE) {
    Log_warn("LoRaBroker::send - rejected, broker mutex held (another send in progress)");
    return BrokerResult::BUSY;
  }

  if (pendingActive) {
    xSemaphoreGive(brokerMutex);
    Log_warn("LoRaBroker::send - rejected, another request is in flight (seq=%u)", pendingSeq);
    return BrokerResult::BUSY;
  }

  // Drain any stale semaphore token from a previous cycle
  xSemaphoreTake(responseSem, 0);

  const uint16_t seq = loraNextSeq();
  pendingSeq    = seq;
  pendingActive = true;
  pendingIsPing = (type == BrokerRequestType::PING);
  outSeq        = seq;

  xSemaphoreGive(brokerMutex);

  // Build and send the frame outside the mutex (no shared state touched after this point)
  BrokerResult encResult = BrokerResult::OK;

  if (type == BrokerRequestType::PING) {
    // Encode Beacon{boot_id} as the PING payload, wrap in a proper LoRaFrame and send.
    loraiot_Beacon beacon = loraiot_Beacon_init_zero;
    beacon.boot_id = bootId;

    uint8_t payloadBuf[loraiot_Beacon_size];
    pb_ostream_t pbStream = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
    if (!pb_encode(&pbStream, loraiot_Beacon_fields, &beacon)) {
      Log_error("LoRaBroker: PING nanopb encode failed: %s", PB_GET_ERROR(&pbStream));
      encResult = BrokerResult::ENCODE_ERR;
    } else {
      LoRaFrame frame;
      frame.ver  = LORA_PROTOCOL_VER;
      frame.type = LoRaFrameType::PING;
      frame.src  = LoRaConfig.getAddress();
      frame.dst  = dstAddr;
      frame.seq  = seq;
      frame.len  = static_cast<uint8_t>(pbStream.bytes_written);
      memcpy(frame.payload, payloadBuf, frame.len);

      if (!LoRaTransport.sendFrame(frame)) {
        Log_error("LoRaBroker: PING sendFrame failed");
        encResult = BrokerResult::ENCODE_ERR;
      } else {
        // Record enqueue time for RTT measurement in the ACK handler
        xSemaphoreTake(brokerMutex, portMAX_DELAY);
        pendingSentMs = millis();
        xSemaphoreGive(brokerMutex);
        Log_info("LoRaBroker: sent PING seq=%u dst=0x%02X boot_id=0x%08X", seq, dstAddr, bootId);
      }
    }
  } else {
    loraiot_Request req = loraiot_Request_init_zero;
    switch (type) {
      case BrokerRequestType::ECHO:
        req.which_command = loraiot_Request_echo_tag;
        if (payload) req.command.echo = *static_cast<const loraiot_Echo*>(payload);
        break;
      case BrokerRequestType::READ:
        req.which_command = loraiot_Request_read_tag;
        if (payload) req.command.read = *static_cast<const loraiot_Read*>(payload);
        break;
      case BrokerRequestType::DESCRIBE:
        req.which_command = loraiot_Request_describe_tag;
        if (payload) req.command.describe = *static_cast<const loraiot_Describe*>(payload);
        break;
      default:
        break;
    }
    encResult = encodeAndSend(req, dstAddr, seq);
  }

  if (encResult != BrokerResult::OK) {
    // Encode failed - clear the pending slot so the broker is not stuck
    xSemaphoreTake(brokerMutex, portMAX_DELAY);
    pendingActive = false;
    xSemaphoreGive(brokerMutex);
  }

  return encResult;
}

BrokerResult loraBrokerWait(uint32_t timeoutMs) {
  if (responseSem == nullptr) return BrokerResult::TIMEOUT;

  TickType_t ticks = pdMS_TO_TICKS(timeoutMs);
  if (xSemaphoreTake(responseSem, ticks) == pdTRUE) {
    return BrokerResult::OK;
  }

  // Timed out - clear the pending slot
  if (brokerMutex != nullptr) {
    xSemaphoreTake(brokerMutex, portMAX_DELAY);
    pendingActive = false;
    xSemaphoreGive(brokerMutex);
  }
  Log_warn("LoRaBroker::wait - timeout after %ums (seq=%u)", timeoutMs, (unsigned)pendingSeq);
  return BrokerResult::TIMEOUT;
}

const loraiot_Response& loraBrokerGetResponse() {
  return lastResponse;
}

const PingResult& loraBrokerGetPingResult() {
  return lastPingResult;
}

void loraBrokerOnResponse(const loraiot_Response& response) {
  if (brokerMutex == nullptr || responseSem == nullptr) return;

  xSemaphoreTake(brokerMutex, portMAX_DELAY);
  bool match = pendingActive && !pendingIsPing &&
               (response.request_seq == pendingSeq);
  if (match) {
    lastResponse  = response;
    pendingActive = false;
  }
  xSemaphoreGive(brokerMutex);

  if (match) {
    xSemaphoreGive(responseSem);
  } else {
    Log_debug("LoRaBroker: response seq=%u discarded (pending=%u active=%d)",
              response.request_seq, (unsigned)pendingSeq, (int)pendingActive);
  }
}

void loraBrokerOnPingAck(uint8_t srcAddr, uint32_t rttMs) {
  if (brokerMutex == nullptr || responseSem == nullptr) return;

  xSemaphoreTake(brokerMutex, portMAX_DELAY);
  bool match = pendingActive && pendingIsPing;
  if (match) {
    lastPingResult.addr  = srcAddr;
    lastPingResult.rttMs = rttMs;
    pendingActive = false;
  }
  xSemaphoreGive(brokerMutex);

  if (match) {
    xSemaphoreGive(responseSem);
  }
}
