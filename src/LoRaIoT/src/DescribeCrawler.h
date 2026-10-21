#pragma once

#include <Arduino.h>

#include "../../lora/src/nanopb/loraiot.pb.h"

/**
 * DescribeCrawler.h - LOCAL node Describe crawl state machine.
 *
 * Triggered whenever DeviceRegistry.updateNodeMeta() returns true (boot_id
 * changed or registry_rev advanced on a remote node). Iterates Describe
 * requests (device indices 0, 1, 2, ...) until it receives STATUS_BAD_REQUEST,
 * which signals the end of the device list. Stores each Device definition in
 * DeviceRegistry and calls markCrawlComplete() when finished.
 *
 * Integration points (called from Application.cpp):
 *   - describeCrawlerLoop() must be called from applicationLoop() on every tick.
 *   - describeCrawlerOnResponse() must be called from the LoRa receive callback
 *     when a RESPONSE frame carrying a Device definition or BAD_REQUEST is decoded.
 *   - describeCrawlerSchedule() may be called to force a crawl for a node.
 *
 * Only exists on LOCAL.
 *
 * Threading: describeCrawlerLoop() runs on the Arduino main task (Core 1 on
 * dual-core). describeCrawlerOnResponse() is called from the loraTransportTask
 * receive callback. Internal state is protected by a FreeRTOS mutex.
 */

// Timeout waiting for a Describe RESPONSE before retrying (milliseconds)
static constexpr uint32_t DESCRIBE_RESPONSE_TIMEOUT_MS = 3000;

// Maximum retries for a single Describe request before aborting the crawl
static constexpr uint8_t DESCRIBE_MAX_RETRIES = 3;

/**
 * Schedule a Describe crawl for the given node address.
 * Safe to call from any task; sets the crawlPending flag in DeviceRegistry.
 */
void describeCrawlerSchedule(uint8_t addr);

/**
 * Drive the Describe crawl state machine. Call from applicationLoop() on LOCAL.
 * Picks up pending crawls from DeviceRegistry, sends REQUEST(Describe) frames,
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
