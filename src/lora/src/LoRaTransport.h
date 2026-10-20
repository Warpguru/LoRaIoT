#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

/**
 * LoRaTransport.h - Minimal E220 raw transport layer interface.
 *
 * ARDUINO SINGLETON PATTERN DOCUMENTATION:
 * - 'LoRaTransportClass' is the underlying class type. A suffix is used here
 *   because C++ prevents a global variable from having the exact same name
 *   as its class type in the same scope (name shadowing collision).
 * - 'LoRaTransport' is the single globally exported object instance.
 * - Application code should always interact with the 'LoRaTransport' instance.
 *
 * Implements the E220 transceiver interface, FreeRTOS background task,
 * and thread-safe outgoing queue. All low-level E220 logic, pin handling,
 * and timing rules are encapsulated inside this library.
 */

typedef void (*LoRaReceiveCallback)(const uint8_t* data, size_t len);

// Max E220 sub-packet / UART write size (REG1_SUB_PACKET_200B)
static constexpr size_t LORA_MAX_FRAME_LEN = 200;

struct LoRaTxMessage {
  uint8_t data[LORA_MAX_FRAME_LEN];
  size_t len;
};

class LoRaTransportClass {
 public:
  // Canonical E220 Hardware Pin Mapping (from AGENTS.md)
  static constexpr uint8_t PIN_M0  = 4;    // Output: mode control
  static constexpr uint8_t PIN_M1  = 2;    // Output: mode control
  static constexpr uint8_t PIN_AUX = 15;   // Input: HIGH = module ready
  static constexpr uint8_t PIN_RX  = 16;   // ESP32 RX2 <- E220 TXD
  static constexpr uint8_t PIN_TX  = 17;   // ESP32 TX2 -> E220 RXD

  // E220 Register Command Constants
  static constexpr uint8_t CMD_WRITE_PERSISTENT              = 0xC0;   // Permanent configuration write (saved to flash)
  static constexpr uint8_t CMD_READ                         = 0xC1;   // Register read command
  static constexpr uint8_t CMD_WRITE_TEMP                   = 0xC2;   // Temporary configuration write (not saved to flash)
  static constexpr uint8_t REG_START_ADDR                    = 0x00;   // Start register address
  static constexpr uint8_t REG_WRITE_COUNT                   = 0x08;   // Number of registers to write
  static constexpr uint8_t CRYPT_KEY_HIGH                    = 0x00;   // Default high-byte encryption key
  static constexpr uint8_t CRYPT_KEY_LOW                     = 0x00;   // Default low-byte encryption key
  static constexpr uint8_t ADDH                              = 0x00;   // Module address high byte (always 0; the node address is ADDL, see LoRaConfig.h)
  static constexpr uint8_t ERROR_REPLY_BYTE                  = 0xFF;   // First byte of the E220 "FF FF FF" wrong-format reply
  static constexpr size_t  ERROR_REPLY_LEN                   = 3;      // Length of the E220 "FF FF FF" wrong-format reply
  static constexpr size_t  CONFIG_HEADER_LEN                 = 3;      // C0/C1/C2 + StartAddr + Length prefix of a config command or reply
  static constexpr size_t  HEX_CHARS_PER_BYTE                = 3;      // "%02X " per byte in hex log output
  static constexpr size_t  HEX_ENTRY_SIZE                    = HEX_CHARS_PER_BYTE + 1; // One hex entry plus string terminator
  static constexpr uint8_t CHAR_PRINTABLE_MIN                = 32;     // ASCII printable space boundary
  static constexpr uint8_t CHAR_PRINTABLE_MAX                = 126;    // ASCII printable tilde boundary

