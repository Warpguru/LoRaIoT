#include "PingTask.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "../../diagnostics/src/TaskRegistry.h"
#include "../../util/src/LogBuffer.h"
#include "../../util/src/TaskUtils.h"
#include "Application.h"
#include "../../registry/src/PeerRegistry.h"
#include "LoRaBroker.h"
#include "LoRaConfig.h"

/**
 * PingTask.cpp - link-liveness task, identical on LOCAL and REMOTE. See PingTask.h for the
 * overall design. The only tier difference is data, not code: the peers it watches live in
 * cachedRegistry() (SensorRegistry on LOCAL, ControlRegistry on REMOTE), and only LOCAL ever
 * receives BEACONs, so only LOCAL's beacon queue ever fills.
 *
 * The task has two triggers, both handled in one loop so that only one PING is ever
 * in flight from this task (the broker has a single slot anyway):
 *
 *   - a node address arrives on beaconQueue  -> answer the BEACON with a PING
 *   - the queue wait times out (every PING_SWEEP_INTERVAL_MS) -> look for idle nodes
 *
 * Why a queue and not a direct call from the receive callback: pinging blocks for up
 * to PING_ATTEMPT_TIMEOUT_MS waiting for the ACK, and that ACK is delivered by the very
 * task (loraTransportTask) that runs the receive callback. Blocking there would
 * deadlock the receive path, so the callback only posts the address and returns.
 */

// ---- State ------------------------------------------------------------------

// Beacon notifications from loraTransportTask. Items are node addresses.
static QueueHandle_t beaconQueue = nullptr;

// Control change notifications from RestService. Items are node addresses.
static QueueHandle_t controlChangeQueue = nullptr;

// When each node was last pinged by this task, to enforce PING_MIN_GAP_MS.
// Only the ping task touches this table, so it needs no lock. It has one slot per
// tracked node; a slot with addr == 0 (the invalid address) is unused.
struct PingStamp {
    uint8_t  addr;
    uint32_t pingedMs;
};
static PingStamp pingStamps[LORA_MAX_PEERS];

// ---- Static helpers ---------------------------------------------------------

/**
 * Return the stamp slot for addr. Reuses the slot already assigned to addr, then an
 * unused slot, and finally the slot pinged longest ago. The last case only happens
 * after cachedRegistry() evicted an offline node to make room for a new address.
 */
static PingStamp& stampFor(uint8_t addr) {
    PingStamp* oldest = &pingStamps[0];
    for (uint8_t i = 0; i < LORA_MAX_PEERS; i++) {
        if (pingStamps[i].addr == addr) {
            return pingStamps[i];
        }
        if (pingStamps[i].addr == 0) {
            pingStamps[i].addr     = addr;
            pingStamps[i].pingedMs = 0;
            return pingStamps[i];
        }
        if ((millis() - pingStamps[i].pingedMs) > (millis() - oldest->pingedMs)) {
            oldest = &pingStamps[i];
        }
    }
    oldest->addr     = addr;
    oldest->pingedMs = 0;
    return *oldest;
}

/**
 * Send one unicast PING to addr and wait for its ACK.
 *
 * Returns OK on ACK, TIMEOUT when no ACK came, BUSY when the broker slot was taken
 * (for example by a REST request) so the node was never actually tested. BUSY must
 * not be treated as a failure, otherwise a busy broker would mark live nodes offline.
 */
static BrokerResult pingOnce(uint8_t addr) {
    uint32_t seq = 0;
    BrokerResult result = loraBrokerSend(BrokerRequestType::PING, addr, nullptr, seq);
    if (result != BrokerResult::OK) {
        return result;
    }
    // Remember when we pinged, whatever the outcome, so a flood of beacons cannot
    // turn into a flood of pings.
    stampFor(addr).pingedMs = millis();
    return loraBrokerWait(PING_ATTEMPT_TIMEOUT_MS);
}

/**
 * Answer a BEACON from addr with a single PING, unless this node was pinged less than
 * PING_MIN_GAP_MS ago. A single attempt is enough here: a lost reply only delays the
 * REMOTE's switch to the slow beacon rate until its next beacon.
 */
static void replyToBeacon(uint8_t addr) {
    PingStamp& stamp = stampFor(addr);
    if (stamp.pingedMs != 0 && (millis() - stamp.pingedMs) < PING_MIN_GAP_MS) {
        Log_debug("PingTask: skip beacon reply to 0x%02X (pinged %ums ago)",
                  addr, millis() - stamp.pingedMs);
        return;
    }
    BrokerResult result = pingOnce(addr);
    Log_debug("PingTask: beacon reply PING to 0x%02X -> %s", addr,
              result == BrokerResult::OK ? "ACK" : (result == BrokerResult::BUSY ? "busy" : "no ACK"));
}

/**
 * Answer a control change notification for addr with an immediate PING without
 * enforcing PING_MIN_GAP_MS.
 */
static void replyToControlChange(uint8_t addr) {
    BrokerResult result = pingOnce(addr);
    Log_debug("PingTask: control change PING to 0x%02X -> %s", addr,
              result == BrokerResult::OK ? "ACK" : (result == BrokerResult::BUSY ? "busy" : "no ACK"));
}

