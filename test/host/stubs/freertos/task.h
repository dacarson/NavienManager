// Host-test stub for freertos/task.h. Task creation succeeds but runs nothing;
// vTaskDelay is a no-op. Tests call NavienLearner::taskStep() directly.
#pragma once

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                          void *param, UBaseType_t prio, TaskHandle_t *handle,
                                          int core) {
  (void) fn;
  (void) name;
  (void) stack;
  (void) param;
  (void) prio;
  (void) core;
  if (handle) *handle = nullptr;
  return pdPASS;
}

inline void vTaskDelay(TickType_t ticks) { (void) ticks; }
