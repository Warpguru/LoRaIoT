#include "DescribeCrawler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../util/src/LogBuffer.h"
#include "DeviceRegistry.h"

/**
 * DescribeCrawler.cpp - LOCAL node Describe crawl state machine.
 *
 * State machine overview:
 *
 *   IDLE ──(crawlPending detected)──► REQUESTING
 *   REQUESTING ──(RESPONSE received)──► REQUESTING (next device idx)
 *   REQUESTING ──(STATUS_BAD_REQUEST)──► IDLE (crawl complete)
 *   REQUESTING ──(timeout / max retries)──► IDLE (crawl aborted)
 *
 * The actual REQUEST(Describe) frame encoding and transmission is delegated to
 * sendDescribeRequest() below. In Phase 1 this is a no-op stub; the real encoder
 * will be filled in Phase 2 once the frame protocol layer is wired.
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

// ---- Forward declaration of the (Phase 2) frame-send stub ------------------

/**
 * Send a REQUEST(Describe{device}) frame to targetAddr.
 * Returns the SEQ number assigned to this REQUEST (used to match the RESPONSE).
 *
 * Phase 1: stub - logs the intent and returns a dummy SEQ of 0.
 * Phase 2: replace with real protobuf encode + LoRaTransport.sendRaw() call.
 */
static uint32_t sendDescribeRequest(uint8_t targetAddr, uint8_t deviceIdx) {
  Log_info("DescribeCrawler: REQUEST(Describe{device=%u}) -> node 0x%02X [stub - Phase 2]",
           deviceIdx, targetAddr);
  // TODO(Phase 2): encode loraiot_Request{describe:{device:deviceIdx}}, build frame,
  //                call LoRaTransport.sendRaw(), return real SEQ.
  return 0;
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
        ctx.deviceIdx   = 0;
        ctx.retryCount  = 0;
        ctx.sentAtMs    = millis();
        ctx.inFlightSeq = sendDescribeRequest(ctx.targetAddr, ctx.deviceIdx);
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
  ctx.inFlightSeq = sendDescribeRequest(ctx.targetAddr, ctx.deviceIdx);

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
    ctx.inFlightSeq = sendDescribeRequest(ctx.targetAddr, ctx.deviceIdx);
  } else {
    // Unexpected result type - skip and advance
    Log_warn("DescribeCrawler: unexpected response.which_result=%u from node 0x%02X, advancing",
             response.which_result, srcAddr);
    ctx.deviceIdx++;
    ctx.retryCount  = 0;
    ctx.sentAtMs    = millis();
    ctx.inFlightSeq = sendDescribeRequest(ctx.targetAddr, ctx.deviceIdx);
  }

  xSemaphoreGive(crawlerMutex);
}
