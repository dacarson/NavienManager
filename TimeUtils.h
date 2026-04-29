#pragma once
#include <time.h>

// TZ-free equivalent of timegm(). Converts a UTC struct tm to time_t using
// pure integer arithmetic — no setenv("TZ"), no mktime(), no floating point.
//
// Supported range on ESP32 Arduino (signed 32-bit time_t):
//   1970-01-01 00:00:00 UTC  →  0
//   2038-01-19 03:14:07 UTC  →  INT32_MAX (2147483647)
// Inputs outside this range produce undefined (overflowed) results.
time_t proper_timegm(const struct tm *t);
