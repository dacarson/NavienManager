// Host-test stub for freertos/queue.h: a single-slot queue (the learner's
// demand-event queue has capacity 1 and is written with xQueueOverwrite).
//
// Queues come from a static pool, not the heap: the learner creates its queue
// in begin() and, like FreeRTOS objects on the device, never deletes it, so a
// heap allocation here is reported by LeakSanitizer in CI as a leak that is
// not one. The pool is sized for a test binary that creates a learner per
// test; exhausting it aborts loudly.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"

struct HostQueue {
  size_t item_size;
  unsigned char slot[64];
  bool full;
};
typedef HostQueue *QueueHandle_t;

namespace host_rtos {
static const int QUEUE_POOL = 64;
inline HostQueue *queue_pool() {
  static HostQueue pool[QUEUE_POOL];
  return pool;
}
inline int &queue_used() {
  static int n = 0;
  return n;
}
}  // namespace host_rtos

inline QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
  (void) length;
  if (item_size > sizeof(HostQueue::slot)) {
    fprintf(stderr, "freertos stub: queue item size %u exceeds the stub slot\n", (unsigned) item_size);
    abort();
  }
  if (host_rtos::queue_used() >= host_rtos::QUEUE_POOL) {
    fprintf(stderr, "freertos stub: queue pool exhausted\n");
    abort();
  }
  HostQueue *q = &host_rtos::queue_pool()[host_rtos::queue_used()++];
  q->item_size = item_size;
  q->full = false;
  return q;
}

inline BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item) {
  memcpy(q->slot, item, q->item_size);
  q->full = true;
  return pdTRUE;
}

inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
  (void) wait;
  if (q->full) return pdFALSE;
  return xQueueOverwrite(q, item);
}

inline BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait) {
  (void) wait;
  if (!q->full) return pdFALSE;
  memcpy(out, q->slot, q->item_size);
  q->full = false;
  return pdTRUE;
}
