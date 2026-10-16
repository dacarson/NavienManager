/*
Copyright (c) 2026 David Carson (dacarson)
Copyright (c) 2026 Brian Conrey (wsu-runner/navien-manager fork): platform seam

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

// ---------------------------------------------------------------------------
// Platform seam for the learner / scheduler core.
//
// The core is NavienLearner, PeakFinder, BucketStore, SchedulerBase and
// TimeUtils. Everything in those files that touches the platform (logging,
// the wall clock, the UTC offset, blob persistence) goes through the macros
// and the LearnerStorage interface declared here, so the same source files
// build in three places:
//
//   1. The HomeSpan sketch (default): Serial / WEBLOG logging, time(), the
//      libc TZ for the UTC offset, LittleFS for blobs
//      (LearnerStorageLittleFS.cpp).
//   2. Another firmware that embeds the core, e.g. the navien-bridge ESPHome
//      component: defines LEARNER_PLATFORM_CUSTOM and supplies
//      "LearnerPlatformCustom.h" plus a learnerStorage() implementation.
//   3. Host-side tests: same as 2 with a fake clock and in-memory storage.
//
// A custom platform header must define:
//
//   LEARNER_LOG(fmt, ...)         printf-style; the platform appends the newline
//   LEARNER_WEBLOG(fmt, ...)      user-visible log line (may alias LEARNER_LOG)
//   LEARNER_NOW()                 time_t: current epoch; 0 or a small value
//                                 while the clock is not yet synchronised
//   LEARNER_UTC_OFFSET_MIN(now)   int: UTC = local + offset, normalised to
//                                 [-720, 720] (PST is +480, Tokyo is -540)
//
// and may set the feature flags below (they default to 0 under a custom
// platform and to 1 in the sketch):
//
//   LEARNER_WITH_ARDUINO_EXTRAS   Arduino String / ArduinoJson APIs: the JSON
//                                 handoff variant of checkNewSchedule(),
//                                 ingestBucketPayload(), appendStatusHTML()
//   LEARNER_WITH_UDP_STATUS       learner status broadcast over AsyncUDP
//   LEARNER_WITH_NVS_SCHEDULE     SchedulerBase's own NVS persistence, TZ
//                                 handling and WiFi/SNTP time wait
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Named blob storage. The core keeps a handful of fixed-size records (the
// bucket histogram, the measured-efficiency window); each is loaded and saved
// whole under a short name. save() must be atomic with respect to power loss:
// after a failed save the previous contents are still loadable.
class LearnerStorage {
 public:
  virtual ~LearnerStorage() {}
  // Load exactly `len` bytes of the named blob. false if absent, short or
  // unreadable; `buf` is then unspecified and the caller re-initialises.
  virtual bool load(const char *name, void *buf, size_t len) = 0;
  // Replace the named blob with `len` bytes.
  virtual bool save(const char *name, const void *buf, size_t len) = 0;
};

// Provided by the platform: LearnerStorageLittleFS.cpp in the sketch, the
// embedding firmware or the test harness otherwise.
LearnerStorage &learnerStorage();

#ifdef LEARNER_PLATFORM_CUSTOM

#include "LearnerPlatformCustom.h"

#else  // HomeSpan sketch defaults

#include <Arduino.h>
#include "HomeSpan.h"

#define LEARNER_LOG(fmt, ...)    Serial.printf(fmt "\n", ##__VA_ARGS__)
#define LEARNER_WEBLOG(fmt, ...) WEBLOG(fmt, ##__VA_ARGS__)
#define LEARNER_NOW()            time(nullptr)

// UTC offset from the libc TZ (set from NVS by SchedulerBase or the Telnet
// `timezone` command). Snapshotted by callers so one recompute pass uses one
// offset even if the clock crosses an hour boundary mid-pass.
inline int learnerUtcOffsetMinFromLibc(time_t now) {
  struct tm local_tm = {}, utc_tm = {};
  localtime_r(&now, &local_tm);
  gmtime_r(&now, &utc_tm);
  int off = (utc_tm.tm_hour * 60 + utc_tm.tm_min) - (local_tm.tm_hour * 60 + local_tm.tm_min);
  if (off > 720) off -= 1440;
  if (off < -720) off += 1440;
  return off;
}
#define LEARNER_UTC_OFFSET_MIN(now) learnerUtcOffsetMinFromLibc(now)

#define LEARNER_WITH_ARDUINO_EXTRAS 1
#define LEARNER_WITH_UDP_STATUS     1
#define LEARNER_WITH_NVS_SCHEDULE   1

#endif  // LEARNER_PLATFORM_CUSTOM

#ifndef LEARNER_WITH_ARDUINO_EXTRAS
#define LEARNER_WITH_ARDUINO_EXTRAS 0
#endif
#ifndef LEARNER_WITH_UDP_STATUS
#define LEARNER_WITH_UDP_STATUS 0
#endif
#ifndef LEARNER_WITH_NVS_SCHEDULE
#define LEARNER_WITH_NVS_SCHEDULE 0
#endif
