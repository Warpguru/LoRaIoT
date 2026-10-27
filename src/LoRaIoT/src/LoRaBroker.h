#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../lora/src/nanopb/loraiot.pb.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../lora/src/LoRaFrame.h"

/**
 * LoRaBroker.h - Request/response broker between HTTP handlers and loraTask.
 *
 * ESPAsyncWebServer callbacks run on Core 0 alongside the WiFi stack and must
 * never do Serial2 I/O directly (AGENTS.md threading rule). This broker provides
 * the synchronisation bridge:
 *
 *   HTTP handler (Core 0)          loraTask (Core 1 / single writer of UART2)
 *   ─────────────────────          ──────────────────────────────────────────
 *   1. loraBrokerSend(req, addr)   3. encodes + calls LoRaTransport.sendFrame()
 *   2. loraBrokerWait(timeout)     4. receive callback calls loraBrokerOnResponse()
 *      blocks on semaphore    <───    gives the semaphore, stores loraiot_Response
 *   5. reads loraBrokerGetResponse()
 *
 * Only one request/response can be in flight at a time (LoRa is half-duplex).
 * A mutex guards the pending slot. If a second request arrives while one is
 * in flight it receives BROKER_ERR_BUSY immediately.
 *
 * PING frames use LoRaTransport.sendFrame() with a proper LoRaFrame header and
 * nanopb-encoded Beacon payload. REQUEST frames (ECHO, READ, DESCRIBE) still use
 * the unframed Phase 2 stub pending the full M4 frame codec integration.
 */

// Maximum time an HTTP handler will block waiting for a RESPONSE (ms).
// ESPAsyncWebServer callbacks run on the async_tcp task which is monitored by
// the ESP Task WDT (default 5000 ms). Every loraBrokerWait() call blocks that
// task, so the combined wait must stay well below 5000 ms.
// Default is 3000 ms; the hard ceiling is 4000 ms (1000 ms headroom for the
// response-building path after the semaphore is given).
static constexpr uint32_t BROKER_DEFAULT_TIMEOUT_MS = 3000;

// Hard ceiling for the ?timeout=ms query parameter. Must be less than the
// ESP Task WDT timeout (5000 ms) minus response-building overhead (~1000 ms).
static constexpr uint32_t BROKER_MAX_TIMEOUT_MS = 4000;

// Minimum sensible timeout
static constexpr uint32_t BROKER_MIN_TIMEOUT_MS = 200;

/** Error codes returned by loraBrokerSend() and loraBrokerWait(). */
enum class BrokerResult : uint8_t {
  OK         = 0,  // Response received and stored successfully
  BUSY       = 1,  // Another request is already in flight
  TIMEOUT    = 2,  // No response received within the timeout window
  ENCODE_ERR = 3,  // nanopb encode failed (payload too large or bad input)
};

/** The type of outgoing request to build. */
enum class BrokerRequestType : uint8_t {
  ECHO     = 0,
  PING     = 1,
  READ     = 2,
  DESCRIBE = 3,
  DATA_ACK = 4,
};

/**
 * Initialise the broker mutex, semaphore and boot_id. Call once from applicationSetup()
 * after LoRaTransport.begin().
 */
void loraBrokerBegin();

/**
 * Return the boot_id generated at loraBrokerBegin(). Non-zero random value that changes
 * every boot. Travels in every outgoing PING frame as the Beacon payload boot_id field.
 */
uint32_t loraBrokerBootId();

/**
 * Read the pending PING slot (seq and send time) without modifying it.
 * Returns true and fills seq/sentMs only when a PING is currently in flight.
 * Thread-safe (taken under brokerMutex).
 */
bool loraBrokerPendingPing(uint16_t& seq, uint32_t& sentMs);

