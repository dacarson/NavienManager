// Host-test stub for freertos/FreeRTOS.h: the handful of types and constants
// the learner core uses. Single-threaded; the Core 0 task is never started
// (xTaskCreatePinnedToCore succeeds without running anything) and tests drive
// NavienLearner::taskStep() by hand.
#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(ms) ((TickType_t) (ms))
#define portMAX_DELAY ((TickType_t) 0xffffffffUL)
