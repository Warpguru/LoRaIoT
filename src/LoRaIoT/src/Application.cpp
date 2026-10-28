#include "Application.h"

#include <Arduino.h>

#include "../../util/src/LogBuffer.h"
#include "Server.h"
#include "../../util/src/StatusLed.h"
#include "Tier.h"
#include "LoRaConfig.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../lora/src/LoRaFrame.h"
#include "../../lora/src/LoRaState.h"
#include "../../lora/src/nanopb/pb_encode.h"
#include "../../lora/src/nanopb/pb_decode.h"
#include "../../lora/src/nanopb/loraiot.pb.h"
#include "../../diagnostics/src/Diagnostics.h"
#include "../../registry/src/PeerRegistry.h"
#include "DescribeCrawler.h"
#include "LoRaBroker.h"
#include "BeaconTask.h"
#include "PingTask.h"
#include "PushTask.h"

/**
 * Application.cpp - Main application setup and loop.
 *
 * Constructs the LoRa transport and global singletons.
 * HTTP handler logic lives in service/RestService.
 * WebSocket broadcast + receive live in service/WebSocketService.
 */

// ---- Application Constants -------------------------------------------------
static constexpr uint32_t BOOT_DELAY_MS    = 1000;  // Safe boot startup delay

// ---- LoRa readiness gate ---------------------------------------------------
// Set to true by applicationLoop() after the boot diagnostics complete.
// Radio tasks must poll loraIsReady() before touching the radio.
// Written once by the Arduino main task; read by FreeRTOS tasks.
// volatile is sufficient: single writer, monotonic true-only transition.
static volatile bool loraReady = false;

bool loraIsReady() {
  return loraReady;
}

// LOCAL owns control data and caches REMOTE's sensor data; REMOTE is the mirror image.
PeerRegistry& ownedRegistry() {
  return Gateway.isLocal() ? ControlRegistry : SensorRegistry;
}

PeerRegistry& cachedRegistry() {
  return Gateway.isLocal() ? SensorRegistry : ControlRegistry;
}

// ---- PING/ACK helpers ------------------------------------------------------

/**
 * Called by REMOTE when it receives a PING frame.
 * Sends an ACK back to the originator.
 * Runs on loraTransportTask - only queue operations, no UART I/O.
 */
static void sendPingAck(const LoRaFrame& ping) {
  // Build the ACK payload: Ack{acked_type=PING, acked_seq=ping.seq, status=OK}
  loraiot_Ack ack = loraiot_Ack_init_zero;
  ack.acked_type = static_cast<uint32_t>(LoRaFrameType::PING);
  ack.acked_seq  = ping.seq;
  ack.status     = loraiot_Status_STATUS_OK;

  uint8_t payloadBuf[loraiot_Ack_size];
  pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&pbOut, loraiot_Ack_fields, &ack)) {
    Log_error("ACK encode failed for PING seq=%u: %s", ping.seq, PB_GET_ERROR(&pbOut));
    return;
  }

  LoRaFrame ackFrame;
  ackFrame.ver  = LORA_PROTOCOL_VER;
  ackFrame.type = LoRaFrameType::ACK;
  ackFrame.src  = LoRaConfig.getAddress();
  ackFrame.dst  = ping.src;
  ackFrame.seq  = loraNextSeq();
  ackFrame.len  = static_cast<uint8_t>(pbOut.bytes_written);
  memcpy(ackFrame.payload, payloadBuf, ackFrame.len);

  if (LoRaTransport.sendFrame(ackFrame)) {
    Log_info("ACK sent to 0x%02X acked_seq=%u", ping.src, ping.seq);
  } else {
    Log_error("ACK sendFrame failed for PING seq=%u", ping.seq);
  }
}

/**
 * Called by either node when it receives a DATA_ACK frame.
 * Sends an ACK back to the originator with the given status.
 * Runs on loraTransportTask - only queue operations, no UART I/O.
 */