  // --- E220 Register 0 (REG0) Bitfield Definitions ---
  // UART Baud Rate (Bits 7-5)
  static constexpr uint8_t REG0_UART_BAUD_1200               = 0x00;   // Binary: 00000000
  static constexpr uint8_t REG0_UART_BAUD_2400               = 0x20;   // Binary: 00100000
  static constexpr uint8_t REG0_UART_BAUD_4800               = 0x40;   // Binary: 01000000
  static constexpr uint8_t REG0_UART_BAUD_9600               = 0x60;   // Binary: 01100000
  static constexpr uint8_t REG0_UART_BAUD_19200              = 0x80;   // Binary: 10000000
  static constexpr uint8_t REG0_UART_BAUD_38400              = 0xA0;   // Binary: 10100000
  static constexpr uint8_t REG0_UART_BAUD_57600              = 0xC0;   // Binary: 11000000
  static constexpr uint8_t REG0_UART_BAUD_115200             = 0xE0;   // Binary: 11100000

  // UART Parity (Bits 4-3)
  static constexpr uint8_t REG0_PARITY_8N1                  = 0x00;   // Binary: 00000000
  static constexpr uint8_t REG0_PARITY_8O1                  = 0x08;   // Binary: 00001000
  static constexpr uint8_t REG0_PARITY_8E1                  = 0x10;   // Binary: 00010000
  static constexpr uint8_t REG0_PARITY_8N1_ALT              = 0x18;   // Binary: 00011000

  // Air Data Rate (Bits 2-0) - E220-400T22D manual section 7.2 (000, 001 and 010 are all 2.4k)
  static constexpr uint8_t REG0_AIR_DATA_RATE_2K4            = 0x02;   // Binary: 00000010 (manual default)
  static constexpr uint8_t REG0_AIR_DATA_RATE_4K8            = 0x03;   // Binary: 00000011
  static constexpr uint8_t REG0_AIR_DATA_RATE_9K6            = 0x04;   // Binary: 00000100
  static constexpr uint8_t REG0_AIR_DATA_RATE_19K2           = 0x05;   // Binary: 00000101
  static constexpr uint8_t REG0_AIR_DATA_RATE_38K4           = 0x06;   // Binary: 00000110
  static constexpr uint8_t REG0_AIR_DATA_RATE_62K5           = 0x07;   // Binary: 00000111

  // --- E220 Register 1 (REG1) Bitfield Definitions ---
  // Sub-packet size (Bits 7-6)
  static constexpr uint8_t REG1_SUB_PACKET_200B              = 0x00;   // Binary: 00000000
  static constexpr uint8_t REG1_SUB_PACKET_128B              = 0x40;   // Binary: 01000000
  static constexpr uint8_t REG1_SUB_PACKET_64B               = 0x80;   // Binary: 10000000
  static constexpr uint8_t REG1_SUB_PACKET_32B               = 0xC0;   // Binary: 11000000

  // RSSI Noise (Bit 5)
  static constexpr uint8_t REG1_RSSI_NOISE_DISABLE           = 0x00;   // Binary: 00000000
  static constexpr uint8_t REG1_RSSI_NOISE_ENABLE            = 0x20;   // Binary: 00100000

  // Transmission Power (Bits 1-0)
  static constexpr uint8_t REG1_POWER_22DBM                  = 0x00;   // Binary: 00000000
  static constexpr uint8_t REG1_POWER_17DBM                  = 0x01;   // Binary: 00000001
  static constexpr uint8_t REG1_POWER_13DBM                  = 0x02;   // Binary: 00000010
  static constexpr uint8_t REG1_POWER_10DBM                  = 0x03;   // Binary: 00000011

  // --- E220 Register 3 (REG3) Bitfield Definitions ---
  // RSSI Byte Enable (Bit 7)
  static constexpr uint8_t REG3_RSSI_BYTE_DISABLE            = 0x00;   // Binary: 00000000
  static constexpr uint8_t REG3_RSSI_BYTE_ENABLE             = 0x80;   // Binary: 10000000

  // Transmission Mode / Method (Bit 6)
  static constexpr uint8_t REG3_MODE_TRANSPARENT             = 0x00;   // Binary: 00000000
  static constexpr uint8_t REG3_MODE_FIXED                   = 0x40;   // Binary: 01000000

