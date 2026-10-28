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

/** One registered FreeRTOS task entry. */
struct TaskRegistryEntry {
  const char*  name;          ///< Human-readable task label shown in GET /status JSON.
  TaskHandle_t handle;        ///< FreeRTOS task handle; nullptr if TASK_CREATE failed.
  uint32_t     stackBytes;    ///< Nominal stack size passed to TASK_CREATE (bytes).
};

/** Maximum number of tasks the registry can track. */
static constexpr uint8_t TASK_REGISTRY_MAX = 8;

class TaskRegistryClass {
 public:
  /**
   * Register a task handle. Call immediately after TASK_CREATE.
   *
   * @param name       Human-readable label used in GET /status JSON.
   * @param handle     Handle returned by TASK_CREATE (may be nullptr if creation failed).
   * @param stackBytes Nominal stack size passed to TASK_CREATE.
   */
  void add(const char* name, TaskHandle_t handle, uint32_t stackBytes) {
    if (count < TASK_REGISTRY_MAX) {
      entries[count++] = { name, handle, stackBytes };
    }
  }

  uint8_t           count = 0;                       ///< Number of registered entries.
  TaskRegistryEntry entries[TASK_REGISTRY_MAX] = {};  ///< Registered task entries.
};

/** Global singleton - extern declared here, defined in TaskRegistry.cpp. */
extern TaskRegistryClass TaskRegistry;
