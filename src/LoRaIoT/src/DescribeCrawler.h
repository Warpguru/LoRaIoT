#pragma once

#include <Arduino.h>

#include "../../lora/src/nanopb/loraiot.pb.h"

/**
 * DescribeCrawler.h - Describe crawl state machine.
 *
 * Runs on both nodes; on each node it crawls the peer's owned registry via cachedRegistry().
 *
 * Triggered whenever cachedRegistry().requestCrawl() sets crawlPending for a peer. This
 * happens in three situations:
 *   1. updatePeerMeta() detects a boot_id change or a registry_rev advance (BEACON/PING).
 *   2. A DATA or DATA_ACK frame arrives for a device not yet in the cache (Application.cpp).
 *   3. A REST ?refresh=true request is handled (RestService.cpp).
 *
 * Iterates Describe requests (device indices 1, 2, ...) until it receives STATUS_BAD_REQUEST,
 * which signals the end of the device list. Device index 0 is reserved for the node itself
 * and always returns BAD_REQUEST, so crawling starts at DESCRIBE_FIRST_DEVICE_IDX (1).
 * Stores each Device definition in the cached registry and calls markCrawlComplete() when done.
 *
 * Integration points (called from Application.cpp):
 *   - describeCrawlerLoop() must be called from applicationLoop() on every tick.
 *   - describeCrawlerOnResponse() must be called from the LoRa receive callback
 *     when a RESPONSE frame carrying a Device definition or BAD_REQUEST is decoded.
 *
 * Threading: describeCrawlerLoop() runs on the Arduino main task (Core 1 on
 * dual-core). describeCrawlerOnResponse() is called from the loraTransportTask
 * receive callback. Internal state is protected by a FreeRTOS mutex.
 */

// Timeout waiting for a Describe RESPONSE before retrying (milliseconds)
static constexpr uint32_t DESCRIBE_RESPONSE_TIMEOUT_MS = 3000;

// Maximum retries for a single Describe request before aborting the crawl
static constexpr uint8_t DESCRIBE_MAX_RETRIES = 3;

// First device index to query during a Describe crawl.
// Device index 0 is reserved for the REMOTE node itself (always BAD_REQUEST);
// SoftAP-registered devices are assigned indices starting at 1.
static constexpr uint8_t DESCRIBE_FIRST_DEVICE_IDX = 1;

/**
 * Drive the Describe crawl state machine. Call from applicationLoop() on both nodes.
 * Picks up pending crawls from cachedRegistry(), sends REQUEST(Describe) frames,
 * and advances through device indices until the crawl completes or times out.
 */
void describeCrawlerLoop();

/**
 * Called by the LoRa receive/decode layer when a RESPONSE frame arrives that
 * answers a Describe REQUEST (i.e. response.which_result == loraiot_Response_def_tag
 * or status == STATUS_BAD_REQUEST for the current in-flight Describe SEQ).
 *
 * Parameters:
 *   srcAddr  - LoRa source address of the node that sent the RESPONSE.
 *   response - Decoded loraiot_Response struct.
 */
void describeCrawlerOnResponse(uint8_t srcAddr, const loraiot_Response& response);
