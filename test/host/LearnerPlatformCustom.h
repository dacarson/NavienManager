// Host-test platform for the learner core (see LearnerPlatform.h): a clock the
// test steps by hand, a fixed UTC offset, logging to stderr when verbose, and
// no Arduino extras. The in-memory LearnerStorage lives in the test file.
#pragma once

#include <stdio.h>
#include <time.h>

namespace learner_host {
extern time_t now;           // what LEARNER_NOW() returns
extern int utc_offset_min;   // what LEARNER_UTC_OFFSET_MIN() returns (UTC = local + offset)
extern bool verbose;         // echo LEARNER_LOG lines to stderr
}  // namespace learner_host

#define LEARNER_LOG(fmt, ...)                                              \
  do {                                                                     \
    if (learner_host::verbose) fprintf(stderr, fmt "\n", ##__VA_ARGS__);   \
  } while (0)
#define LEARNER_WEBLOG(fmt, ...) LEARNER_LOG(fmt, ##__VA_ARGS__)
#define LEARNER_NOW() (learner_host::now)
#define LEARNER_UTC_OFFSET_MIN(now) (learner_host::utc_offset_min)
