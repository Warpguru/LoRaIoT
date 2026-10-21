#include "LoRaBroker.h"

#include "../../util/src/LogBuffer.h"
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
 * Phase 2 stub: encodeAndSend() logs the intent and calls LoRaTransport.sendRaw()
 * with a zeroed buffer as a placeholder. Phase 3 replaces this with real nanopb
 * frame encoding once the full frame header/CRC layer is wired.
 */

// ---- Broker state -----------------------------------------------------------

static SemaphoreHandle_t brokerMutex   = nullptr;
static SemaphoreHandle_t responseSem   = nullptr;

static volatile bool     pendingActive = false;
static volatile uint32_t pendingSeq    = 0;
static volatile bool     pendingIsPing = false;

static loraiot_Response  lastResponse  = loraiot_Response_init_zero;
static PingResult        lastPingResult = {0, 0};

// Monotonically increasing SEQ counter for outgoing REQUESTs
static volatile uint32_t nextSeq = 1;

// ---- Phase 2 stub frame sender ----------------------------------------------

/**
 * Encode a loraiot_Request into a raw byte frame and queue it for transmission.
 *
 * Phase 2 stub: builds only the nanopb payload (no frame header / CRC yet).
 * Sends the raw protobuf bytes directly via LoRaTransport.sendRaw() so the
 * round-trip can be exercised on hardware before Phase 3 wires the full frame.
 *
 * Phase 3 will wrap this with: [0xAA 0x55][VER][TYPE][SRC][DST][SEQ][LEN]
 * + payload + CRC16, all in one atomic write.
 */
static BrokerResult encodeAndSend(const loraiot_Request& req, uint8_t dstAddr, uint32_t seq) {
  uint8_t buf[LORA_MAX_PAYLOAD_LEN];
  pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
  if (!pb_encode(&stream, loraiot_Request_fields, &req)) {
    Log_error("LoRaBroker: nanopb encode failed: %s", PB_GET_ERROR(&stream));
    return BrokerResult::ENCODE_ERR;
  }
  size_t encodedLen = stream.bytes_written;
  Log_info("LoRaBroker: sending REQUEST seq=%u type=%u dst=0x%02X encoded=%u bytes [Phase2 stub - no frame header yet]",
           seq, req.which_command, dstAddr, (unsigned)encodedLen);
  LoRaTransport.sendRaw(buf, encodedLen);
  return BrokerResult::OK;
}

// ---- Public API -------------------------------------------------------------

void loraBrokerBegin() {
  brokerMutex  = xSemaphoreCreateMutex();
  responseSem  = xSemaphoreCreateBinary();
  if (brokerMutex == nullptr || responseSem == nullptr) {
    Log_error("LoRaBroker::begin - failed to create synchronisation primitives");
  }
}

BrokerResult loraBrokerSend(BrokerRequestType type, uint8_t dstAddr,
                             const void* payload, uint32_t& outSeq) {
  if (brokerMutex == nullptr) return BrokerResult::BUSY;

  xSemaphoreTake(brokerMutex, portMAX_DELAY);

  if (pendingActive) {
    xSemaphoreGive(brokerMutex);
    Log_warn("LoRaBroker::send - rejected, another request is in flight (seq=%u)", pendingSeq);
    return BrokerResult::BUSY;
  }

  // Drain any stale semaphore token from a previous cycle
  xSemaphoreTake(responseSem, 0);

  uint32_t seq = nextSeq++;
  pendingSeq    = seq;
  pendingActive = true;
  pendingIsPing = (type == BrokerRequestType::PING);
  outSeq        = seq;

  xSemaphoreGive(brokerMutex);

  // Build the loraiot_Request outside the mutex (no shared state touched)
  BrokerResult encResult = BrokerResult::OK;

  if (type == BrokerRequestType::PING) {
    // PING is a BEACON-type frame (not REQUEST/RESPONSE). Send a minimal
    // plain-text ping for now; Phase 3 will encode a proper Beacon frame.
    const char pingMsg[] = "PING\n";
    Log_info("LoRaBroker: sending PING seq=%u dst=0x%02X [Phase2 stub]", seq, dstAddr);
    LoRaTransport.sendRaw(reinterpret_cast<const uint8_t*>(pingMsg), sizeof(pingMsg) - 1);
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
  Log_warn("LoRaBroker::wait - timeout after %u ms (seq=%u)", timeoutMs, (unsigned)pendingSeq);
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

void loraBrokerOnPingAck(uint32_t bootId, uint32_t rttMs) {
  if (brokerMutex == nullptr || responseSem == nullptr) return;

  xSemaphoreTake(brokerMutex, portMAX_DELAY);
  bool match = pendingActive && pendingIsPing;
  if (match) {
    lastPingResult.bootId = bootId;
    lastPingResult.rttMs  = rttMs;
    pendingActive = false;
  }
  xSemaphoreGive(brokerMutex);

  if (match) {
    xSemaphoreGive(responseSem);
  }
}
