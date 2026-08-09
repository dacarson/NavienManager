// Host-side sanity test harness for PeakFinder::findDaySlots().
// Not a correctness oracle against the Python reference -- this confirms
// PeakFinder compiles, produces sane non-overlapping slots, and that the
// elapsedWeeks parameter (see archive/OnDeviceDollarCostWindowSearch.md) is
// actually wired into the cost formula: the same raw demand accumulated over
// a longer period must report a smaller net dollar benefit (covered_per_week
// = raw_count / elapsedWeeks). Whether that dilution also visibly narrows
// the *chosen width* depends on how the specific data is spaced relative to
// RECIRC_WASTE_USD's tiny per-cycle cost ($0.0024) -- real-world data proved
// this out during the on-device retuning study, but reliably reproducing a
// width change from a few hand-crafted buckets is fragile, so this test
// checks the more fundamental, robust property instead: the dollar value
// itself scales the way the formula implies.
//
// Lives outside the sketch root (unlike TimeUtils_test.cpp) because the
// Arduino/ESP32 builder auto-compiles every .cpp file directly in the sketch
// folder into the firmware -- a second host-test file with its own main()
// there collides with TimeUtils_test.cpp's main() at link time. Subfolders
// aren't auto-swept, so this one is safe here.
//
// Compile and run (from repo root):
//   g++ -I . -I test/test_stubs -o test/PeakFinder_test test/PeakFinder_test.cpp PeakFinder.cpp && ./test/PeakFinder_test

#include "PeakFinder.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void setBucket(BucketFile::Bucket *day, int bucketIdx,
                       uint16_t raw_count, float weighted_score) {
    day[bucketIdx].raw_count = raw_count;
    day[bucketIdx].weighted_score = weighted_score;
}

static void buildFixture(BucketFile::Bucket *day) {
    memset(day, 0, BUCKET_PER_DAY * sizeof(BucketFile::Bucket));

    // Strong, reliable peak at 08:00 (bucket 96) -- happens ~20x, high score.
    setBucket(day, 96, 20, 60.0f);
    setBucket(day, 95, 8,  20.0f);
    setBucket(day, 97, 8,  20.0f);

    // Moderate peak at 13:00 (bucket 156).
    setBucket(day, 156, 5, 15.0f);

    // Marginal peak right at the default MIN_WEIGHTED_SCORE floor, at 19:00
    // (bucket 228).
    setBucket(day, 228, 3, 6.0f);

    // Pure noise: single occurrences scattered through the day -- below
    // MIN_OCCURRENCES (3), should be filtered out entirely.
    setBucket(day, 30,  1, 3.0f);
    setBucket(day, 200, 1, 3.0f);
    setBucket(day, 270, 1, 3.0f);
}

// Well-formedness: no zero/negative-width slots, no overlaps.
static void checkWellFormed(const char *label, const TimeSlot *slots, int n) {
    for (int i = 0; i < n; i++) {
        if (slots[i].start_min >= slots[i].end_min) {
            printf("FAIL [%s]: slot %d has non-positive width (%d..%d)\n",
                   label, i, slots[i].start_min, slots[i].end_min);
            failures++;
        }
        for (int j = i + 1; j < n; j++) {
            bool overlap = slots[i].start_min < slots[j].end_min &&
                           slots[j].start_min < slots[i].end_min;
            if (overlap) {
                printf("FAIL [%s]: slots %d and %d overlap\n", label, i, j);
                failures++;
            }
        }
    }
}

// Find the slot covering a given bucket's minute-of-day, or nullptr.
static const TimeSlot *slotCovering(const TimeSlot *slots, int n, int bucketIdx) {
    int minute = bucketIdx * 5;
    for (int i = 0; i < n; i++) {
        if (minute >= slots[i].start_min && minute < slots[i].end_min) {
            return &slots[i];
        }
    }
    return nullptr;
}