static void sendDataAck(const LoRaFrame& req, loraiot_Status status) {
  loraiot_Ack ack = loraiot_Ack_init_zero;
  ack.acked_type = static_cast<uint32_t>(LoRaFrameType::DATA_ACK);
  ack.acked_seq  = req.seq;
  ack.status     = status;

  uint8_t payloadBuf[loraiot_Ack_size];
  pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&pbOut, loraiot_Ack_fields, &ack)) {
    Log_error("ACK encode failed for DATA_ACK seq=%u: %s", req.seq, PB_GET_ERROR(&pbOut));
    return;
  }

  LoRaFrame ackFrame;
  ackFrame.ver  = LORA_PROTOCOL_VER;
  ackFrame.type = LoRaFrameType::ACK;
  ackFrame.src  = LoRaConfig.getAddress();
  ackFrame.dst  = req.src;
  ackFrame.seq  = loraNextSeq();
  ackFrame.len  = static_cast<uint8_t>(pbOut.bytes_written);
  memcpy(ackFrame.payload, payloadBuf, ackFrame.len);

  if (LoRaTransport.sendFrame(ackFrame)) {
    Log_info("DATA_ACK ACK sent to 0x%02X acked_seq=%u status=%u",
             req.src, req.seq, static_cast<uint8_t>(status));
  } else {
    Log_error("DATA_ACK ACK sendFrame failed for seq=%u", req.seq);
  }
}

// ---- loraPushData ----------------------------------------------------------

/**
 * Encode a loraiot_Data payload as a DATA frame and enqueue it for
 * transmission to dstAddr. Fire-and-forget: posts to the LoRa TX queue
 * and returns immediately. Safe to call from any task or core.
 * No-op if LoRa transport is not yet initialised.
 */
void loraPushData(uint8_t dstAddr, const loraiot_Data& data) {
  if (!loraIsReady()) {
    Log_debug("loraPushData: LoRa not ready, dropping DATA to 0x%02X", dstAddr);
    return;
  }

  uint8_t payloadBuf[LORA_MAX_PAYLOAD_LEN];
  pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&pbOut, loraiot_Data_fields, &data)) {
    Log_error("loraPushData: Data encode failed for dst=0x%02X: %s", dstAddr, PB_GET_ERROR(&pbOut));
    return;
  }

  LoRaFrame frame;
  frame.ver  = LORA_PROTOCOL_VER;
  frame.type = LoRaFrameType::DATA;
  frame.src  = LoRaConfig.getAddress();
  frame.dst  = dstAddr;
  frame.seq  = loraNextSeq();
  frame.len  = static_cast<uint8_t>(pbOut.bytes_written);
  memcpy(frame.payload, payloadBuf, frame.len);

  if (LoRaTransport.sendFrame(frame)) {
    Log_info("DATA pushed to 0x%02X device=%u values=%u seq=%u",
             dstAddr, data.device, data.values_count, frame.seq);
  } else {
    Log_error("loraPushData: sendFrame failed for dst=0x%02X", dstAddr);
  }
}

// ---- REQUEST/RESPONSE helpers ----------------------------------------------

/**
 * Send a RESPONSE with STATUS_BAD_REQUEST back to the requester.
 * Used when a Describe or Read request cannot be fulfilled.
 */
static void sendBadRequestResponse(const LoRaFrame& req) {
  static loraiot_Response response;
  response = loraiot_Response_init_zero;
  response.request_seq = req.seq;
  response.status      = loraiot_Status_STATUS_BAD_REQUEST;

  static uint8_t payloadBuf[loraiot_Response_size];
  pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&pbOut, loraiot_Response_fields, &response)) {
    Log_error("RESPONSE(BAD_REQUEST) encode failed for seq=%u: %s", req.seq, PB_GET_ERROR(&pbOut));
    return;
  }

  LoRaFrame respFrame;
  respFrame.ver  = LORA_PROTOCOL_VER;
  respFrame.type = LoRaFrameType::RESPONSE;
  respFrame.src  = LoRaConfig.getAddress();
  respFrame.dst  = req.src;
  respFrame.seq  = loraNextSeq();
  respFrame.len  = static_cast<uint8_t>(pbOut.bytes_written);
  memcpy(respFrame.payload, payloadBuf, respFrame.len);

  LoRaTransport.sendFrame(respFrame);
  Log_info("RESPONSE(BAD_REQUEST) sent to 0x%02X request_seq=%u", req.src, req.seq);
}

