#include "BeaconTask.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <esp_random.h>

#include "../../diagnostics/src/TaskRegistry.h"
#include "../../util/src/LogBuffer.h"
#include "../../util/src/TaskUtils.h"
#include "Application.h"
#include "LoRaConfig.h"
#include "LoRaBroker.h"
#include "SensorRegistry.h"
#include "../../lora/src/LoRaFrame.h"
#include "../../lora/src/LoRaTransport.h"
#include "../../lora/src/nanopb/pb_encode.h"
#include "../../lora/src/nanopb/loraiot.pb.h"

/**
 * BeaconTask.cpp - REMOTE beacon state machine implementation.
 *
 * Runs a single FreeRTOS task that wakes periodically, builds and sends one
 * BEACON frame, then sleeps for an interval determined by the current state:
 *
 *   UNDISCOVERED  - no frame from LOCAL seen, or last contact > BEACON_REVERT_TIMEOUT_MS ago
 *   DISCOVERED    - a frame from LOCAL was received within BEACON_REVERT_TIMEOUT_MS
 *
 * beaconTaskNotifyDiscovered() is called from loraTransportTask (the receive
 * callback) and performs a single 32-bit aligned write to lastLocalContactMs,
 * which is safe without a mutex on ESP32.
 */

// ---- State ------------------------------------------------------------------

// Firmware version encoded as 0xMMmmpp (major=1, minor=0, patch=0).
static constexpr uint32_t BEACON_FW_VERSION = 0x010000;

// Timestamp of the last frame received from LOCAL (millis()). 0 = never seen.
// Written by beaconTaskNotifyDiscovered() from loraTransportTask.
// Read by beaconTask(). Single 32-bit aligned write on ESP32 - safe without mutex.
static volatile uint32_t lastLocalContactMs = 0;

// ---- Public: notify ---------------------------------------------------------

void beaconTaskNotifyDiscovered() {
    lastLocalContactMs = millis();
}

// ---- Static helpers ---------------------------------------------------------

/**
 * Build and send one BEACON frame. Errors are logged; no return value.
 */
static void sendBeacon() {
    // 1. Build Beacon payload.
    loraiot_Beacon beacon = loraiot_Beacon_init_zero;
    beacon.boot_id          = loraBrokerBootId();
    beacon.has_fw_version   = true;
    beacon.fw_version       = BEACON_FW_VERSION;
    beacon.has_uptime_s     = true;
    beacon.uptime_s         = (uint32_t)(millis() / 1000UL);
    beacon.has_registry_rev = true;
    beacon.registry_rev     = SensorRegistry.getRegistryRev();

    // 2. Encode into a stack buffer.
    uint8_t payloadBuf[loraiot_Beacon_size];
    pb_ostream_t pbOut = pb_ostream_from_buffer(payloadBuf, sizeof(payloadBuf));
    if (!pb_encode(&pbOut, loraiot_Beacon_fields, &beacon)) {
        Log_error("BeaconTask: pb_encode failed: %s", PB_GET_ERROR(&pbOut));
        return;
    }

    // 3. Build LoRaFrame.
    LoRaFrame frame;
    frame.ver  = LORA_PROTOCOL_VER;
    frame.type = LoRaFrameType::BEACON;
    frame.src  = LoRaConfig.getAddress();
    frame.dst  = LORA_ADDR_BROADCAST;
    frame.seq  = loraNextSeq();
    frame.len  = (uint8_t)pbOut.bytes_written;
    memcpy(frame.payload, payloadBuf, frame.len);

    // 4. Enqueue for transmission.
    if (LoRaTransport.sendFrame(frame)) {
        Log_info("BeaconTask: BEACON sent seq=%u uptime=%us registry_rev=%u",
                 frame.seq, beacon.uptime_s, beacon.registry_rev);
    } else {
        Log_error("BeaconTask: sendFrame failed for seq=%u", frame.seq);
    }
}

// ---- FreeRTOS task ----------------------------------------------------------

static void beaconTask(void* /*param*/) {
    // Wait until the boot diagnostics have completed and the radio gate is open.
    // loraIsReady() becomes true after the first diagnosticsLoop() call returns.
    // Polling with a short sleep is correct: this task has nothing to do until
    // the radio is safe to use, and the poll cost is negligible.
    while (!loraIsReady()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    Log_info("BeaconTask: LoRa gate open - starting beacon loop");

    for (;;) {
        // 1. Determine current state.
        const uint32_t lastContact = lastLocalContactMs;
        const bool discovered = (lastContact != 0) &&
                                ((millis() - lastContact) < BEACON_REVERT_TIMEOUT_MS);

        // 2. Send one BEACON frame.
        sendBeacon();

        // 3. Compute interval: base + random jitter in [0, BEACON_JITTER_MAX_MS].
        const uint32_t base     = discovered ? BEACON_INTERVAL_DISCOVERED_MS
                                             : BEACON_INTERVAL_UNDISCOVERED_MS;
        const uint32_t interval = base + (esp_random() % (BEACON_JITTER_MAX_MS + 1));

        // 4. Log state and sleep duration.
        Log_debug("BeaconTask: state=%s next=%ums",
                  discovered ? "DISCOVERED" : "UNDISCOVERED", interval);

        // 5. Sleep until the next beacon.
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

// ---- Public: begin ----------------------------------------------------------

void beaconTaskBegin() {
    TaskHandle_t handle = nullptr;
    TASK_CREATE(beaconTask, "Beacon", BEACON_TASK_STACK_SIZE, nullptr, BEACON_TASK_PRIORITY, &handle);
    TaskRegistry.add("Beacon", handle, BEACON_TASK_STACK_SIZE);
    Log_info("BeaconTask: started (UNDISCOVERED, interval=%ums)", BEACON_INTERVAL_UNDISCOVERED_MS);
}
