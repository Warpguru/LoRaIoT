#include "LoRaTransport.h"

#include <Arduino.h>
#include <HardwareSerial.h>

#include "../../util/src/LogBuffer.h"
#include "../../util/src/TaskUtils.h"
#include "../../LoRaIoT/src/LoRaConfig.h"

// Hardware Serial object mapped to UART2
static HardwareSerial LoRaSerial(2);

// The actual global memory allocation for the LoRa transport layer singleton.
// All application-level files include "LoRaTransport.h" and interact with this object.
LoRaTransportClass LoRaTransport;

/**
 * Reusable helper to format and log a byte array in hexadecimal.
 */
static void logHex(const char* prefix, const uint8_t* data, size_t len) {
  char hex[128] = {0};
  size_t pos = 0;
  for (size_t i = 0; i < len && pos + 4 < sizeof(hex); i++) {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
  }
  Log_info("%s %s", prefix, hex);
}

void LoRaTransportClass::begin() {
  Log_info("Initializing LoRa transport layer infrastructure...");

  // 1. Configure E220 hardware control pins
  pinMode(PIN_M0, OUTPUT);
  pinMode(PIN_M1, OUTPUT);
  pinMode(PIN_AUX, INPUT);

  // 2. Perform E220 permanent configuration in Mode 3 (Deep Sleep / Config)
  Log_info("Configuring E220 module registers permanently in Mode 3...");
  setMode(HIGH, HIGH); // M1=1, M0=1

  // Start serial at 9600 baud (fixed for E220 config mode)
  LoRaSerial.begin(CONFIG_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  // Construct the command payload to permanently configure the E220.
  // Using explicit bitwise-OR options makes the config list completely self-documenting.
  uint8_t configCmd[CONFIG_CMD_LEN] = {
      CMD_WRITE_PERSISTENT,
      REG_START_ADDR,
      REG_WRITE_COUNT,
      ADDH_LOCAL,
      LoRaConfig.getAddress(),
      (REG0_UART_BAUD_115200 | REG0_PARITY_8N1 | REG0_AIR_DATA_RATE_62K5),
      (REG1_SUB_PACKET_200B | REG1_RSSI_NOISE_DISABLE | REG1_POWER_22DBM),
      LoRaConfig.getChannel(),
      (REG3_MODE_FIXED | REG3_RSSI_BYTE_DISABLE | REG3_LBT_DISABLE),
      CRYPT_KEY_HIGH,
      CRYPT_KEY_LOW
  };

  logHex("LoRaTransport::begin - Sent config cmd:", configCmd, CONFIG_CMD_LEN);

  // AUX must be HIGH and stable before we write config data
  unsigned long settleStart = millis();
  while (digitalRead(PIN_AUX) == LOW) {
    if (millis() - settleStart > AUX_TIMEOUT_MS) {
      Log_error("LoRaTransport::begin - Timeout waiting for AUX HIGH before writing config");
      break;
    }
    delay(POLL_INTERVAL_MS);
  }
  delay(GUARD_SETTLE_DELAY_MS);

  // Write configuration command
  LoRaSerial.write(configCmd, CONFIG_CMD_LEN);
  delay(TRANSMIT_SETTLE_DELAY_MS);

  // Read response (should echo C1 00 08 ...)
  uint8_t response[CONFIG_RESPONSE_LEN] = {0};
  size_t bytesRead = LoRaSerial.readBytes(response, CONFIG_RESPONSE_LEN);
  
  logHex("LoRaTransport::begin - Read config response:", response, bytesRead);

  if (bytesRead < CONFIG_RESPONSE_LEN || response[0] == 0xFF) {
    Log_error("LoRaTransport::begin - Error writing configuration to E220 (invalid response received)");
  } else {
    Log_info("LoRaTransport::begin - E220 configuration applied successfully.");
  }

  // 3. Return to Mode 0 (Normal operation: M1=0, M0=0) and configure normal baud rate
  setMode(LOW, LOW);
  LoRaSerial.end();
  
  // Re-open serial at 115200 baud (as configured in REG0)
  LoRaSerial.begin(NORMAL_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  // 4. Create the FreeRTOS Queue for outgoing transmissions
  txQueue = xQueueCreate(TX_QUEUE_SIZE, sizeof(LoRaTxMessage));
  if (txQueue == nullptr) {
    Log_error("LoRaTransport::begin - Failed to create TX Queue!");
  }

  // 5. Spawn background transceiver task (TASK_CREATE pins task to Core 1 on dual-core)
  TASK_CREATE(loraTransportTask, "LoRaTransTask", TASK_STACK_SIZE, this, TASK_PRIORITY, &transportTaskHandle);

  Log_info("LoRa transport layer task started.");
}

void LoRaTransportClass::sendRaw(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0 || txQueue == nullptr) return;

  // Truncate to maximum sub-packet frame capacity
  size_t transmitLen = (len > 200) ? 200 : len;

  LoRaTxMessage msg;
  memcpy(msg.data, data, transmitLen);
  msg.len = transmitLen;

  // Thread-safe post to outgoing queue; do not block indefinitely if queue is full
  if (xQueueSend(txQueue, &msg, pdMS_TO_TICKS(TASK_DELAY_MS)) != pdPASS) {
    Log_warn("LoRaTransport::sendRaw - Outgoing queue full! Frame dropped.");
  }
}

void LoRaTransportClass::setReceiveCallback(LoRaReceiveCallback cb) {
  receiveCallback = cb;
}

void LoRaTransportClass::pause() {
  if (transportTaskHandle != nullptr) {
    vTaskSuspend(transportTaskHandle);
  }
  LoRaSerial.end();
}

void LoRaTransportClass::resume() {
  LoRaSerial.begin(NORMAL_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  if (transportTaskHandle != nullptr) {
    vTaskResume(transportTaskHandle);
  }
}

void LoRaTransportClass::sendRawInternal(const uint8_t* data, size_t len) {
  // 1. Mandatory AUX pin check: poll AUX HIGH (wait until E220 is idle)
  unsigned long timeoutStart = millis();
  while (digitalRead(PIN_AUX) == LOW) {
    if (millis() - timeoutStart > AUX_TIMEOUT_MS) {
      Log_warn("LoRaTransport::sendRawInternal - Timeout waiting for AUX HIGH (idle) before write");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }

  // 2. Prepend the Fixed Mode routing header: [ADDH][ADDL][CHAN]
  const uint8_t targetAddh = ADDH_LOCAL; // always 0x00 in this layout
  const uint8_t targetAddl = (LoRaConfig.getAddress() == ADDR_LOCAL) ? ADDR_REMOTE : ADDR_LOCAL;
  const uint8_t targetChan = LoRaConfig.getChannel();

  // Print diagnostics for transmission routing
  if (len > 0) {
    char dbgPrefix[64];
    snprintf(dbgPrefix, sizeof(dbgPrefix), "LoRa TX: target=[0x%02X, 0x%02X] ch=0x%02X payload:", targetAddh, targetAddl, targetChan);
    logHex(dbgPrefix, data, len);
  }

  // Write routing header and payload atomically
  LoRaSerial.write(targetAddh);
  LoRaSerial.write(targetAddl);
  LoRaSerial.write(targetChan);
  LoRaSerial.write(data, len);

  // 3. Wait for AUX to go LOW (E220 begins RF transmission from buffer)
  timeoutStart = millis();
  while (digitalRead(PIN_AUX) == HIGH) {
    if (millis() - timeoutStart > AUX_TRANSMIT_DETECT_TIMEOUT_MS) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }

  // 4. Wait for AUX to go HIGH again (E220 finished buffer transmission)
  timeoutStart = millis();
  while (digitalRead(PIN_AUX) == LOW) {
    if (millis() - timeoutStart > AUX_TRANSMIT_COMPLETE_TIMEOUT_MS) {
      Log_warn("LoRaTransport::sendRawInternal - Timeout waiting for AUX HIGH (transmit complete)");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }

  // 5. Add 50ms guard delay (required E220 air-transmission finish time)
  vTaskDelay(pdMS_TO_TICKS(GUARD_DELAY_MS));
}

void LoRaTransportClass::setMode(uint8_t m0, uint8_t m1) {
  digitalWrite(PIN_M0, m0);
  digitalWrite(PIN_M1, m1);

  // Wait for AUX to rise to HIGH, ensuring E220 completes transition
  unsigned long timeoutStart = millis();
  while (digitalRead(PIN_AUX) == LOW) {
    if (millis() - timeoutStart > AUX_TIMEOUT_MS) {
      break;
    }
    delay(POLL_INTERVAL_MS);
  }
  delay(GUARD_SETTLE_DELAY_MS); // AUX must stay HIGH >= 2 ms before UART data is accepted
}

void LoRaTransportClass::loraTransportTask(void* param) {
  LoRaTransportClass* self = static_cast<LoRaTransportClass*>(param);
  Log_info("LoRa background transport task loop initialized.");

  static uint8_t rxBuffer[256];
  static size_t rxIndex = 0;

  while (true) {
    // 1. Process RX (read incoming bytes on UART2)
    while (LoRaSerial.available() > 0) {
      int c = LoRaSerial.read();
      if (c >= 0) {
        Log_debug("LoRa RX: byte 0x%02X (%c)", c, (c >= CHAR_PRINTABLE_MIN && c <= CHAR_PRINTABLE_MAX) ? static_cast<char>(c) : '.');
        if (rxIndex < sizeof(rxBuffer) - 1) {
          rxBuffer[rxIndex++] = static_cast<uint8_t>(c);
          // For Milestone 0 raw transceiving: frames are plain-text ending with '\n'
          if (c == '\n') {
            rxBuffer[rxIndex] = '\0';
            if (self->receiveCallback != nullptr) {
              self->receiveCallback(rxBuffer, rxIndex);
            }
            rxIndex = 0;
          }
        } else {
          // Prevent overflow: reset index on full buffer with no marker
          rxIndex = 0;
        }
      }
    }

    // 2. Process TX (drain outgoing queue and transmit)
    if (self->txQueue != nullptr) {
      LoRaTxMessage txMsg;
      if (xQueueReceive(self->txQueue, &txMsg, 0) == pdPASS) {
        self->sendRawInternal(txMsg.data, txMsg.len);
      }
    }

    // Yield control to let other core tasks run
    vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
  }
}