/**
 * Called when this node receives a REQUEST(Describe{device}) frame.
 * Looks up the device definition in ownedRegistry() and sends RESPONSE(Device)
 * or RESPONSE(STATUS_BAD_REQUEST) when the device index is unknown.
 * Device index 0 is the node itself - not in the registry (SoftAP devices
 * start at 1), so index 0 always returns BAD_REQUEST (signals end of device list
 * to the requester if no SoftAP devices are registered).
 * Runs on loraTransportTask - only queue operations, no UART I/O.
 */
static void sendDescribeResponse(const LoRaFrame& req) {
  static loraiot_Request request;
  request = loraiot_Request_init_zero;
  pb_istream_t pbIn = pb_istream_from_buffer(req.payload, req.len);
  if (!pb_decode(&pbIn, loraiot_Request_fields, &request)) {
    Log_warn("REQUEST(Describe) from 0x%02X seq=%u - decode failed: %s",
             req.src, req.seq, PB_GET_ERROR(&pbIn));
    return;
  }

  const uint32_t deviceIdx = request.command.describe.device;
  Log_info("REQUEST(Describe{device=%u}) from 0x%02X seq=%u", deviceIdx, req.src, req.seq);

  static loraiot_Device dev;
  dev = loraiot_Device_init_zero;
  if (!ownedRegistry().toProtoDevice(req.src, deviceIdx, dev)) {
    // Device not found (includes device 0 which is the node itself, not in the registry)
    Log_info("REQUEST(Describe{device=%u}): not found, sending BAD_REQUEST", deviceIdx);
    pushTaskNotifyCrawlComplete(req.src);
    sendBadRequestResponse(req);
    return;
  }

  static loraiot_Response response;
  response = loraiot_Response_init_zero;
  response.request_seq  = req.seq;
  response.status       = loraiot_Status_STATUS_OK;
  response.which_result = loraiot_Response_def_tag;
  response.result.def   = dev;

  static uint8_t payloadBuf[loraiot_Response_size];
  pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&pbOut, loraiot_Response_fields, &response)) {
    Log_error("RESPONSE(Device) encode failed for seq=%u: %s", req.seq, PB_GET_ERROR(&pbOut));
    return;
  }

  LoRaFrame respFrame;
  respFrame.ver  = LORA_PROTOCOL_VER;
  respFrame.type = LoRaFrameType::RESPONSE;
  respFrame.src  = LoRaConfig.getAddress();
  respFrame.dst  = req.src;
  respFrame.seq  = loraNextSeq();
  respFrame.len  = static_cast<uint8_t>(pbOut.bytes_written);
  memcpy(respFrame.payload, payloadBuf, respFrame.len);

  if (LoRaTransport.sendFrame(respFrame)) {
    Log_info("RESPONSE(Device{device=%u}) sent to 0x%02X request_seq=%u", deviceIdx, req.src, req.seq);
  } else {
    Log_error("RESPONSE(Device) sendFrame failed for request_seq=%u", req.seq);
  }
}

/**
 * Called when this node receives a REQUEST(Read{device}) frame.
 * Looks up the most recent values in ownedRegistry() and sends RESPONSE(Data)
 * or RESPONSE(STATUS_BAD_REQUEST) when the device index is unknown.
 * Runs on loraTransportTask - only queue operations, no UART I/O.
 */
