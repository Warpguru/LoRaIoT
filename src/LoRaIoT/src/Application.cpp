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
#include "DeviceRegistry.h"
#include "SensorRegistry.h"
#include "DescribeCrawler.h"
#include "LoRaBroker.h"

/**
 * Application.cpp - Main application setup and loop.
 *
 * Constructs the LoRa transport and global singletons.
 * HTTP handler logic lives in service/RestService.
 * WebSocket broadcast + receive live in service/WebSocketService.
 */

// ---- Application Constants -------------------------------------------------
static constexpr uint32_t PING_INTERVAL_MS = 2000;  // Periodic PING interval on LOCAL
static constexpr uint32_t BOOT_DELAY_MS    = 1000;  // Safe boot startup delay

// ---- PING/ACK helpers ------------------------------------------------------

/**
 * Called by REMOTE when it receives a PING frame.
 * Decodes the Beacon payload (logs the sender's boot_id), then sends an ACK back.
 * Runs on loraTransportTask - only queue operations, no UART I/O.
 */
static void sendPingAck(const LoRaFrame& ping) {
  // Decode the Beacon payload to log LOCAL's boot_id (proves the protobuf round-trip).
  loraiot_Beacon beacon = loraiot_Beacon_init_zero;
  pb_istream_t pbIn = pb_istream_from_buffer(ping.payload, ping.len);
  if (pb_decode(&pbIn, loraiot_Beacon_fields, &beacon)) {
    Log_info("PING from 0x%02X seq=%u boot_id=0x%08X",
             ping.src, ping.seq, beacon.boot_id);
  } else {
    Log_warn("PING from 0x%02X seq=%u - Beacon decode failed: %s",
             ping.src, ping.seq, PB_GET_ERROR(&pbIn));
  }

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

// ---- LoRa Receive Callback -------------------------------------------------

/**
 * Called from loraTransportTask whenever a CRC-valid, addressed frame arrives.
 * Dispatch is role-gated: REMOTE handles PING; LOCAL handles ACK.
 * All other frame types are logged at DEBUG and ignored until M4/M6 wire them.
 */
static void onLoRaReceive(const LoRaFrame& frame) {
  const bool isLocal = (strcmp(Gateway.getTierName(), "LOCAL") == 0);

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

  // Duplicate detection and reboot detection (doc/LoRaProtocol.md §6, §6.1).
  // Drop duplicate frames silently; LoRaState handles the state update.
  if (!LoRaState.updateAndCheckDuplicate(frame.src, frame.seq,
                                          static_cast<uint8_t>(frame.type), bootId)) {
    return;  // duplicate - already logged at DEBUG inside LoRaState
  }

  switch (frame.type) {

    case LoRaFrameType::PING:
      if (!isLocal) {
        // REMOTE: send an ACK back to the originator
        sendPingAck(frame);
      } else {
        Log_debug("onLoRaReceive: LOCAL received unexpected PING from 0x%02X", frame.src);
      }
      break;

    case LoRaFrameType::ACK:
      if (isLocal) {
        // LOCAL: match the ACK to the pending PING, compute RTT, wake the broker
        loraiot_Ack ack = loraiot_Ack_init_zero;
        pb_istream_t pbIn = pb_istream_from_buffer(frame.payload, frame.len);
        if (!pb_decode(&pbIn, loraiot_Ack_fields, &ack)) {
          Log_warn("ACK from 0x%02X seq=%u - Ack decode failed: %s",
                   frame.src, frame.seq, PB_GET_ERROR(&pbIn));
          break;
        }
        if (ack.acked_type != static_cast<uint32_t>(LoRaFrameType::PING)) {
          Log_debug("ACK from 0x%02X: acked_type=%u (not PING), ignoring", frame.src, ack.acked_type);
          break;
        }
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
        Log_info("PING ACK from 0x%02X seq=%u rtt=%u ms", frame.src, ack.acked_seq, rttMs);
        loraBrokerOnPingAck(DEVICE_REGISTRY_BOOT_ID_NONE, rttMs);
      } else {
        Log_debug("onLoRaReceive: REMOTE received unexpected ACK from 0x%02X", frame.src);
      }
      break;

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
  // DeviceRegistry is used on LOCAL; SensorRegistry is used on REMOTE.
  // Both are always initialized; the active tier gates which one is used at runtime.
  DeviceRegistry.begin();
  SensorRegistry.begin();

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

  Log_info("Starting LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ") tier=%s logLevel=%s loraChannel=0x%02X loraAddr=0x%02X cpuFreq=%d MHz",
           Gateway.getTierName(), LogBuffer.getLevelName(), LoRaConfig.getChannel(), LoRaConfig.getAddress(), getCpuFrequencyMhz());
  Serial.printf("\n--- LoRaIoT v1.0.0 (built " __DATE__ " " __TIME__ ") tier=%s logLevel=%s loraChannel=0x%02X loraAddr=0x%02X cpuFreq=%d MHz ---\n",
                Gateway.getTierName(), LogBuffer.getLevelName(), LoRaConfig.getChannel(), LoRaConfig.getAddress(), getCpuFrequencyMhz());

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
    Log_info("LoRa started: tier=%s loraChannel=0x%02X loraAddr=0x%02X",
             Gateway.getTierName(), LoRaConfig.getChannel(), LoRaConfig.getAddress());
    // LoRa module is configured and online; transition the status LED to READY
    statusLed.setState(LedState::READY);
  } else {
    Log_error("Gateway tier is not configured - LoRa transport not started. Call GET /reset and select a tier in the captive portal.");
    statusLed.setState(LedState::NO_DEVICE);
  }
}

