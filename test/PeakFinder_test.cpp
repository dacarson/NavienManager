// Host-side sanity test harness for PeakFinder::findDaySlots().
// Not a correctness oracle against the Python reference (the on-device port
// intentionally keeps a fixed-width architecture per
// archive/CostGroundedWindowSizing.md) -- this just confirms PeakFinder still
// compiles and produces sane, non-overlapping slots after retuning
// PEAK_HALF_WIDTH_MIN.
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

static void setBucket(BucketFile::Bucket *day, int bucketIdx,
                       uint16_t raw_count, float weighted_score) {
    day[bucketIdx].raw_count = raw_count;
    day[bucketIdx].weighted_score = weighted_score;
}

int main(void) {
    static BucketFile::Bucket day[BUCKET_PER_DAY];
    memset(day, 0, sizeof(day));

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

    TimeSlot slots[MAX_PEAK_CANDIDATES];
    int n = PeakFinder::findDaySlots(day, slots);

    printf("findDaySlots() returned %d slot(s):\n", n);
    for (int i = 0; i < n; i++) {
        printf("  [%d] %02d:%02d-%02d:%02d  score=%.1f\n", i,
               slots[i].start_min / 60, slots[i].start_min % 60,
               slots[i].end_min   / 60, slots[i].end_min   % 60,
               slots[i].score);
    }

    // Sanity checks (not exhaustive -- see file header).
    int failures = 0;
    if (n < 1) {
        printf("FAIL: expected at least 1 slot (strong peak at 08:00)\n");
        failures++;
    }
    for (int i = 0; i < n; i++) {
        if (slots[i].start_min >= slots[i].end_min) {
            printf("FAIL: slot %d has non-positive width (%d..%d)\n",
                   i, slots[i].start_min, slots[i].end_min);
            failures++;
        }
        for (int j = i + 1; j < n; j++) {
            bool overlap = slots[i].start_min < slots[j].end_min &&
                           slots[j].start_min < slots[i].end_min;
            if (overlap) {
                printf("FAIL: slots %d and %d overlap\n", i, j);
                failures++;
            }
        }
    }

    if (failures == 0) {
        printf("PASS: %d slot(s), non-overlapping, well-formed.\n", n);
        return 0;
    } else {
        printf("%d check(s) failed.\n", failures);
        return 1;
    }
}
