#include "StatusLed.h"

#include <Arduino.h>

/**
 * StatusLed.cpp - Sole owner of the onboard status LED (GPIO 2).
 *
 * The led task runs permanently on whichever core FreeRTOS schedules it.
 * It blocks on ulTaskNotifyTake() in BOOTING and READY states (zero CPU),
 * uses vTaskDelay() in NO_DEVICE (yields for 1 s between toggles), and
 * loops synchronously in FAULT (SOS pattern) until setState() is called.
 *
 * No Java equivalent - ESP32-specific hardware feedback facility.
 */

// Global singleton instance.
StatusLed statusLed;

// ---- Timing and task constants ---------------------------------------------

static constexpr int SOS_DOT_DURATION_MS = 150;
static constexpr int SOS_DASH_DURATION_MS = 450;
static constexpr int SOS_SYMBOL_GAP_MS = 150;
static constexpr int SOS_WORD_GAP_MS = 700;
static constexpr int SOS_CYCLE_GAP_MS = 2000;            // Pause between two SOS cycles
static constexpr int SOS_PULSES_PER_LETTER = 3;          // S = ..., O = ---
static constexpr uint32_t NO_DEVICE_TOGGLE_MS = 1000;    // Slow blink half period
static constexpr uint32_t LED_TASK_STACK_SIZE = 1024;    // Bytes of stack for ledTask
static constexpr UBaseType_t LED_TASK_PRIORITY = 1;

// ---- blinkSOS --------------------------------------------------------------

/**
 * Drives one full SOS cycle (... --- ...) on the given pin and returns.
 * Called repeatedly by ledTask while in FAULT state.
 */
void StatusLed::blinkSOS(int pin) {
  // Three dots
  for (int pulseIndex = 0; pulseIndex < SOS_PULSES_PER_LETTER; pulseIndex++) {
    digitalWrite(pin, HIGH);
    delay(SOS_DOT_DURATION_MS);
    digitalWrite(pin, LOW);
    delay(SOS_SYMBOL_GAP_MS);
  }
  delay(SOS_WORD_GAP_MS);
  // Three dashes
  for (int pulseIndex = 0; pulseIndex < SOS_PULSES_PER_LETTER; pulseIndex++) {
    digitalWrite(pin, HIGH);
    delay(SOS_DASH_DURATION_MS);
    digitalWrite(pin, LOW);
    delay(SOS_SYMBOL_GAP_MS);
  }
  delay(SOS_WORD_GAP_MS);
  // Three dots
  for (int pulseIndex = 0; pulseIndex < SOS_PULSES_PER_LETTER; pulseIndex++) {
    digitalWrite(pin, HIGH);
    delay(SOS_DOT_DURATION_MS);
    digitalWrite(pin, LOW);
    delay(SOS_SYMBOL_GAP_MS);
  }
  delay(SOS_CYCLE_GAP_MS);
}

// ---- ledTask ---------------------------------------------------------------

/**
 * Permanent FreeRTOS task - sole owner of STATUS_LED_PIN.
 *
 * BOOTING / READY : sets LED then blocks indefinitely on task notification.
 *                   setState() sends the notification to wake immediately.
 * NO_DEVICE       : toggles LED every 1 s via vTaskDelay (yields between
 * toggles). setState() interrupts the delay via task notification. FAULT : runs
 * one SOS cycle then checks for a state change. Loops forever until setState()
 * is called with another state.
 */
void StatusLed::ledTask(void *param) {
  StatusLed *self = static_cast<StatusLed *>(param);

  for (;;) {
    switch (self->ledState) {

    case LedState::BOOTING:
      // Set LED and block indefinitely. FreeRTOS removes this task from the
      // scheduler ready list - zero CPU consumed until setState() notifies.
      digitalWrite(STATUS_LED_PIN, HIGH);
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      break;

    case LedState::READY:
      // Set LED and block indefinitely - same mechanism as BOOTING.
      digitalWrite(STATUS_LED_PIN, LOW);
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      break;

    case LedState::NO_DEVICE:
      // Toggle LED then block for up to 1 s. Yields the core for the full
      // 1 s interval. setState() wakes it early via xTaskNotifyGive.
      digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NO_DEVICE_TOGGLE_MS));
      break;

    case LedState::FAULT:
      // One SOS cycle (~3.5 s). Each delay() call yields via vTaskDelay
      // internally. Loops back to check ledState after each cycle so a
      // future setState(READY) can recover without waiting forever.
      blinkSOS(STATUS_LED_PIN);
      break;
    }
  }
}

// ---- begin -----------------------------------------------------------------

void StatusLed::begin() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH); // ON immediately while task starts
  xTaskCreate(ledTask, "StatusLed", LED_TASK_STACK_SIZE, this, LED_TASK_PRIORITY, &taskHandle);
}

// ---- setState --------------------------------------------------------------

void StatusLed::setState(LedState newState) {
  if (newState == ledState) {
    return;
  }
  ledState = newState;
  if (taskHandle != nullptr) {
    xTaskNotifyGive(taskHandle);
  }
}