// ---- applicationLoop -------------------------------------------------------

void applicationLoop() {
  // Service any pending on-demand hardware wiring checks
  diagnosticsLoop();

  // Purge stale LoRaState entries (both tiers; cheap when no entries are stale)
  LoRaState.purgeStale();

  // Drive the Describe crawl state machine (LOCAL only; no-op cost on REMOTE)
  if (strcmp(Gateway.getTierName(), "LOCAL") == 0) {
    describeCrawlerLoop();
  }

  static unsigned long lastSendTime = 0;

  // LOCAL Node: Periodically send a PING to REMOTE via the broker.
  // loraBrokerSend() returns BUSY if a REST ping is already in flight - skip that tick silently.
  if (strcmp(Gateway.getTierName(), "LOCAL") == 0) {
    unsigned long now = millis();
    if (now - lastSendTime >= PING_INTERVAL_MS) {
      lastSendTime = now;

      uint32_t seq = 0;
      const BrokerResult res = loraBrokerSend(BrokerRequestType::PING, LORA_DEFAULT_ADDR_REMOTE, nullptr, seq);
      if (res == BrokerResult::OK) {
        // Block up to LORA_REQUEST_TIMEOUT_MS for the ACK (onLoRaReceive calls loraBrokerOnPingAck).
        const BrokerResult waitRes = loraBrokerWait(LoRaTransportClass::LORA_REQUEST_TIMEOUT_MS);
        if (waitRes == BrokerResult::TIMEOUT) {
          Log_warn("PING seq=%u: no ACK within %u ms", seq, (unsigned)LoRaTransportClass::LORA_REQUEST_TIMEOUT_MS);
        }
        // On OK, PING ACK was already logged by onLoRaReceive.
      } else if (res == BrokerResult::BUSY) {
        Log_debug("applicationLoop: PING skipped - broker busy (REST ping in flight)");
      }
    }
  }

  // ESPAsyncWebServer is fully non-blocking; this is a no-op kept for symmetry.
  handleServerRequests();

  // TODO: drain WebSocket command queue and dispatch LoRa/device actions.
  // Mirror the pattern in SerialController/Application.cpp applicationLoop().
}
