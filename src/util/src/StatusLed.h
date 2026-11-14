#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstddef>

/**
 * StatusLed.h - Sole owner of the status and LoRa activity LEDs.
 *
 * Runs a permanent FreeRTOS task that drives the LEDs according to a state
 * machine. All external code calls setState() / pulse() - no other file ever calls
 * pinMode(), digitalWrite(), or digitalRead() on the LED pins.
 *
 * All methods are task context only (any task, any core), never ISR.
 *
 * States:
 *   OFF       - steady OFF
 *   BOOTING   - steady ON  (from begin() until server + device are ready)
 *   READY     - steady OFF (server up and communicating normally)
 *   WARNING   - fast blink 250 ms on / 250 ms off (Status LED: unconfigured; RX LED: link lost)
 *   FAULT     - SOS pattern, loops forever  (unrecoverable error)
 *   ON        - steady ON
 *
 * No Java equivalent - ESP32-specific hardware feedback facility.
 */

// ---- Pin assignments and timing constants ----------------------------------

#ifndef STATUS_LED_PIN
static constexpr int STATUS_LED_PIN = 21;
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
#ifndef RX_LED_PIN
static constexpr int RX_LED_PIN = 6;
#endif
#ifndef TX_LED_PIN
static constexpr int TX_LED_PIN = 7;
#endif
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
#ifndef RX_LED_PIN
static constexpr int RX_LED_PIN = 4;
#endif
#ifndef TX_LED_PIN
static constexpr int TX_LED_PIN = 5;
#endif
#else  // Default: ESP32 (esp32dev), ESP32-S2, ESP32-S3
#ifndef RX_LED_PIN
static constexpr int RX_LED_PIN = 13;
#endif
#ifndef TX_LED_PIN
static constexpr int TX_LED_PIN = 18;
#endif
#endif

static constexpr uint32_t ACTIVITY_PULSE_MS = 100;

// ---- LED and state enumerations --------------------------------------------

enum class Led : uint8_t {
  STATUS_LED,  ///< Node state LED (status LED on GPIO 21)
  RX_LED,      ///< LoRa frame received activity LED
  TX_LED,      ///< LoRa frame transmitting activity LED
  COUNT        ///< Total number of managed LEDs; not a valid LED identifier
};

static constexpr size_t LED_COUNT = static_cast<size_t>(Led::COUNT);

enum class LedState : uint8_t {
  OFF,        ///< Steady OFF
  BOOTING,    ///< Steady ON - held from begin() until server and devices are ready
  READY,      ///< Steady OFF - server up and communicating normally
  WARNING,    ///< Fast blink 250 ms on / 250 ms off (Status LED: unconfigured; RX LED: link lost)
  FAULT,      ///< SOS blink pattern, loops forever - unrecoverable error
  ON          ///< Steady ON
};

/**
 * StatusLed - Singleton controller of the physical status LED state machine.
 *
 * This class operates as a global singleton instance 'statusLed'.
 * It manages an internal FreeRTOS task that drives the blink codes and transitions
 * based on the LedState enum.
 */
class StatusLed {
public:
  StatusLed();

  /**
   * Configures all three LED pins, turns all three LEDs on (BOOTING state for visual self-test),
   * and starts the led task on whichever core is available (tskNO_AFFINITY).
   * Must be called once from applicationSetup() before any other call.
   */
  void begin();

  /**
   * Sets the state of one LED and wakes ledTask immediately.
   * Task context only (any task, any core), never ISR.
   */
  void setState(Led led, LedState state);

  /**
   * Lights the LED for ACTIVITY_PULSE_MS on top of its current pattern (the output is pattern OR pulse).
   * A new pulse while one is running restarts the hold time. Works for every LED, including STATUS_LED.
   * Task context only (any task, any core), never ISR.
   */
  void pulse(Led led);

private:
  /** FreeRTOS task body - drives the deadline-driven LED engine. */
  static void ledTask(void* param);

  struct LedChannel {
    std::atomic<LedState> desiredState{LedState::OFF};
    std::atomic<uint32_t> pulseTriggerCount{0};
  };

  LedChannel channels[LED_COUNT];
  TaskHandle_t taskHandle = nullptr;  ///< Handle of the ledTask FreeRTOS task.
};

/** Global singleton instance exported for direct use across the application. */
extern StatusLed statusLed;
