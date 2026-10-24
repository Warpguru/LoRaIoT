#include "DescribeCrawler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../util/src/LogBuffer.h"
#include "../../lora/src/LoRaFrame.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../lora/src/nanopb/pb_encode.h"
#include "../../lora/src/nanopb/loraiot.pb.h"
#include "DeviceRegistry.h"
#include "LoRaConfig.h"

/**
 * DescribeCrawler.cpp - LOCAL node Describe crawl state machine.
 *
 * State machine overview:
 *
 *   IDLE ──(crawlPending detected)──► REQUESTING (device=DESCRIBE_FIRST_DEVICE_IDX)
 *   REQUESTING ──(RESPONSE received)──► REQUESTING (next device idx)
 *   REQUESTING ──(STATUS_BAD_REQUEST)──► IDLE (crawl complete)
 *   REQUESTING ──(timeout / max retries)──► IDLE (crawl aborted)
 */

// ---- Crawler state ----------------------------------------------------------

enum class CrawlState : uint8_t {
  IDLE       = 0,   // No crawl in progress
  REQUESTING = 1,   // Waiting for a Describe RESPONSE for the current device idx
};

struct CrawlerContext {
  CrawlState state        = CrawlState::IDLE;
  uint8_t    targetAddr   = 0;      // Node address being crawled
  uint8_t    deviceIdx    = 0;      // Device index of the in-flight Describe request
  uint8_t    retryCount   = 0;      // Retries for the current device index
  uint32_t   sentAtMs     = 0;      // millis() when the current REQUEST was sent
  uint32_t   inFlightSeq  = 0;      // SEQ number of the in-flight REQUEST frame
};

static CrawlerContext ctx;
static SemaphoreHandle_t crawlerMutex = nullptr;

// ---- Real REQUEST(Describe) frame sender ------------------------------------

/**
 * Encode and send a REQUEST(Describe{device=deviceIdx}) frame to targetAddr.
 * Returns the SEQ number assigned to the frame (stored in ctx.inFlightSeq so
 * describeCrawlerOnResponse() can match it against the incoming RESPONSE).
 * Bypasses LoRaBroker intentionally: the broker has a single in-flight slot used
 * by REST handlers; the crawler manages its own retry state independently.
 */
static uint16_t sendDescribeRequest(uint8_t targetAddr, uint8_t deviceIdx) {
  // Build loraiot_Request{describe:{device:deviceIdx}}
  static loraiot_Request req;
  req = loraiot_Request_init_zero;
  req.which_command           = loraiot_Request_describe_tag;
  req.command.describe.device = deviceIdx;

  static uint8_t payloadBuf[LORA_MAX_PAYLOAD_LEN];
  pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
  if (!pb_encode(&pbOut, loraiot_Request_fields, &req)) {
    Log_error("DescribeCrawler: REQUEST(Describe{device=%u}) encode failed: %s",
              deviceIdx, PB_GET_ERROR(&pbOut));
    return 0;
  }

  const uint16_t seq = loraNextSeq();
  LoRaFrame frame;
  frame.ver  = LORA_PROTOCOL_VER;
  frame.type = LoRaFrameType::REQUEST;
  frame.src  = LoRaConfig.getAddress();
  frame.dst  = targetAddr;
  frame.seq  = seq;
  frame.len  = static_cast<uint8_t>(pbOut.bytes_written);
  memcpy(frame.payload, payloadBuf, frame.len);

  if (!LoRaTransport.sendFrame(frame)) {
    Log_error("DescribeCrawler: REQUEST(Describe{device=%u}) sendFrame failed", deviceIdx);
    return 0;
  }

  Log_info("DescribeCrawler: REQUEST(Describe{device=%u}) -> node 0x%02X seq=%u",
           deviceIdx, targetAddr, seq);
  return seq;
}

// ---- Public API -------------------------------------------------------------

void describeCrawlerSchedule(uint8_t addr) {
  // DeviceRegistry.updateNodeMeta() already set crawlPending before this is called.
  // describeCrawlerLoop() picks it up on the next applicationLoop tick.
  // This function must NOT touch any mutex: it is called from loraTransportTask
  // (the receive callback), and crawlerMutex is owned by describeCrawlerLoop()
  // which runs on the applicationLoop task — taking it here would deadlock.
  Log_info("DescribeCrawler: crawl scheduled for node 0x%02X", addr);
}

