// Host-side test harness for proper_timegm().
// Compile and run:
//   g++ -o TimeUtils_test TimeUtils_test.cpp TimeUtils.cpp && ./TimeUtils_test

#include "TimeUtils.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

struct TestCase {
    const char *label;
    int y, mo, d, h, mi, s;
    long long expected;
};

static const TestCase cases[] = {
    { "Unix epoch",                          1970,  1,  1,  0,  0,  0,          0LL },
    { "1970-12-31 23:59:59 UTC",             1970, 12, 31, 23, 59, 59,   31535999LL },
    { "2025-01-01 00:00:00 UTC",             2025,  1,  1,  0,  0,  0, 1735689600LL },
    // PST→PDT spring-forward: clocks move 02:00→03:00 PST on this UTC instant
    { "2025-03-09 10:00:00 UTC (DST ref)",   2025,  3,  9, 10,  0,  0, 1741514400LL },
    // Half-hour offset: 2025-07-01 02:30 UTC = noon in UTC+9:30
    { "2025-07-01 02:30:00 UTC (+9:30 ref)", 2025,  7,  1,  2, 30,  0, 1751337000LL },
    { "2024-02-29 12:00:00 UTC (leap day)",  2024,  2, 29, 12,  0,  0, 1709208000LL },
    // INT32_MAX — upper limit on ESP32 Arduino signed 32-bit time_t
    { "2038-01-19 03:14:07 UTC (INT32_MAX)", 2038,  1, 19,  3, 14,  7, 2147483647LL },
};

int main(void) {
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const TestCase &tc = cases[i];
        struct tm t;
        memset(&t, 0, sizeof(t));
        t.tm_year = tc.y - 1900;
        t.tm_mon  = tc.mo - 1;
        t.tm_mday = tc.d;
        t.tm_hour = tc.h;
        t.tm_min  = tc.mi;
        t.tm_sec  = tc.s;
        long long got = (long long)proper_timegm(&t);
        if (got == tc.expected) {
            printf("PASS  %-42s  %lld\n", tc.label, got);
        } else {
            printf("FAIL  %-42s  expected %lld, got %lld\n", tc.label, tc.expected, got);
            ++failures;
        }
    }
    printf("\n%s  (%d failure%s)\n",
           failures == 0 ? "ALL PASSED" : "FAILED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