  // LBT Enable (Bit 4; bit 5 is reserved)
  static constexpr uint8_t REG3_LBT_DISABLE                  = 0x00;   // Binary: 00000000
  static constexpr uint8_t REG3_LBT_ENABLE                   = 0x10;   // Binary: 00010000

  // E220 Protocol & Timing Constants (from doc/Feasibility.md)
  static constexpr uint32_t CONFIG_BAUD                       = 9600;   // Fixed config baud in Mode 3
  static constexpr uint32_t NORMAL_BAUD                       = 115200; // Normal Mode 0 baud
  static constexpr uint32_t AUX_TIMEOUT_MS                     = 1000;
  static constexpr uint32_t AUX_TRANSMIT_DETECT_TIMEOUT_MS     = 50;
  static constexpr uint32_t AUX_TRANSMIT_COMPLETE_TIMEOUT_MS   = 1000;
  static constexpr uint32_t GUARD_DELAY_MS                    = 50;     // 50ms air time guard delay
  
  // Settle, Sizing and Poll Timing Constants
  static constexpr uint32_t GUARD_SETTLE_DELAY_MS            = 5;      // Settle delay after mode changes / AUX rising edge
  static constexpr uint32_t TRANSMIT_SETTLE_DELAY_MS        = 10;     // Delay to let UART transmission finish before reading response
  static constexpr uint32_t POLL_INTERVAL_MS                 = 1;      // Polling delay when blocking waiting for pin changes
  static constexpr size_t   CONFIG_CMD_LEN                   = 11;     // Size of the E220 C0 config command
  static constexpr size_t   CONFIG_RESPONSE_LEN              = 11;     // Size of the E220 config write response
  
  static constexpr size_t   DEBUG_PREFIX_SIZE                = 64;     // Buffer for the "LoRa TX: target=..." log prefix
  static constexpr size_t   RX_BUFFER_SIZE                   = 256;    // Receive line buffer in loraTransportTask
  static constexpr char     RX_FRAME_TERMINATOR              = '\n';   // Milestone 0 plain-text frames end with a newline
  static constexpr char     RX_NON_PRINTABLE_PLACEHOLDER     = '.';    // Shown in the debug log for non-printable bytes

  // FreeRTOS Task Configuration Constants
  static constexpr uint32_t TASK_STACK_SIZE = 4096;
  static constexpr UBaseType_t TASK_PRIORITY = 2;
  static constexpr uint32_t TASK_DELAY_MS    = 10;      // Responsive polling rate
  static constexpr uint32_t TX_QUEUE_SIZE    = 10;      // Size of the TX message queue

  /**
   * Initializes the E220 hardware pins, sets Mode 3 to write configurations permanently,
   * then switches back to Mode 0 (Normal) and spawns the background transceiving task.
   */
  void begin();

  /**
   * Pushes raw data onto the FreeRTOS TX queue to be transmitted asynchronously 
   * by the background loraTransportTask. This method is thread-safe.
   */
  void sendRaw(const uint8_t* data, size_t len);

  /**
   * Registers the function to call when a raw data frame is received.
   */
  void setReceiveCallback(LoRaReceiveCallback cb);

  /**
   * Temporarily suspends the background transceiver task and closes UART2.
   */
  void pause();

  /**
   * Re-opens UART2 at normal operating baud and resumes the background task.
   */
  void resume();

 private:
  LoRaReceiveCallback receiveCallback = nullptr;
  TaskHandle_t transportTaskHandle = nullptr;
  QueueHandle_t txQueue = nullptr;

  /**
   * Physically writes a raw frame over the UART2 interface, executing all mandatory E220 
   * AUX line handshakes, polling, and guard delays. Must be called inside loraTransportTask only.
   */
  void sendRawInternal(const uint8_t* data, size_t len);

  /**
   * Safely transitions the M0 and M1 lines and waits for AUX to settle.
   */
  void setMode(uint8_t m0, uint8_t m1);

  /**
   * Background task that manages E220 RX/TX operations and queue draining.
   */
  static void loraTransportTask(void* param);
};

// Global singleton instance exported for direct use across the application
extern LoRaTransportClass LoRaTransport;
