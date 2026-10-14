#pragma once

/**
 * TaskUtils.h - Portable FreeRTOS task creation helper.
 *
 * TASK_CREATE(func, name, stack, param, prio, handle)
 *
 * On dual-core builds (LORAIOT_DUAL_CORE defined) the task is pinned to
 * Core 1, keeping the Arduino loop / WiFi stack on Core 0 undisturbed.
 *
 * On single-core builds the pin-to-core variant is omitted entirely.
 * CONFIG_FREERTOS_UNICORE is set automatically by ESP-IDF for every
 * single-core target (ESP32-C3, C6, S2, H2, etc.) - no per-board ifdefs
 * needed. Passing core ID 1 on a unicore build causes an immediate kernel
 * panic, which is exactly what this macro avoids.
 */

#if defined(LORAIOT_DUAL_CORE) && !defined(CONFIG_FREERTOS_UNICORE)
  #define TASK_CREATE(func, name, stack, param, prio, handle) \
    xTaskCreatePinnedToCore((func), (name), (stack), (param), (prio), (handle), 1)
#else
  #define TASK_CREATE(func, name, stack, param, prio, handle) \
    xTaskCreate((func), (name), (stack), (param), (prio), (handle))
#endif