/**
 * Encode and enqueue a typed REQUEST frame to the given node address.
 * Assigns a new SEQ number to the request; the SEQ is used by loraBrokerOnResponse()
 * to match the reply.
 *
 * Parameters:
 *   type     - Which REQUEST command to build.
 *   dstAddr  - Destination node address (0x02..0xFE).
 *   payload  - Pointer to the relevant sub-message struct:
 *                ECHO:     const loraiot_Echo*
 *                PING:     nullptr  (PING is a BEACON-type frame, not REQUEST)
 *                READ:     const loraiot_Read*
 *                DESCRIBE: const loraiot_Describe*
 *   outSeq   - Set to the SEQ assigned to this request on success.
 *
 * Returns BrokerResult::BUSY if a prior request has not yet been answered.
 * Returns BrokerResult::ENCODE_ERR if nanopb encoding fails.
 * Returns BrokerResult::OK and populates outSeq on success.
 *
 * Thread-safe. May be called from any task/core.
 */
BrokerResult loraBrokerSend(BrokerRequestType type, uint8_t dstAddr,
                             const void* payload, uint32_t& outSeq);

/**
 * Block the calling task until the pending response arrives or timeout elapses.
 * Call immediately after a successful loraBrokerSend().
 *
 * Returns BrokerResult::OK      - response received; call loraBrokerGetResponse().
 *         BrokerResult::TIMEOUT - no response within timeoutMs.
 *
 * Must be called from the same task that called loraBrokerSend().
 */
BrokerResult loraBrokerWait(uint32_t timeoutMs = BROKER_DEFAULT_TIMEOUT_MS);

/**
 * Return a const reference to the most recently received RESPONSE.
 * Valid only after loraBrokerWait() returns OK. The struct is overwritten
 * on the next successful loraBrokerSend()/loraBrokerWait() cycle.
 */
const loraiot_Response& loraBrokerGetResponse();

/**
 * Called by the LoRa receive/decode layer when any RESPONSE frame arrives.
 * Matches request_seq against the pending outSeq; if it matches, stores the
 * response and unblocks the waiting HTTP handler via the semaphore.
 * No-op if no request is pending or the SEQ does not match.
 *
 * Safe to call from the loraTransportTask receive callback.
 */
void loraBrokerOnResponse(const loraiot_Response& response);

/**
 * Called by the LoRa receive/decode layer when a PING ACK (Ack frame for a
 * PING) arrives. Stores the responder address and RTT, then unblocks the
 * waiting handler. The Ack message carries no boot_id (doc/LoRaProtocol.md
 * section 6.1), so callers that need it read it from SensorRegistry.
 * No-op if no PING is pending.
 */
void loraBrokerOnPingAck(uint8_t srcAddr, uint32_t rttMs);

/**
 * After a successful PING wait, retrieve the ping result.
 * addr is the node that answered (differs from the destination for a broadcast PING).
 */
struct PingResult {
  uint8_t  addr;
  uint32_t rttMs;
};
const PingResult& loraBrokerGetPingResult();

/**
 * Called by the LoRa receive/decode layer when a DATA_ACK ACK (Ack frame for a
 * DATA_ACK frame) arrives. Stores the responder address and ACK status, then unblocks
 * the waiting handler.
 * No-op if no DATA_ACK is pending.
 */
void loraBrokerOnDataAck(uint8_t srcAddr, loraiot_Status ackStatus);

/**
 * After a successful DATA_ACK wait, retrieve the ack result.
 */
struct AckResult {
  uint8_t        addr;
  loraiot_Status status;
};
const AckResult& loraBrokerGetAckResult();

/**
 * Send a loraiot_Data payload reliably to dstAddr using DATA_ACK frames with retry.
 *
 * - Sends DATA_ACK frame, waits LORA_ACK_TIMEOUT_MS (200 ms).
 * - Up to LORA_MAX_RETRIES + 1 (4) attempts total.
 * - Retransmissions reuse the same SEQ.
 * - Delays random(LORA_BACKOFF_MIN_MS, LORA_BACKOFF_MAX_MS) before retries.
 * - If ACK received: fills outStatus and returns BrokerResult::OK.
 * - If all retries fail: calls LoRaTransport.incrementAckLoss(), sets outStatus = STATUS_BAD_REQUEST,
 *   returns BrokerResult::TIMEOUT.
 * - If broker mutex is held / pendingActive: returns BrokerResult::BUSY immediately.
 */
BrokerResult loraSendDataReliable(uint8_t dstAddr, const loraiot_Data& data,
                                  loraiot_Status& outStatus);
