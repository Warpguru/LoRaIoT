#include "StatusLed.h"

#include <Arduino.h>

#include "../../diagnostics/src/TaskRegistry.h"

/**
 * StatusLed.cpp - Sole owner of the status and LoRa activity LEDs.
 *
 * Drives all managed LEDs (STATUS_LED, RX_LED, TX_LED) via a single deadline-driven
 * FreeRTOS task. The task evaluates absolute millisecond timestamps for state patterns
 * (steady, blinking, SOS Morse) and temporary activity pulses (e.g. 100 ms RX flash).
 * It calculates the earliest upcoming deadline across all channels and sleeps for that
 * exact duration, consuming zero CPU when all LEDs are idle or steady.
 *
 * No Java equivalent - ESP32-specific hardware feedback facility.
 */

// Global singleton instance.
StatusLed statusLed;

// ---- Timing and task constants ---------------------------------------------

static constexpr int LED_PINS[LED_COUNT] = {
  STATUS_LED_PIN,
  RX_LED_PIN,
  TX_LED_PIN
};

static constexpr int SOS_DOT_DURATION_MS = 150;
static constexpr int SOS_DASH_DURATION_MS = 450;
static constexpr int SOS_SYMBOL_GAP_MS = 150;
static constexpr int SOS_WORD_GAP_MS = 700;
static constexpr int SOS_CYCLE_GAP_MS = 2000;            // Pause between two SOS cycles
static constexpr uint32_t WARNING_TOGGLE_MS = 250;      // Fast warning blink half period (250 ms ON / 250 ms OFF)
static constexpr uint32_t LED_TASK_STACK_SIZE = 1536;    // Stack size for ledTask (FreeRTOS bytes)
static constexpr UBaseType_t LED_TASK_PRIORITY = 1;

// SOS step sequence: even index = ON, odd index = OFF.
// Timing breakdown reproducing standard Morse code:
//   S: 3 dots with symbol gaps, followed by a word gap
//   O: 3 dashes with symbol gaps, followed by a word gap
//   S: 3 dots with symbol gaps, followed by a cycle gap before repeat
static constexpr uint16_t SOS_PATTERN_STEPS[] = {
  // S: dot, gap, dot, gap, dot, word gap
  SOS_DOT_DURATION_MS,
  SOS_SYMBOL_GAP_MS,
  SOS_DOT_DURATION_MS,
  SOS_SYMBOL_GAP_MS,
  SOS_DOT_DURATION_MS,
  SOS_SYMBOL_GAP_MS + SOS_WORD_GAP_MS,

  // O: dash, gap, dash, gap, dash, word gap
  SOS_DASH_DURATION_MS,
  SOS_SYMBOL_GAP_MS,
  SOS_DASH_DURATION_MS,
  SOS_SYMBOL_GAP_MS,
  SOS_DASH_DURATION_MS,
  SOS_SYMBOL_GAP_MS + SOS_WORD_GAP_MS,

  // S: dot, gap, dot, gap, dot, cycle gap
  SOS_DOT_DURATION_MS,
  SOS_SYMBOL_GAP_MS,
  SOS_DOT_DURATION_MS,
  SOS_SYMBOL_GAP_MS,
  SOS_DOT_DURATION_MS,
  SOS_SYMBOL_GAP_MS + SOS_CYCLE_GAP_MS
};

static constexpr size_t SOS_STEP_COUNT = sizeof(SOS_PATTERN_STEPS) / sizeof(SOS_PATTERN_STEPS[0]);

// ---- Per-LED runtime state (private to ledTask) ----------------------------

/**
 * Tracks the internal state machine and deadlines for a single physical LED channel.
 * Stored outside the task stack to minimize stack memory consumption.
 */
struct LedRuntime {
  LedState activePatternState       = LedState::OFF;  // The pattern currently being generated (OFF, BOOTING, READY, WARNING, FAULT, ON)
  bool     patternPinLevel          = false;          // HIGH / LOW dictated by the state pattern
  size_t   sosStepIndex             = 0;              // Current position in SOS timing table (0..17)
  uint32_t nextPatternToggleTimeMs  = 0;              // millis() timestamp when the current pattern step expires
  bool     hasPatternTimer          = false;          // True if the pattern requires periodic edge toggles (WARNING or FAULT)

  uint32_t lastHandledPulseTrigger  = 0;              // Tracks pulseTriggerCount to detect newly requested pulses
  uint32_t pulseUntilTimeMs         = 0;              // millis() timestamp when the temporary activity pulse expires
  bool     isPulseLit               = false;          // True while a temporary activity pulse is actively lit
};

