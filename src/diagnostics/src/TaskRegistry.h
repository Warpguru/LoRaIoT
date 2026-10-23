#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * TaskRegistry.h - Collects FreeRTOS task handles for diagnostics.
 *
 * Each task registers its handle here immediately after TASK_CREATE so that
 * GET /status can report per-task stack high-water marks via
 * uxTaskGetStackHighWaterMark(). The registry is populated during
 * applicationSetup(); all reads happen later from HTTP handlers.
 *
 * No mutex is needed: handles are written once during setup (single-threaded
 * phase) and only read thereafter.
 */

struct TaskRegistryEntry {
  const char*  name;
  TaskHandle_t handle;
  uint32_t     stackBytes;  // nominal stack size at creation (for context)
};

static constexpr uint8_t TASK_REGISTRY_MAX = 8;

class TaskRegistryClass {
 public:
  /**
   * Register a task handle. Call immediately after TASK_CREATE.
   * name     - human-readable label used in GET /status JSON
   * handle   - handle returned by TASK_CREATE (may be nullptr if creation failed)
   * stackBytes - nominal stack size passed to TASK_CREATE
   */
  void add(const char* name, TaskHandle_t handle, uint32_t stackBytes) {
    if (count < TASK_REGISTRY_MAX) {
      entries[count++] = { name, handle, stackBytes };
    }
  }

  uint8_t              count = 0;
  TaskRegistryEntry    entries[TASK_REGISTRY_MAX] = {};
};

/** Global singleton — extern declared here, defined in TaskRegistry.cpp. */
extern TaskRegistryClass TaskRegistry;