static void sendReadResponse(const LoRaFrame& req) {
  static loraiot_Request request;
  request = loraiot_Request_init_zero;
  pb_istream_t pbIn = pb_istream_from_buffer(req.payload, req.len);
  if (!pb_decode(&pbIn, loraiot_Request_fields, &request)) {
    Log_warn("REQUEST(Read) from 0x%02X seq=%u - decode failed: %s",
             req.src, req.seq, PB_GET_ERROR(&pbIn));
    return;
  }

  const uint32_t deviceIdx = request.command.read.device;
  Log_info("REQUEST(Read{device=%u}) from 0x%02X seq=%u", deviceIdx, req.src, req.seq);

  static loraiot_Data data;
  data = loraiot_Data_init_zero;
  if (!ownedRegistry().toProtoData(req.src, deviceIdx, data)) {
    Log_info("REQUEST(Read{device=%u}): not found, sending BAD_REQUEST", deviceIdx);
    sendBadRequestResponse(req);
    return;
  }

  static loraiot_Response response;
  response = loraiot_Response_init_zero;
  response.request_seq  = req.seq;
  response.status       = loraiot_Status_STATUS_OK;
  response.which_result = loraiot_Response_data_tag;
  response.result.data  = data;

  static uint8_t payloadBuf[loraiot_Response_size];
  pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&pbOut, loraiot_Response_fields, &response)) {
    Log_error("RESPONSE(Data) encode failed for seq=%u: %s", req.seq, PB_GET_ERROR(&pbOut));
    return;
  }

  LoRaFrame respFrame;
  respFrame.ver  = LORA_PROTOCOL_VER;
  respFrame.type = LoRaFrameType::RESPONSE;
  respFrame.src  = LoRaConfig.getAddress();
  respFrame.dst  = req.src;
  respFrame.seq  = loraNextSeq();
  respFrame.len  = static_cast<uint8_t>(pbOut.bytes_written);
  memcpy(respFrame.payload, payloadBuf, respFrame.len);

  if (LoRaTransport.sendFrame(respFrame)) {
    Log_info("RESPONSE(Data{device=%u, values=%u}) sent to 0x%02X request_seq=%u",
             deviceIdx, data.values_count, req.src, req.seq);
  } else {
    Log_error("RESPONSE(Data) sendFrame failed for request_seq=%u", req.seq);
  }
}

/**
 * Called by REMOTE when it receives a REQUEST(echo) frame.
 * Decodes the Echo payload, reverses the text, and sends a RESPONSE back.
 * Runs on loraTransportTask - only queue operations, no UART I/O.
 */
static void sendEchoResponse(const LoRaFrame& req) {
  static loraiot_Request request;
  request = loraiot_Request_init_zero;
  pb_istream_t pbIn = pb_istream_from_buffer(req.payload, req.len);
  if (!pb_decode(&pbIn, loraiot_Request_fields, &request)) {
    Log_warn("REQUEST(echo) from 0x%02X seq=%u - decode failed: %s",
             req.src, req.seq, PB_GET_ERROR(&pbIn));
    return;
  }
  if (request.which_command != loraiot_Request_echo_tag) {
    Log_debug("REQUEST from 0x%02X seq=%u cmd=%u - not an echo, ignoring",
              req.src, req.seq, request.which_command);
    return;
  }

  // Reverse the text in place.
  char reversed[sizeof(request.command.echo.text)];
  const size_t textLen = strnlen(request.command.echo.text, sizeof(request.command.echo.text) - 1);
  for (size_t i = 0; i < textLen; i++) {
    reversed[i] = request.command.echo.text[textLen - 1 - i];
  }
  reversed[textLen] = '\0';
  Log_info("REQUEST(echo) from 0x%02X seq=%u text=\"%s\" -> \"%s\"",
           req.src, req.seq, request.command.echo.text, reversed);

  // Build RESPONSE{request_seq, echo{reversed}}.
  static loraiot_Response response;
  response = loraiot_Response_init_zero;
  response.request_seq  = req.seq;
  response.status       = loraiot_Status_STATUS_OK;
  response.which_result = loraiot_Response_echo_tag;
  strncpy(response.result.echo.text, reversed, sizeof(response.result.echo.text) - 1);

  static uint8_t payloadBuf[loraiot_Response_size];
  pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&pbOut, loraiot_Response_fields, &response)) {
    Log_error("RESPONSE(echo) encode failed for seq=%u: %s", req.seq, PB_GET_ERROR(&pbOut));
    return;
  }

  LoRaFrame respFrame;
  respFrame.ver  = LORA_PROTOCOL_VER;
  respFrame.type = LoRaFrameType::RESPONSE;
  respFrame.src  = LoRaConfig.getAddress();
  respFrame.dst  = req.src;
  respFrame.seq  = loraNextSeq();
  respFrame.len  = static_cast<uint8_t>(pbOut.bytes_written);
  memcpy(respFrame.payload, payloadBuf, respFrame.len);

  if (LoRaTransport.sendFrame(respFrame)) {
    Log_info("RESPONSE(echo) sent to 0x%02X request_seq=%u", req.src, req.seq);
  } else {
    Log_error("RESPONSE(echo) sendFrame failed for request_seq=%u", req.seq);
  }
}

// ---- LoRa Receive Callback -------------------------------------------------