// ---- Constructor -----------------------------------------------------------

StatusLed::StatusLed() {
  channels[static_cast<size_t>(Led::STATUS_LED)].desiredState.store(LedState::BOOTING, std::memory_order_relaxed);
  channels[static_cast<size_t>(Led::RX_LED)].desiredState.store(LedState::BOOTING, std::memory_order_relaxed);
  channels[static_cast<size_t>(Led::TX_LED)].desiredState.store(LedState::BOOTING, std::memory_order_relaxed);
}

// ---- ledTask ---------------------------------------------------------------

/**
 * Permanent FreeRTOS task - sole owner of all managed LED pins.
 *
 * Algorithm for each iteration across all LEDs:
 * 1. Capture current system uptime timestamp (nowMs).
 * 2. Activity Pulses: Check if a new pulse was triggered (restarting expiration to nowMs + 100ms)
 *    or if an ongoing pulse has expired.
 * 3. State Patterns: Check if desiredState changed. If so, initialize the pattern (steady level,
 *    start 1s blink, or reset SOS step index). If unchanged, check if the current pattern timer expired
 *    and advance to the next step.
 * 4. GPIO Output: Set physical pin level to (patternPinLevel OR isPulseLit).
 * 5. Deadline Calculation: Find the smallest remaining duration among all active timers (pulses and patterns).
 * 6. Sleep: If no timers are active, block indefinitely until notified (zero CPU). If a timer is already due,
 *    drain notifications and loop immediately. Otherwise, sleep for the exact remaining duration.
 */
void StatusLed::ledTask(void* param) {
  StatusLed* self = static_cast<StatusLed*>(param);
  static LedRuntime runtime[LED_COUNT];

  for (;;) {
    const uint32_t nowMs = millis();
    int32_t earliestDeadlineRemainingMs = -1;  // -1 indicates all LEDs steady, sleep indefinitely

    for (size_t i = 0; i < LED_COUNT; ++i) {
      const LedState desiredState = self->channels[i].desiredState.load(std::memory_order_relaxed);
      const uint32_t triggerCount = self->channels[i].pulseTriggerCount.load(std::memory_order_relaxed);
      LedRuntime& rt = runtime[i];

      // --- 1. Evaluate Activity Pulse Timestamps ---
      if (triggerCount != rt.lastHandledPulseTrigger) {
        // A new pulse() was requested: set/extend expiration to now + ACTIVITY_PULSE_MS (100 ms)
        rt.lastHandledPulseTrigger = triggerCount;
        rt.isPulseLit = true;
        rt.pulseUntilTimeMs = nowMs + ACTIVITY_PULSE_MS;
      } else if (rt.isPulseLit) {
        // Check if the pulse duration has elapsed using wrap-safe signed subtraction
        if (static_cast<int32_t>(rt.pulseUntilTimeMs - nowMs) <= 0) {
          rt.isPulseLit = false;
        }
      }

      // --- 2. Evaluate State Patterns (Steady, Blink, SOS) ---
      if (desiredState != rt.activePatternState) {
        // State changed: re-initialize the pattern generator for this LED
        rt.activePatternState = desiredState;
        rt.sosStepIndex = 0;

        switch (desiredState) {
          case LedState::ON:
          case LedState::BOOTING:
            // Steady ON: pin held HIGH, no timer required
            rt.patternPinLevel = true;
            rt.hasPatternTimer = false;
            break;

          case LedState::OFF:
          case LedState::READY:
            // Steady OFF: pin held LOW, no timer required
            rt.patternPinLevel = false;
            rt.hasPatternTimer = false;
            break;

          case LedState::WARNING:
            // Fast warning blink: starts HIGH for the first 250 ms interval
            rt.patternPinLevel = true;
            rt.nextPatternToggleTimeMs = nowMs + WARNING_TOGGLE_MS;
            rt.hasPatternTimer = true;
            break;

          case LedState::FAULT:
            // SOS Morse code: step 0 is even -> HIGH (first dot)
            rt.patternPinLevel = true;
            rt.nextPatternToggleTimeMs = nowMs + SOS_PATTERN_STEPS[0];
            rt.hasPatternTimer = true;
            break;
        }
      } else if (rt.hasPatternTimer) {
        // Pattern continues: check if the current blink/Morse step duration has elapsed
        if (static_cast<int32_t>(rt.nextPatternToggleTimeMs - nowMs) <= 0) {
          if (rt.activePatternState == LedState::WARNING) {
            // Toggle between HIGH and LOW every 250 ms
            rt.patternPinLevel = !rt.patternPinLevel;
            rt.nextPatternToggleTimeMs = nowMs + WARNING_TOGGLE_MS;
          } else if (rt.activePatternState == LedState::FAULT) {
            // Advance to next step in the 18-step SOS Morse table
            rt.sosStepIndex = (rt.sosStepIndex + 1) % SOS_STEP_COUNT;
            rt.patternPinLevel = ((rt.sosStepIndex % 2) == 0);  // Even steps = ON, odd steps = OFF
            rt.nextPatternToggleTimeMs = nowMs + SOS_PATTERN_STEPS[rt.sosStepIndex];
          }
        }
      }

      // --- 3. Output Level to GPIO (Pattern OR Pulse) ---
      // An activity pulse forces the LED ON even during OFF segments of a pattern
      const bool physicalPinLevel = rt.patternPinLevel || rt.isPulseLit;
      digitalWrite(LED_PINS[i], physicalPinLevel ? HIGH : LOW);

      // --- 4. Find Earliest Upcoming Deadline Across All LEDs ---
      // Evaluate remaining time for active activity pulses
      if (rt.isPulseLit) {
        int32_t remainingMs = static_cast<int32_t>(rt.pulseUntilTimeMs - nowMs);
        if (remainingMs < 0) {
          remainingMs = 0;
        }
        if (earliestDeadlineRemainingMs < 0 || remainingMs < earliestDeadlineRemainingMs) {
          earliestDeadlineRemainingMs = remainingMs;
        }
      }

      // Evaluate remaining time for active pattern timers
      if (rt.hasPatternTimer) {
        int32_t remainingMs = static_cast<int32_t>(rt.nextPatternToggleTimeMs - nowMs);
        if (remainingMs < 0) {
          remainingMs = 0;
        }
        if (earliestDeadlineRemainingMs < 0 || remainingMs < earliestDeadlineRemainingMs) {
          earliestDeadlineRemainingMs = remainingMs;
        }
      }
    }

    // --- 5. Sleep until Next Deadline (Zero CPU When Idle) ---
    if (earliestDeadlineRemainingMs < 0) {
      // All LEDs in steady state (BOOTING, READY, OFF, ON) - sleep indefinitely until notified
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    } else if (earliestDeadlineRemainingMs == 0) {
      // Deadline due now - drain pending notifications and process next step immediately
      ulTaskNotifyTake(pdTRUE, 0);
    } else {
      // Sleep for exact remaining ms (clamped to at least 1 FreeRTOS tick)
      TickType_t ticksToWait = pdMS_TO_TICKS(earliestDeadlineRemainingMs);
      if (ticksToWait == 0) {
        ticksToWait = 1;
      }
      ulTaskNotifyTake(pdTRUE, ticksToWait);
    }
  }
}