int main(void) {
    static BucketFile::Bucket day[BUCKET_PER_DAY];

    // --- Scenario 1: short accumulation (elapsedWeeks=1, e.g. a freshly
    // reset device) -- covered_per_week is large, so net_benefit should be
    // large too.
    buildFixture(day);
    TimeSlot shortSlots[MAX_PEAK_CANDIDATES];
    int nShort = PeakFinder::findDaySlots(day, shortSlots, /*elapsedWeeks=*/1.0f);
    printf("elapsedWeeks=1: findDaySlots() returned %d slot(s):\n", nShort);
    for (int i = 0; i < nShort; i++) {
        printf("  [%d] %02d:%02d-%02d:%02d  net_benefit=$%.4f\n", i,
               shortSlots[i].start_min / 60, shortSlots[i].start_min % 60,
               shortSlots[i].end_min   / 60, shortSlots[i].end_min   % 60,
               shortSlots[i].score);
    }
    checkWellFormed("elapsedWeeks=1", shortSlots, nShort);

    // --- Scenario 2: long accumulation (elapsedWeeks=52, a full year) --
    // same raw demand, but covered_per_week is diluted ~52x, so net_benefit
    // for the same peak should be substantially smaller.
    buildFixture(day);
    TimeSlot longSlots[MAX_PEAK_CANDIDATES];
    int nLong = PeakFinder::findDaySlots(day, longSlots, /*elapsedWeeks=*/52.0f);
    printf("elapsedWeeks=52: findDaySlots() returned %d slot(s):\n", nLong);
    for (int i = 0; i < nLong; i++) {
        printf("  [%d] %02d:%02d-%02d:%02d  net_benefit=$%.4f\n", i,
               longSlots[i].start_min / 60, longSlots[i].start_min % 60,
               longSlots[i].end_min   / 60, longSlots[i].end_min   % 60,
               longSlots[i].score);
    }
    checkWellFormed("elapsedWeeks=52", longSlots, nLong);

    if (nShort < 1) {
        printf("FAIL: expected at least 1 slot at elapsedWeeks=1 (strong peak at 08:00)\n");
        failures++;
    } else {
        const TimeSlot *shortPeakSlot = slotCovering(shortSlots, nShort, 96);
        const TimeSlot *longPeakSlot  = slotCovering(longSlots,  nLong,  96);
        if (!shortPeakSlot) {
            printf("FAIL: strong peak at 08:00 missing from elapsedWeeks=1 result\n");
            failures++;
        } else if (!longPeakSlot) {
            printf("FAIL: strong peak at 08:00 missing from elapsedWeeks=52 result "
                   "(should still get a slot -- 20 raw hits even diluted 52x is still "
                   "worth more than a wasted cycle)\n");
            failures++;
        } else {
            printf("Strong-peak net_benefit: elapsedWeeks=1 -> $%.4f, elapsedWeeks=52 -> $%.4f\n",
                   shortPeakSlot->score, longPeakSlot->score);
            // Diluting the rate ~52x must reduce (never increase) the reported
            // dollar value for the same underlying raw demand. Requiring at
            // least a 10x drop (not the full 52x) leaves headroom for the two
            // scenarios picking different candidate widths with different gas
            // costs, while still catching a genuinely broken/unused
            // elapsedWeeks parameter.
            if (longPeakSlot->score >= shortPeakSlot->score / 10.0f) {
                printf("FAIL: expected elapsedWeeks=52 net_benefit ($%.4f) to be well "
                       "below elapsedWeeks=1 ($%.4f) -- elapsedWeeks doesn't appear to "
                       "be diluting covered_per_week\n",
                       longPeakSlot->score, shortPeakSlot->score);
                failures++;
            }
        }
    }

    if (failures == 0) {
        printf("PASS: all checks passed.\n");
        return 0;
    } else {
        printf("%d check(s) failed.\n", failures);
        return 1;
    }
}