/**
 * Called from loraTransportTask whenever a CRC-valid, addressed frame arrives.
 * PING, ACK and BEACON stay tier-specific; REQUEST, RESPONSE and DATA are handled
 * identically on both nodes.
 * All other frame types are logged at DEBUG and ignored.
 */
static void onLoRaReceive(const LoRaFrame& frame) {
  const bool isLocal = Gateway.isLocal();

  // Extract boot_id for duplicate/reboot detection.
  // PING and BEACON carry a Beacon payload; all other types pass 0.
  uint32_t bootId = 0;
  if (frame.type == LoRaFrameType::PING || frame.type == LoRaFrameType::BEACON) {
    loraiot_Beacon beacon = loraiot_Beacon_init_zero;
    pb_istream_t pbIn = pb_istream_from_buffer(frame.payload, frame.len);
    if (pb_decode(&pbIn, loraiot_Beacon_fields, &beacon)) {
      bootId = beacon.boot_id;
    }
  }

  // Any valid frame from a peer proves it is alive, so refresh its liveness
  // (and revive it if it had been marked offline). Done before the duplicate check
  // because a retransmitted duplicate is just as good a proof of life. Unknown nodes
  // are ignored here; only a BEACON (on LOCAL) or PING (on REMOTE) creates a registry entry.
  if (cachedRegistry().markSeen(frame.src)) {
    Log_info("Node 0x%02X is back online", frame.src);
    if (isLocal) {
      pushTaskNotifyNodeBack(frame.src);
    }
  }

  // Duplicate detection and reboot detection (doc/LoRaProtocol.md §6, §6.1).
  // Drop duplicate frames silently; LoRaState handles the state update.
  if (!LoRaState.updateAndCheckDuplicate(frame.src, frame.seq,
                                          static_cast<uint8_t>(frame.type), bootId)) {
    if (frame.type == LoRaFrameType::DATA_ACK) {
      Log_info("DATA_ACK from 0x%02X seq=%u is duplicate - re-sending ACK", frame.src, frame.seq);
      sendDataAck(frame, loraiot_Status_STATUS_OK);
    }
    return;  // duplicate - already logged at DEBUG inside LoRaState
  }

  // Notify the beacon task that LOCAL has contacted us (REMOTE only).
  // Any non-duplicate frame from any sender is sufficient - it proves
  // the link is up and we have been discovered.
  if (!isLocal) {
    beaconTaskNotifyDiscovered();
  }

  switch (frame.type) {

    case LoRaFrameType::PING:
      if (!isLocal) {
        // REMOTE: register sender in cachedRegistry() (ControlRegistry), schedule
        // Describe crawl if boot_id changed or registry_rev advanced.
        loraiot_Beacon beacon = loraiot_Beacon_init_zero;
        pb_istream_t pbIn = pb_istream_from_buffer(frame.payload, frame.len);
        if (pb_decode(&pbIn, loraiot_Beacon_fields, &beacon)) {
          const bool crawlNeeded = cachedRegistry().updatePeerMeta(
              frame.src, beacon.boot_id,
              beacon.has_registry_rev, beacon.registry_rev);
          Log_info("PING from 0x%02X boot_id=0x%08X registry_rev=%u%s",
                   frame.src, beacon.boot_id,
                   beacon.has_registry_rev ? beacon.registry_rev : 0,
                   crawlNeeded ? " (crawl scheduled)" : "");
          if (crawlNeeded) {
            describeCrawlerSchedule(frame.src);
          }
        } else {
          Log_warn("PING from 0x%02X seq=%u - Beacon decode failed: %s",
                   frame.src, frame.seq, PB_GET_ERROR(&pbIn));
        }
      } else {
        // LOCAL: a REMOTE keep-alive PING. The markSeen() above already refreshed the
        // liveness of a known REMOTE; no registry metadata is taken from it.
        Log_debug("PING from 0x%02X seq=%u (REMOTE keep-alive)", frame.src, frame.seq);
      }
      // Both tiers answer every PING so the sender's keep-alive (PingTask) gets its ACK.
      sendPingAck(frame);
      break;

    case LoRaFrameType::ACK: {
      loraiot_Ack ack = loraiot_Ack_init_zero;
      pb_istream_t pbIn = pb_istream_from_buffer(frame.payload, frame.len);
      if (!pb_decode(&pbIn, loraiot_Ack_fields, &ack)) {
        Log_warn("ACK from 0x%02X seq=%u - Ack decode failed: %s",
                 frame.src, frame.seq, PB_GET_ERROR(&pbIn));
        break;
      }
      if (ack.acked_type == static_cast<uint32_t>(LoRaFrameType::PING)) {
        // Both tiers send keep-alive PINGs, so both accept the ACK.
        uint16_t pendingSeq = 0;
        uint32_t sentMs     = 0;
        if (!loraBrokerPendingPing(pendingSeq, sentMs)) {
          Log_debug("ACK from 0x%02X acked_seq=%u: no PING pending, discarding", frame.src, ack.acked_seq);
          break;
        }
        if (ack.acked_seq != pendingSeq) {
          Log_debug("ACK from 0x%02X: acked_seq=%u != pending seq=%u, discarding",
                    frame.src, ack.acked_seq, pendingSeq);
          break;
        }
        const uint32_t rttMs = millis() - sentMs;
        Log_info("PING ACK from 0x%02X seq=%u rtt=%ums", frame.src, ack.acked_seq, rttMs);
        loraBrokerOnPingAck(frame.src, rttMs);
      } else if (ack.acked_type == static_cast<uint32_t>(LoRaFrameType::DATA_ACK)) {
        Log_info("DATA_ACK ACK from 0x%02X seq=%u status=%u",
                 frame.src, ack.acked_seq, static_cast<uint8_t>(ack.status));
        loraBrokerOnDataAck(frame.src, ack.status);
      } else {
        Log_debug("ACK from 0x%02X: unhandled acked_type=%u, ignoring", frame.src, ack.acked_type);
      }
      break;
    }

    case LoRaFrameType::REQUEST: {
      // Dispatch to the appropriate handler based on which_command.
      // Decode the request type first (without consuming payload - we re-decode inside each handler).
      static loraiot_Request peek;
      peek = loraiot_Request_init_zero;
      pb_istream_t pbPeek = pb_istream_from_buffer(frame.payload, frame.len);
      if (!pb_decode(&pbPeek, loraiot_Request_fields, &peek)) {
        Log_warn("REQUEST from 0x%02X seq=%u - decode failed: %s",
                 frame.src, frame.seq, PB_GET_ERROR(&pbPeek));
        break;
      }
      if (peek.which_command == loraiot_Request_echo_tag) {
        sendEchoResponse(frame);
      } else if (peek.which_command == loraiot_Request_describe_tag) {
        sendDescribeResponse(frame);
      } else if (peek.which_command == loraiot_Request_read_tag) {
        sendReadResponse(frame);
      } else {
        Log_debug("onLoRaReceive: unknown REQUEST cmd=%u from 0x%02X seq=%u",
                  peek.which_command, frame.src, frame.seq);
      }
      break;
    }

    case LoRaFrameType::RESPONSE: {
      // Decode the Response payload, then route to the correct handler.
      // - def_tag                       -> describeCrawlerOnResponse (Describe crawl)
      // - STATUS_BAD_REQUEST            -> describeCrawlerOnResponse (end of device list) and
      //                                    loraBrokerOnResponse (failed REST Read); each ignores
      //                                    a response whose request_seq it is not waiting for
      // - data_tag or echo_tag          -> loraBrokerOnResponse (REST handler waiting)
      static loraiot_Response response;
      response = loraiot_Response_init_zero;
      pb_istream_t pbIn = pb_istream_from_buffer(frame.payload, frame.len);
      if (!pb_decode(&pbIn, loraiot_Response_fields, &response)) {
        Log_warn("RESPONSE from 0x%02X seq=%u - decode failed: %s",
                 frame.src, frame.seq, PB_GET_ERROR(&pbIn));
        break;
      }
      Log_info("RESPONSE from 0x%02X request_seq=%u status=%u which=%u",
               frame.src, response.request_seq, (uint8_t)response.status,
               response.which_result);
      if (response.which_result == loraiot_Response_def_tag) {
        // Describe crawl response (Device definition)
        describeCrawlerOnResponse(frame.src, response);
      } else if (response.status == loraiot_Status_STATUS_BAD_REQUEST) {
        // End-of-list signal for the crawler, or error answer for a waiting REST handler
        describeCrawlerOnResponse(frame.src, response);
        loraBrokerOnResponse(response);
      } else {
        // Echo or Data response destined for a waiting REST handler
        loraBrokerOnResponse(response);
      }
      break;
    }

    case LoRaFrameType::BEACON:
      if (isLocal) {
        // LOCAL: register the sender in SensorRegistry and schedule a Describe
        // crawl if boot_id changed or registry_rev advanced.
        // The Beacon payload was already decoded at the top of this function.
        loraiot_Beacon beacon = loraiot_Beacon_init_zero;
        pb_istream_t pbIn = pb_istream_from_buffer(frame.payload, frame.len);
        if (pb_decode(&pbIn, loraiot_Beacon_fields, &beacon)) {
          const bool crawlNeeded = SensorRegistry.updatePeerMeta(
              frame.src, beacon.boot_id,
              beacon.has_registry_rev, beacon.registry_rev);
          Log_info("BEACON from 0x%02X boot_id=0x%08X registry_rev=%u%s",
                   frame.src, beacon.boot_id,
                   beacon.has_registry_rev ? beacon.registry_rev : 0,
                   crawlNeeded ? " (crawl scheduled)" : "");
          if (crawlNeeded) {
            describeCrawlerSchedule(frame.src);
          }
          // Answer the BEACON with a PING so the REMOTE learns it was discovered and
          // drops to the slow beacon rate. Queued, not sent here, because waiting for
          // the ACK would block the very task that receives it.
          pingTaskNotifyBeacon(frame.src);
        } else {
          Log_warn("BEACON from 0x%02X seq=%u - Beacon decode failed: %s",
                   frame.src, frame.seq, PB_GET_ERROR(&pbIn));
        }
      } else {
        Log_debug("onLoRaReceive: REMOTE received unexpected BEACON from 0x%02X", frame.src);
      }
      break;

    case LoRaFrameType::DATA: {
      loraiot_Data data = loraiot_Data_init_zero;
      pb_istream_t pbIn = pb_istream_from_buffer(frame.payload, frame.len);
      if (!pb_decode(&pbIn, loraiot_Data_fields, &data)) {
        Log_warn("DATA from 0x%02X seq=%u - decode failed: %s",
                 frame.src, frame.seq, PB_GET_ERROR(&pbIn));
        break;
      }
      if (cachedRegistry().storeData(frame.src, data)) {
        Log_info("DATA from 0x%02X device=%u values=%u stored",
                 frame.src, data.device, data.values_count);
      } else {
        // Device schema not yet known - crawl immediately so the next DATA
        // frame is not dropped too. The current frame's values are lost but
        // the window shrinks from the BEACON interval to one crawl round-trip.
        describeCrawlerSchedule(frame.src);
        Log_info("DATA from 0x%02X device=%u: schema unknown, Describe crawl scheduled",
                 frame.src, data.device);
      }
      break;
    }

    case LoRaFrameType::DATA_ACK: {
      loraiot_Data data = loraiot_Data_init_zero;
      pb_istream_t pbIn = pb_istream_from_buffer(frame.payload, frame.len);
      if (!pb_decode(&pbIn, loraiot_Data_fields, &data)) {
        Log_warn("DATA_ACK from 0x%02X seq=%u - decode failed: %s",
                 frame.src, frame.seq, PB_GET_ERROR(&pbIn));
        sendDataAck(frame, loraiot_Status_STATUS_BAD_REQUEST);
        break;
      }
      if (cachedRegistry().storeData(frame.src, data)) {
        Log_info("DATA_ACK from 0x%02X device=%u values=%u stored",
                 frame.src, data.device, data.values_count);
        sendDataAck(frame, loraiot_Status_STATUS_OK);
      } else {
        // Device schema not yet known - crawl immediately and answer BAD_REQUEST
        describeCrawlerSchedule(frame.src);
        Log_info("DATA_ACK from 0x%02X device=%u: schema unknown, Describe crawl scheduled",
                 frame.src, data.device);
        sendDataAck(frame, loraiot_Status_STATUS_BAD_REQUEST);
      }
      break;
    }

    default:
      Log_debug("onLoRaReceive: unhandled frame type=0x%02X from 0x%02X seq=%u",
                static_cast<uint8_t>(frame.type), frame.src, frame.seq);
      break;
  }
}

