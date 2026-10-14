#pragma once

#include <Arduino.h>

/**
 * StatusLed.h - Sole owner of the onboard status LED (GPIO 2).
 *
 * Runs a permanent FreeRTOS task that drives the LED according to a state
 * machine. All external code calls setState() - no other file ever calls
 * pinMode(), digitalWrite(), or digitalRead() on STATUS_LED_PIN.
 *
 * The task can be woken from any core: setState() uses xTaskNotifyGive()
 * which is ISR- and cross-core-safe.
 *
 * States:
 *   BOOTING   - steady ON  (from begin() until server + device are ready)
 *   NO_DEVICE - slow blink 1 s / 1 s  (server up, no converter found)
 *   READY     - OFF  (server up, converter found and communicating)
 *   FAULT     - SOS pattern, loops forever  (unrecoverable error)
 *
 * No Java equivalent - ESP32-specific hardware feedback facility.
 */

/**
 * Status LED pin - external LED on GPIO 21 (ESP32-WROOM-32, 38-pin).
 *
 * GPIO 21 has no boot-strapping function, no UART assignment, and is not
 * used elsewhere in this project. Wire an LED + 330 Ω resistor between
 * GPIO 21 and GND.
 *
 * The onboard LED (hardwired to GPIO 2 on most DevKit PCBs) cannot be used
 * because the USB-serial bridge drives GPIO 2 as an RX activity indicator
 * in hardware - it flickers on every Serial.println() regardless of firmware.
 */
static constexpr int STATUS_LED_PIN = 21;

enum class LedState {
  BOOTING,   // steady ON
  NO_DEVICE, // slow blink 1 s / 1 s
  READY,     // off
  FAULT      // SOS, loops forever
};

class StatusLed {
public:
  /**
   * Initialises GPIO 2, turns the LED on (BOOTING state), and starts the
   * led task on whichever core is available (tskNO_AFFINITY).
   * Must be called once from applicationSetup() before any other call.
   */
  void begin();

  /**
   * Transitions to a new state and wakes the led task immediately.
   * Safe to call from any core or ISR context.
   */
  void setState(LedState state);

private:
  static void ledTask(void *param);
  static void blinkSOS(int pin);

  volatile LedState ledState = LedState::BOOTING;
  TaskHandle_t taskHandle = nullptr;
};

/** Global singleton - defined in StatusLed.cpp, used by Application.cpp and
 * Server.cpp. */
extern StatusLed statusLed;
