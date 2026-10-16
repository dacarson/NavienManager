// Host-test stub for freertos/semphr.h: a non-blocking mutex flag, from a
// static pool for the same reason as queue.h (never freed on the device, so a
// heap allocation would read as a leak under LeakSanitizer in CI).
#pragma once

#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"

struct HostSemaphore {
  bool taken;
};
typedef HostSemaphore *SemaphoreHandle_t;

namespace host_rtos {
static const int SEM_POOL = 64;
inline HostSemaphore *sem_pool() {
  static HostSemaphore pool[SEM_POOL];
  return pool;
}
inline int &sem_used() {
  static int n = 0;
  return n;
}
}  // namespace host_rtos

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  if (host_rtos::sem_used() >= host_rtos::SEM_POOL) {
    fprintf(stderr, "freertos stub: semaphore pool exhausted\n");
    abort();
  }
  HostSemaphore *s = &host_rtos::sem_pool()[host_rtos::sem_used()++];
  s->taken = false;
  return s;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
  if (s->taken) {
    if (wait == portMAX_DELAY) {
      // Single-threaded harness: a blocking take on a held mutex would never
      // return on the device either (deadlock or a lost handoff). Abort so
      // the bug shows up as a test failure instead of being ignored.
      fprintf(stderr, "freertos stub: blocking xSemaphoreTake on a held mutex\n");
      abort();
    }
    return pdFALSE;
  }
  s->taken = true;
  return pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  s->taken = false;
  return pdTRUE;
}
