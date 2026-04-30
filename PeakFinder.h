/*
Copyright (c) 2026 David Carson (dacarson)

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

#include <stdint.h>
#include "BucketStore.h"

// Maximum slots Eve will accept per day (silently truncates a 4th).
#define MAX_SLOTS_PER_DAY   3

// Maximum local-maxima candidates collected before NMS.  Set to the
// theoretical bound for separation-limited peaks in a 288-bucket day:
//   floor(BUCKET_PER_DAY / sep_buckets) = floor(288 / 9) = 32
// This covers all realistic and smoothed domestic data.  Python has no hard
// cap; pathological (adversarial) smoothed arrays could in theory produce
// more than 32 local maxima, but that cannot occur in real bucket streams.
#define MAX_PEAK_CANDIDATES 32

// ---------------------------------------------------------------------------
// TimeSlot — one recirculation window in minutes-since-midnight.
// Matches the fields used by FakeGatoScheduler::setWeekScheduleFromJSON().
// ---------------------------------------------------------------------------

// Note: this is distinct from SchedulerBase's internal time representation.
// Do not mix PeakFinder::TimeSlot with SchedulerBase slot types without
// explicit conversion.
struct TimeSlot {
    uint16_t start_min;  // minutes since midnight (start of recirc window)
    uint16_t end_min;    // minutes since midnight (end of recirc window)
    float    score;      // weighted score of the peak that produced this slot
};

// ---------------------------------------------------------------------------
// PeakFinder
//
// C++ port of navien_schedule_learner.py::_find_peaks() and
// buckets_to_windows().  Operates entirely on the in-RAM BucketFile arrays;
// no flash I/O.  All methods are static — no instance is needed.
//
// Memory rules (from spec §Memory Budget):
//   Rule 2: smoothed[288] and filtered[288] are declared static inside
//   findPeaks() so they live in BSS rather than on the task stack.
// ---------------------------------------------------------------------------

class PeakFinder {
public:
    // Algorithm parameters — match Python script defaults.
    static constexpr int   PEAK_HALF_WIDTH_MIN     = 30;   // ±30 min window
    static constexpr int   MIN_PEAK_SEPARATION_MIN = 45;   // 9 buckets
    static constexpr int   PREHEAT_MINUTES         = 3;    // COLD_PIPE_DRAIN_MINUTES
    static constexpr float MIN_WEIGHTED_SCORE      = 6.0f;
    static constexpr float MIN_SCORE_FLOOR         = 3.0f;
    static constexpr int   MIN_OCCURRENCES         = 3;
    static constexpr float SCORE_STEP              = 1.0f;
    static constexpr int   SMOOTH_RADIUS           = 2;    // ±2 buckets

    // Find schedule slots for one day using the adaptive threshold algorithm.
    //
    // day_buckets : array of BUCKET_PER_DAY buckets (from BucketFile).
    // out_slots   : caller-supplied array of at least MAX_SLOTS_PER_DAY entries.
    //
    // Returns the number of slots written (0 – MAX_SLOTS_PER_DAY), sorted
    // chronologically (ascending start_min).
    static int findDaySlots(const BucketFile::Bucket *day_buckets,
                            TimeSlot *out_slots);

private:
    // A peak candidate: bucket index and raw weighted score.
    struct Peak {
        int   bucket;  // 0–287
        float score;   // raw weighted_score at this bucket
    };

    // Run one iteration of _find_peaks() with a specific threshold and
    // occurrence floor.  Writes up to MAX_PEAK_CANDIDATES entries into
    // out_accepted in greedy NMS accept order (not guaranteed score-sorted);
    // returns accepted count.  Caller is responsible for sorting by score and
    // truncating to MAX_SLOTS_PER_DAY.
    //
    // Mirrors Python _find_peaks():
    //   1. Build filtered score array (0 for buckets below threshold/occ_floor)
    //   2. Smooth with ±SMOOTH_RADIUS sliding average, always dividing by 5
    //   3. Find local maxima within ±sep_buckets
    //   4. Greedy NMS: accept in descending score order if >= sep_buckets apart
    static int findPeaks(const BucketFile::Bucket *day_buckets,
                         float threshold, int occ_floor, int sep_buckets,
                         Peak *out_accepted);

    // Convert accepted peaks to TimeSlot windows.
    // Applies preheat offset and rounds to nearest 10-minute boundary.
    // Returns number of slots written, sorted chronologically.
    static int buildSlots(const Peak *accepted, int n_accepted,
                          TimeSlot *out_slots);
};
