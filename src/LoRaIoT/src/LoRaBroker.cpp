#include "LoRaBroker.h"

#include <esp_system.h>

#include "../../util/src/LogBuffer.h"
#include "../../LoRaIoT/src/LoRaConfig.h"
#include "../../lora/src/LoRaFrame.h"
#include "../../lora/src/nanopb/pb_encode.h"
#include "../../registry/src/PeerRegistry.h"
#include "Application.h"

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

// pendingKind values
static constexpr uint8_t BROKER_KIND_REQUEST  = 0;  // waiting for RESPONSE
static constexpr uint8_t BROKER_KIND_PING     = 1;  // waiting for PING ACK
static constexpr uint8_t BROKER_KIND_DATA_ACK = 2;  // waiting for DATA_ACK ACK
static volatile uint8_t  pendingKind          = BROKER_KIND_REQUEST;

static loraiot_Response  lastResponse     = loraiot_Response_init_zero;
static PingResult        lastPingResult   = {0, 0};
static AckResult         lastAckResult    = {0, loraiot_Status_STATUS_OK};

// Boot identity - random non-zero value generated once at loraBrokerBegin().
// Carried in every outgoing PING Beacon payload so REMOTE can log it.
static uint32_t bootId = 0;

// ---- REQUEST frame sender ---------------------------------------------------