// ---- applicationSetup ------------------------------------------------------

void applicationSetup() {
  // Turn the status LED on immediately (BOOTING state) before anything else.
  // The led task becomes the sole owner of STATUS_LED_PIN from this point forward.
  statusLed.begin();

  // Initialize registries early so they are ready before any LoRa frames arrive.
  // SensorRegistry: REMOTE owns, LOCAL caches. ControlRegistry: LOCAL owns, REMOTE caches.
  // Both are always initialized; the active tier gates which role is used at runtime.
  SensorRegistry.begin();
  ControlRegistry.begin();

  Serial.begin(LoRaTransportClass::NORMAL_BAUD);
  delay(BOOT_DELAY_MS);

  // Initialise the levelled logger. Must be called before any Log_* call.
  LogBuffer.begin();

  // Load the persisted gateway tier (LOCAL/REMOTE) from NVS. Must be called
  // before setupServer() so the captive portal can pre-select the saved tier.
  Gateway.begin();

  // Initialize and load the LoRa settings (channel and address) from NVS.
  LoRaConfig.begin();

  // Apply the loaded CPU frequency config
  setCpuFrequencyMhz(LoRaConfig.getCpuFreq());

  // Configuration details (log level, channel, address, key, CPU frequency) are
  // logged once by LoRaConfig.begin(); do not repeat them here.
  Log_info("Starting LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ")");
  Serial.printf("\n--- LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ") ---\n");

  // Initialize WiFi, construct service objects, register all routes, start server.
  // setupServer() blocks in the captive portal until the node is provisioned, and
  // the portal save callback stores the tier, channel and address. The E220 must
  // therefore be configured afterwards, otherwise it would be programmed with the
  // defaults of a tier that has not been chosen yet.
  setupServer();

  // Set the transport layer callback and initialize E220 hardware with the final
  // tier, address and channel. Without a tier the node has no LoRa role, so the
  // transport stays off (the REST API remains reachable to provision the node).
  if (Gateway.isConfigured()) {
    LoRaState.begin();
    LoRaTransport.setReceiveCallback(onLoRaReceive);
    LoRaTransport.begin();
    loraBrokerBegin();
    // Only REMOTE beacons. Both tiers run the same keep-alive task: LOCAL answers
    // beacons and detects dead REMOTEs, REMOTE detects a dead LOCAL.
    if (Gateway.isRemote()) {
      beaconTaskBegin();
    }
    pingTaskBegin();
    pushTaskBegin();
    Log_info("LoRa started: tier=%s loraChannel=0x%02X loraAddr=0x%02X airRate=%s",
             Gateway.getTierName(), LoRaConfig.getChannel(), LoRaConfig.getAddress(),
             LoRaTransportClass::airRateName(LoRaConfig.getAirRate()));
    // LoRa module is configured and online; transition the status LED to READY
    statusLed.setState(LedState::READY);
  } else {
    Log_error("Gateway tier is not configured - LoRa transport not started. Call GET /reset and select a tier in the captive portal.");
    statusLed.setState(LedState::NO_DEVICE);
  }
}

// ---- applicationLoop -------------------------------------------------------

void applicationLoop() {
  // Service any pending on-demand hardware wiring checks.
  // On the very first call this runs the boot E220 + codec diagnostics.
  diagnosticsLoop();

  // Gate: mark LoRa as ready after the first diagnostics run completes.
  // All radio tasks (beaconTask, future tasks) wait on loraIsReady() before
  // transmitting. loraReady is written once and never cleared.
  if (!loraReady) {
    loraReady = true;
    Log_info("applicationLoop: boot diagnostics complete - LoRa radio gate opened");
  }

  // Purge stale LoRaState entries (both tiers; cheap when no entries are stale)
  LoRaState.purgeStale();

  // Drive the Describe crawl state machine (both tiers; no-op when nothing is pending)
  describeCrawlerLoop();


  // ESPAsyncWebServer is fully non-blocking; this is a no-op kept for symmetry.
  handleServerRequests();

  // TODO: drain WebSocket command queue and dispatch LoRa/device actions.
  // Mirror the pattern in SerialController/Application.cpp applicationLoop().
}