void describeCrawlerLoop() {
  // Lazy-init mutex on first call (called from applicationLoop, single task)
  if (crawlerMutex == nullptr) {
    crawlerMutex = xSemaphoreCreateMutex();
    if (crawlerMutex == nullptr) {
      Log_error("DescribeCrawler: failed to create mutex");
      return;
    }
  }

  xSemaphoreTake(crawlerMutex, portMAX_DELAY);

  if (ctx.state == CrawlState::IDLE) {
    // Scan for a node with a pending crawl
    uint8_t nodeAddrs[DEVICE_REGISTRY_MAX_NODES];
    uint8_t count = DeviceRegistry.listNodes(nodeAddrs, DEVICE_REGISTRY_MAX_NODES);

    for (uint8_t i = 0; i < count; i++) {
      NodeEntry node;
      if (DeviceRegistry.getNode(nodeAddrs[i], node) && node.crawlPending) {
        ctx.state       = CrawlState::REQUESTING;
        ctx.targetAddr  = nodeAddrs[i];
        ctx.deviceIdx   = DESCRIBE_FIRST_DEVICE_IDX;
        ctx.retryCount  = 0;
        ctx.sentAtMs    = millis();
        ctx.inFlightSeq = (uint32_t)sendDescribeRequest(ctx.targetAddr, ctx.deviceIdx);
        Log_info("DescribeCrawler: starting crawl for node 0x%02X", ctx.targetAddr);
        break;
      }
    }

    xSemaphoreGive(crawlerMutex);
    return;
  }

  // --- CrawlState::REQUESTING ---
  uint32_t now = millis();
  if (now - ctx.sentAtMs < DESCRIBE_RESPONSE_TIMEOUT_MS) {
    // Still waiting for the response - nothing to do this tick
    xSemaphoreGive(crawlerMutex);
    return;
  }

  // Timeout: retry or abort
  ctx.retryCount++;
  if (ctx.retryCount >= DESCRIBE_MAX_RETRIES) {
    Log_warn("DescribeCrawler: crawl aborted for node 0x%02X (max retries reached at device idx %u)",
             ctx.targetAddr, ctx.deviceIdx);
    ctx.state = CrawlState::IDLE;
    xSemaphoreGive(crawlerMutex);
    return;
  }

  Log_warn("DescribeCrawler: timeout waiting for Describe{device=%u} response from node 0x%02X, retry %u/%u",
           ctx.deviceIdx, ctx.targetAddr, ctx.retryCount, DESCRIBE_MAX_RETRIES - 1);
  ctx.sentAtMs    = millis();
  ctx.inFlightSeq = (uint32_t)sendDescribeRequest(ctx.targetAddr, ctx.deviceIdx);

  xSemaphoreGive(crawlerMutex);
}

void describeCrawlerOnResponse(uint8_t srcAddr, const loraiot_Response& response) {
  if (crawlerMutex == nullptr) return;
  xSemaphoreTake(crawlerMutex, portMAX_DELAY);

  if (ctx.state != CrawlState::REQUESTING || ctx.targetAddr != srcAddr) {
    xSemaphoreGive(crawlerMutex);
    return;
  }

  if (response.status == loraiot_Status_STATUS_BAD_REQUEST) {
    // STATUS_BAD_REQUEST on a Describe means no more devices exist.
    // Retrieve pendingRev from the registry to record what rev we just crawled.
    NodeEntry node;
    uint32_t crawledRev = 0;
    if (DeviceRegistry.getNode(srcAddr, node)) {
      crawledRev = node.pendingRev;
    }
    Log_info("DescribeCrawler: crawl complete for node 0x%02X (%u devices found, rev=%u)",
             srcAddr, ctx.deviceIdx, crawledRev);
    DeviceRegistry.markCrawlComplete(srcAddr, crawledRev);
    ctx.state = CrawlState::IDLE;
    xSemaphoreGive(crawlerMutex);
    return;
  }

  // Successful Describe response: store the Device definition
  if (response.which_result == loraiot_Response_def_tag) {
    DeviceRegistry.storeDevice(srcAddr, response.result.def);
    ctx.deviceIdx++;
    ctx.retryCount  = 0;
    ctx.sentAtMs    = millis();
    ctx.inFlightSeq = (uint32_t)sendDescribeRequest(ctx.targetAddr, ctx.deviceIdx);
  } else {
    // Unexpected result type - skip and advance
    Log_warn("DescribeCrawler: unexpected response.which_result=%u from node 0x%02X, advancing",
             response.which_result, srcAddr);
    ctx.deviceIdx++;
    ctx.retryCount  = 0;
    ctx.sentAtMs    = millis();
    ctx.inFlightSeq = (uint32_t)sendDescribeRequest(ctx.targetAddr, ctx.deviceIdx);
  }

  xSemaphoreGive(crawlerMutex);
}