/**
 * Encode a loraiot_Request protobuf into a full LoRaFrame and enqueue for TX.
 * Uses encodeFrame() + LoRaTransport.sendFrame() - same path as PING.
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

  // Generate a non-zero random boot_id (0 is the sentinel PEER_BOOT_ID_NONE).
  do {
    bootId = esp_random();
  } while (bootId == 0);
  Log_info("LoRaBroker::begin - boot_id=0x%08X", bootId);
}

uint32_t loraBrokerBootId() {
  return bootId;
}

bool loraBrokerPendingPing(uint16_t& seq, uint32_t& sentMs) {
  if (brokerMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(brokerMutex, portMAX_DELAY);
  const bool active = pendingActive && (pendingKind == BROKER_KIND_PING);
  if (active) {
    seq    = pendingSeq;
    sentMs = pendingSentMs;
  }
  xSemaphoreGive(brokerMutex);
  return active;
}

BrokerResult loraBrokerSend(BrokerRequestType type, uint8_t dstAddr,
                             const void* payload, uint32_t& outSeq) {
  if (brokerMutex == nullptr) {
    return BrokerResult::BUSY;
  }

  // Try-acquire: if the mutex is held, another send is actively building/sending a
  // frame. Return BUSY immediately rather than blocking - callers must not queue
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
  if (type == BrokerRequestType::PING) {
    pendingKind = BROKER_KIND_PING;
  } else if (type == BrokerRequestType::DATA_ACK) {
    pendingKind = BROKER_KIND_DATA_ACK;
  } else {
    pendingKind = BROKER_KIND_REQUEST;
  }
  outSeq        = seq;

  xSemaphoreGive(brokerMutex);

  // Build and send the frame outside the mutex (no shared state touched after this point)
  BrokerResult encResult = BrokerResult::OK;

  if (type == BrokerRequestType::PING) {
    // Encode Beacon{boot_id} as the PING payload, wrap in a proper LoRaFrame and send.
    loraiot_Beacon beacon = loraiot_Beacon_init_zero;
    beacon.boot_id = bootId;

    // Unicast PING carries registry_rev from this node's owner registry only if it has
    // devices registered for the destination peer (R4). Same code on both tiers: LOCAL
    // advertises its control table, REMOTE its sensor table. Broadcast PINGs (0xFF) stay unchanged.
    if (dstAddr != LORA_ADDR_BROADCAST && ownedRegistry().getDeviceCount(dstAddr) > 0) {
      beacon.has_registry_rev = true;
      beacon.registry_rev     = ownedRegistry().getRegistryRev(dstAddr);
    }

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
  if (brokerMutex == nullptr || responseSem == nullptr) {
    return;
  }

  xSemaphoreTake(brokerMutex, portMAX_DELAY);
  bool match = pendingActive && (pendingKind == BROKER_KIND_REQUEST) &&
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
  if (brokerMutex == nullptr || responseSem == nullptr) {
    return;
  }

  xSemaphoreTake(brokerMutex, portMAX_DELAY);
  bool match = pendingActive && (pendingKind == BROKER_KIND_PING);
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

void loraBrokerOnDataAck(uint8_t srcAddr, loraiot_Status ackStatus) {
  if (brokerMutex == nullptr || responseSem == nullptr) {
    return;
  }

  xSemaphoreTake(brokerMutex, portMAX_DELAY);
  bool match = pendingActive && (pendingKind == BROKER_KIND_DATA_ACK);
  if (match) {
    lastAckResult.addr   = srcAddr;
    lastAckResult.status = ackStatus;
    pendingActive = false;
  }
  xSemaphoreGive(brokerMutex);

  if (match) {
    xSemaphoreGive(responseSem);
  }
}

const AckResult& loraBrokerGetAckResult() {
  return lastAckResult;
}

BrokerResult loraSendDataReliable(uint8_t dstAddr, const loraiot_Data& data,
                                  loraiot_Status& outStatus) {
  if (brokerMutex == nullptr) {
    return BrokerResult::BUSY;
  }

  // Try-acquire broker mutex
  if (xSemaphoreTake(brokerMutex, 0) != pdTRUE) {
    Log_warn("loraSendDataReliable - rejected, broker mutex held (another send in progress)");
    return BrokerResult::BUSY;
  }

  if (pendingActive) {
    xSemaphoreGive(brokerMutex);
    Log_warn("loraSendDataReliable - rejected, another request is in flight (seq=%u)", pendingSeq);
    return BrokerResult::BUSY;
  }

  // Encode the Data payload into LoRaFrame
  uint8_t payloadBuf[loraiot_Data_size];
  pb_ostream_t stream = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&stream, loraiot_Data_fields, &data)) {
    xSemaphoreGive(brokerMutex);
    Log_error("loraSendDataReliable: Data nanopb encode failed: %s", PB_GET_ERROR(&stream));
    return BrokerResult::ENCODE_ERR;
  }

  // Drain stale semaphore token
  xSemaphoreTake(responseSem, 0);

  const uint16_t seq = loraNextSeq();
  pendingSeq     = seq;
  pendingActive  = true;
  pendingKind    = BROKER_KIND_DATA_ACK;
  pendingSentMs  = millis();

  xSemaphoreGive(brokerMutex);

  LoRaFrame frame;
  frame.ver  = LORA_PROTOCOL_VER;
  frame.type = LoRaFrameType::DATA_ACK;
  frame.src  = LoRaConfig.getAddress();
  frame.dst  = dstAddr;
  frame.seq  = seq;
  frame.len  = static_cast<uint8_t>(stream.bytes_written);
  memcpy(frame.payload, payloadBuf, frame.len);

  // Retry loop: up to LORA_MAX_RETRIES + 1 attempts (4 total)
  const uint8_t maxAttempts = LoRaTransportClass::LORA_MAX_RETRIES + 1;
  for (uint8_t attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) {
      // Backoff delay before retry
      uint32_t backoffMs = LoRaTransportClass::LORA_BACKOFF_MIN_MS +
          (esp_random() % (LoRaTransportClass::LORA_BACKOFF_MAX_MS - LoRaTransportClass::LORA_BACKOFF_MIN_MS + 1));
      vTaskDelay(pdMS_TO_TICKS(backoffMs));

      // Re-arm pendingActive if it was cleared by previous wait timeout
      xSemaphoreTake(brokerMutex, portMAX_DELAY);
      xSemaphoreTake(responseSem, 0);
      pendingActive = true;
      pendingKind   = BROKER_KIND_DATA_ACK;
      pendingSeq    = seq;
      xSemaphoreGive(brokerMutex);

      Log_info("loraSendDataReliable: retry %u/%u seq=%u dst=0x%02X",
               attempt, LoRaTransportClass::LORA_MAX_RETRIES, seq, dstAddr);
    }

    if (!LoRaTransport.sendFrame(frame)) {
      Log_error("loraSendDataReliable: sendFrame failed seq=%u dst=0x%02X", seq, dstAddr);
    }

    // Wait for ACK
    BrokerResult waitRes = loraBrokerWait(LoRaTransportClass::LORA_ACK_TIMEOUT_MS);
    if (waitRes == BrokerResult::OK) {
      outStatus = lastAckResult.status;
      Log_info("loraSendDataReliable: ACK received seq=%u status=%u (attempt %u)",
               seq, static_cast<uint8_t>(outStatus), attempt + 1);
      return BrokerResult::OK;
    }
  }

  // All attempts failed
  LoRaTransport.incrementAckLoss();
  outStatus = loraiot_Status_STATUS_BAD_REQUEST;
  Log_warn("loraSendDataReliable: all %u attempts timed out seq=%u dst=0x%02X",
           maxAttempts, seq, dstAddr);
  return BrokerResult::TIMEOUT;
}
