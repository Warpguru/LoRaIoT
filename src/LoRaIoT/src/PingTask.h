#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include "../../lora/src/LoRaTransport.h"

/**
 * PingTask.h - link-liveness task (FreeRTOS), the same code on LOCAL and REMOTE.
 *
 * REMOTE nodes already prove they are
 * alive by beaconing (every ~60 s once discovered), so LOCAL only has to pay for
 * a PING when that proof is missing or when a REMOTE needs to learn it was
 * discovered:
 *
 *   1. Beacon reply   - every BEACON is answered with one unicast PING (at most
 *                       one per PING_MIN_GAP_MS per node). Any frame from LOCAL
 *                       moves a REMOTE from the fast UNDISCOVERED beacon rate to
 *                       the slow DISCOVERED one (see BeaconTask.h).
 *   2. Keep-alive     - a node that has been silent for PING_IDLE_THRESHOLD_MS
 *                       (longer than one beacon interval plus jitter, so at least
 *                       one beacon was missed) is pinged up to PING_OFFLINE_ATTEMPTS
 *                       times. If every attempt fails the node is marked offline
 *                       in cachedRegistry(). Offline nodes are not pinged again;
 *                       any received frame revives them.
 *
 * Both tiers run the keep-alive (item 2) so each side can fail safe on its own:
 * LOCAL watches REMOTEs in SensorRegistry, REMOTE watches LOCAL in ControlRegistry.
 * Item 1 only ever triggers on LOCAL, because only REMOTE nodes send BEACONs. The
 * peer answers every PING with an ACK (see onLoRaReceive()). On REMOTE, LOCAL's
 * beacon-reply PINGs arrive about once per beacon interval, so a healthy LOCAL
 * is never idle for PING_IDLE_THRESHOLD_MS.
 *
 * All pings go through the LoRaBroker (single in-flight slot), so the task only
 * ever blocks itself while waiting; reception keeps running in loraTransportTask.
 *
 * Call pingTaskBegin() after loraBrokerBegin() and the registries' begin().
 */

// ---- Timing and sizing constants --------------------------------------------

// Silence after which a node is checked with explicit PINGs (ms).
// Must stay above BEACON_INTERVAL_DISCOVERED_MS (60 s) + BEACON_JITTER_MAX_MS (1 s),
// otherwise a healthy node would be pinged between two regular beacons.
static constexpr uint32_t PING_IDLE_THRESHOLD_MS = 90000;

// How often the task re-evaluates all nodes for idleness (ms). Also the wait
// timeout on the beacon queue, so the sweep runs even when no beacon arrives.
static constexpr uint32_t PING_SWEEP_INTERVAL_MS = 1000;

// Minimum gap between two beacon-triggered PINGs to the same node (ms).
// Protects the half-duplex link from a node that beacons at the fast 5 s rate.
static constexpr uint32_t PING_MIN_GAP_MS = 5000;

// Number of PING attempts before an idle node is declared offline.
static constexpr uint8_t PING_OFFLINE_ATTEMPTS = 3;

// How long to wait for the ACK of one PING attempt (ms). Defaults to the protocol
// request timeout; a separate constant so the offline check can be tuned alone.
static constexpr uint32_t PING_ATTEMPT_TIMEOUT_MS = LoRaTransportClass::LORA_REQUEST_TIMEOUT_MS;

// Pause between two failed PING attempts (ms). Gives a node that was busy
// transmitting a chance to finish before the next try.
static constexpr uint32_t PING_ATTEMPT_GAP_MS = 1000;

// Capacity of the beacon notification queue. A full queue simply drops the
// notification: the node beacons again later, so nothing is lost permanently.
static constexpr uint8_t PING_QUEUE_LENGTH = 4;

// FreeRTOS task stack size (bytes) and priority.
// Priority 1 keeps it below loraTransportTask (priority 2) so pinging never starves RX.
static constexpr uint32_t PING_TASK_STACK_SIZE = 4096;
static constexpr UBaseType_t PING_TASK_PRIORITY = 1;

// ---- Public interface -------------------------------------------------------

/**
 * Start the ping task. Call once from applicationSetup() on both tiers.
 */
void pingTaskBegin();

/**
 * Tell the ping task that a BEACON arrived from addr, so it answers with a PING (LOCAL only,
 * REMOTE never receives BEACONs).
 * Never blocks; safe to call from loraTransportTask (the receive callback).
 */
void pingTaskNotifyBeacon(uint8_t addr);

/**
 * Tell the ping task that a control change occurred for addr, so it sends an immediate PING.
 * Does not enforce PING_MIN_GAP_MS. Never blocks; safe to call from HTTP handlers.
 */
void pingTaskNotifyControlChange(uint8_t addr);
