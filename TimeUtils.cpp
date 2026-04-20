#include "TimeUtils.h"

// Days from civil (Gregorian) date to Unix epoch (1970-01-01 = day 0).
// Algorithm: Howard Hinnant — https://howardhinnant.github.io/date_algorithms.html
// "days_from_civil", reproduced verbatim in integer arithmetic.
static long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400L);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

time_t proper_timegm(const struct tm *t) {
    const long days = days_from_civil(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    return (time_t)(days * 86400L
                  + (long)t->tm_hour * 3600L
                  + (long)t->tm_min  * 60L
                  + (long)t->tm_sec);
}