// ---- begin -----------------------------------------------------------------

void StatusLed::begin() {
  channels[static_cast<size_t>(Led::STATUS_LED)].desiredState.store(LedState::BOOTING, std::memory_order_relaxed);
  channels[static_cast<size_t>(Led::RX_LED)].desiredState.store(LedState::BOOTING, std::memory_order_relaxed);
  channels[static_cast<size_t>(Led::TX_LED)].desiredState.store(LedState::BOOTING, std::memory_order_relaxed);

  for (size_t i = 0; i < LED_COUNT; ++i) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], HIGH);  // All LEDs ON immediately during boot for visual functional diagnostics
  }

  xTaskCreate(ledTask, "StatusLed", LED_TASK_STACK_SIZE, this, LED_TASK_PRIORITY, &taskHandle);
  TaskRegistry.add("StatusLed", taskHandle, LED_TASK_STACK_SIZE);
}

// ---- setState --------------------------------------------------------------

void StatusLed::setState(Led led, LedState state) {
  size_t idx = static_cast<size_t>(led);
  if (idx >= LED_COUNT) {
    return;
  }
  LedState currentState = channels[idx].desiredState.load(std::memory_order_relaxed);
  if (currentState == state) {
    return;
  }
  channels[idx].desiredState.store(state, std::memory_order_relaxed);
  if (taskHandle != nullptr) {
    xTaskNotifyGive(taskHandle);
  }
}

// ---- pulse -----------------------------------------------------------------

void StatusLed::pulse(Led led) {
  size_t idx = static_cast<size_t>(led);
  if (idx >= LED_COUNT) {
    return;
  }
  channels[idx].pulseTriggerCount.fetch_add(1, std::memory_order_relaxed);
  if (taskHandle != nullptr) {
    xTaskNotifyGive(taskHandle);
  }
}