/**
 * Decide whether an idle node is really gone: up to PING_OFFLINE_ATTEMPTS PINGs with
 * PING_ATTEMPT_GAP_MS between failures. One ACK proves the node is alive (the receive
 * path already refreshed lastSeenMs via cachedRegistry().markSeen), so we stop at once.
 * Only when every attempt got a real timeout is the node marked offline.
 */
static void checkIdleNode(uint8_t addr) {
    Log_info("PingTask: node 0x%02X idle, checking link", addr);

    uint8_t timeouts    = 0;
    uint8_t busyRetries = 0;

    while (timeouts < PING_OFFLINE_ATTEMPTS) {
        BrokerResult result = pingOnce(addr);

        if (result == BrokerResult::OK) {
            Log_info("PingTask: node 0x%02X answered, still online", addr);
            return;
        }

        if (result == BrokerResult::BUSY || result == BrokerResult::ENCODE_ERR) {
            // The node was not tested. Retry a few times, then give up without a verdict:
            // declaring a node dead because WE could not send would be wrong.
            busyRetries++;
            if (busyRetries >= PING_OFFLINE_ATTEMPTS) {
                Log_warn("PingTask: broker unavailable, node 0x%02X check postponed", addr);
                return;
            }
        } else {
            timeouts++;
        }

        if (timeouts < PING_OFFLINE_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(PING_ATTEMPT_GAP_MS));
        }
    }

    cachedRegistry().markOffline(addr);
    Log_warn("PingTask: node 0x%02X did not answer %u PINGs - marked offline",
             addr, PING_OFFLINE_ATTEMPTS);
}

/**
 * Check every online node for idleness. Uses getLiveness(), which does not copy the
 * ~1.4 KB PeerEntry, because this task only has a 4 KB stack.
 */
static void sweepIdleNodes() {
    uint8_t addrs[LORA_MAX_PEERS];
    const uint8_t count = cachedRegistry().listPeers(addrs, LORA_MAX_PEERS);

    for (uint8_t i = 0; i < count; i++) {
        uint32_t lastSeenMs = 0;
        bool     online     = false;
        if (!cachedRegistry().getLiveness(addrs[i], lastSeenMs, online)) {
            continue;
        }
        // Offline nodes are not pinged: it would only waste air time. The next BEACON
        // from a returning node revives it via markSeen().
        if (online && (millis() - lastSeenMs) > PING_IDLE_THRESHOLD_MS) {
            checkIdleNode(addrs[i]);
        }
    }
}

// ---- FreeRTOS task ----------------------------------------------------------

static void pingTask(void* /*param*/) {
    // Same radio gate as BeaconTask: nothing may be sent before the boot diagnostics
    // have finished with the E220.
    while (!loraIsReady()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    Log_info("PingTask: LoRa gate open - starting keep-alive loop airRate=%s",
             LoRaTransportClass::airRateName(LoRaConfig.getAirRate()));

    for (;;) {
        uint8_t addr = 0;
        // Priority 1: process any pending control-change notifications first
        while (xQueueReceive(controlChangeQueue, &addr, 0) == pdTRUE) {
            replyToControlChange(addr);
        }

        // Priority 2: wait for a beacon notification (doubles as sweep timer)
        if (xQueueReceive(beaconQueue, &addr, pdMS_TO_TICKS(PING_SWEEP_INTERVAL_MS)) == pdTRUE) {
            replyToBeacon(addr);
        }

        // Process any control change that arrived during beacon reply or queue wait
        while (xQueueReceive(controlChangeQueue, &addr, 0) == pdTRUE) {
            replyToControlChange(addr);
        }

        sweepIdleNodes();
    }
}

// ---- Public interface -------------------------------------------------------

void pingTaskBegin() {
    beaconQueue        = xQueueCreate(PING_QUEUE_LENGTH, sizeof(uint8_t));
    controlChangeQueue = xQueueCreate(PING_QUEUE_LENGTH, sizeof(uint8_t));
    if (beaconQueue == nullptr || controlChangeQueue == nullptr) {
        Log_error("PingTask: queue creation failed - keep-alive disabled");
        return;
    }
    TaskHandle_t handle = nullptr;
    TASK_CREATE(pingTask, "Ping", PING_TASK_STACK_SIZE, nullptr, PING_TASK_PRIORITY, &handle);
    TaskRegistry.add("Ping", handle, PING_TASK_STACK_SIZE);
    Log_info("PingTask: started (idle=%ums attempts=%u)", PING_IDLE_THRESHOLD_MS, PING_OFFLINE_ATTEMPTS);
}

void pingTaskNotifyBeacon(uint8_t addr) {
    if (beaconQueue == nullptr) {
        return;
    }
    // Timeout 0: never block the receive path. A full queue drops this notification;
    // the node beacons again, so the only cost is a later switch to the slow rate.
    xQueueSend(beaconQueue, &addr, 0);
}

void pingTaskNotifyControlChange(uint8_t addr) {
    if (controlChangeQueue == nullptr) {
        return;
    }
    // Timeout 0: never block the HTTP handler.
    xQueueSend(controlChangeQueue, &addr, 0);
}
