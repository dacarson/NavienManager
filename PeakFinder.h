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
    float    score;      // net dollar benefit of this window (covered demand
                          // value minus gas cost — see buildSlots()), NOT the
                          // peak's raw weighted_score. recomputeWrite() sorts
                          // and prunes to MAX_SLOTS_PER_DAY by this field, so
                          // that pruning is $-based as a direct consequence.
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
    // Algorithm parameters.
    //
    // PEAK_HALF_WIDTH_MIN: retuned from the original ±30min to ±45min based on
    // navien_schedule_learner.py's per-peak $-cost search (_best_slot_for_peak,
    // see archive/CostGroundedWindowSizing.md) run against a full year of real
    // InfluxDB history. That search picks each peak's width independently to
    // maximize covered_per_day*COLD_START_WASTE_USD - gas_waste_usd; across the
    // 21 slots kept for a real week (3/day x 7 days) the chosen half-widths
    // ranged 35-55min with both the mode and mean landing at ~45min — real
    // demand trickles in across a wider span than a sharp single-bucket spike,
    // so covered demand keeps growing out to the search's full local scope
    // (±MIN_PEAK_SEPARATION_MIN) for most peaks. On-device can't run that
    // per-peak search (would need a new "elapsed weeks since decay" concept to
    // turn accumulating weighted_score into a $-comparable rate — see the
    // archive doc), so this fixed constant approximates where the search
    // actually converges rather than a hand-picked guess.
    //
    // Caveat: since this now equals MIN_PEAK_SEPARATION_MIN, two accepted
    // peaks at exactly the minimum allowed separation would produce heavily
    // overlapping fixed windows (this overlap risk already existed at the old
    // 30min value, just less severely, since NMS only guarantees peak
    // *centers* are >= MIN_PEAK_SEPARATION_MIN apart, not that ±half-width
    // windows around them don't overlap). Not observed in practice — real
    // kept peaks in the reference run were consistently 70+ min apart.
    static constexpr int   PEAK_HALF_WIDTH_MIN     = 45;   // ±45 min max search width
    static constexpr int   MIN_PEAK_SEPARATION_MIN = 45;   // 9 buckets
    static constexpr int   PREHEAT_MINUTES         = 3;    // COLD_PIPE_DRAIN_MINUTES
    static constexpr float MIN_WEIGHTED_SCORE      = 6.0f;
    static constexpr float MIN_SCORE_FLOOR         = 3.0f;
    static constexpr int   MIN_OCCURRENCES         = 3;
    static constexpr float SCORE_STEP              = 1.0f;
    static constexpr int   SMOOTH_RADIUS           = 2;    // ±2 buckets
    static constexpr int   WIDTH_STEP_MIN          = 5;    // candidate half-width step

    // Real dollar costs used by buildSlots()'s per-peak width search — see
    // archive/OnDeviceDollarCostWindowSearch.md. Mirrors Logger/config.py +
    // navien_schedule_learner.py; there is no shared source between Python and
    // C++, so keep these in sync by hand if the underlying rates change.
    static constexpr float COLD_START_WASTE_USD = 0.097f;  // COLD_PIPE_DRAIN_MINUTES * AVG_FLOW_LPM * WATER_RATE_USD_PER_L
    static constexpr float RECIRC_WASTE_USD     = 0.0024f; // RECIRC_CYCLE_MINUTES/60 * RECIRC_PARTIAL_KCAL_HR * GAS_RATE_USD_PER_KCAL

    // Find schedule slots for one day using the adaptive threshold algorithm.
    //
    // day_buckets  : array of BUCKET_PER_DAY buckets representing one local
    //               calendar day (built by NavienLearner::RECOMPUTING from the
    //               UTC-indexed BucketStore via the current UTC offset).
    // out_slots    : caller-supplied array of at least MAX_PEAK_CANDIDATES entries.
    // elapsedWeeks : weeks since BucketFile::accumulation_start_epoch, snapshotted
    //               once per recompute pass by NavienLearner (RECOMPUTE_LOAD).
    //               Converts accumulating weighted_score into a $/week rate
    //               comparable to the flat per-cycle RECIRC_WASTE_USD.
    //
    // Returns the number of slots written (0 – MAX_PEAK_CANDIDATES), sorted
    // chronologically (ascending start_min) in local minutes-since-midnight.
    // No per-day cap is applied here; recomputeWrite() prunes to
    // MAX_SLOTS_PER_DAY per local day by score (now net dollar benefit, not
    // raw occurrence score — see TimeSlot::score) before converting to UTC.
    static int findDaySlots(const BucketFile::Bucket *day_buckets,
                            TimeSlot *out_slots, float elapsedWeeks);

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

    // Net dollar benefit of window [start_min,end_min) — covered_per_week *
    // COLD_START_WASTE_USD - gas_waste_usd — counting demand only from
    // buckets [lo_bucket,hi_bucket]. The scope and the window need not
    // match: bestSlotForPeak() passes a fixed ±MIN_PEAK_SEPARATION_MIN
    // neighbourhood around one peak regardless of candidate width (so a
    // different peak's demand doesn't distort this peak's search), while
    // buildSlots() re-scores an already-decided (possibly merged) window
    // by passing the window's own extent as the scope, so its reported
    // benefit reflects everything actually inside it.
    static float scoreWindow(const BucketFile::Bucket *day_buckets,
                             int lo_bucket, int hi_bucket,
                             int start_min, int end_min, float elapsedWeeks);

    // For one peak, search candidate half-widths (WIDTH_STEP_MIN steps, up to
    // PEAK_HALF_WIDTH_MIN) and return the one maximizing scoreWindow(), scoped
    // to ±MIN_PEAK_SEPARATION_MIN around the peak. Mirrors
    // navien_schedule_learner.py's _best_slot_for_peak().
    //
    // Returns false (out_slot untouched) if no candidate width has positive
    // net benefit — the peak isn't worth a slot at all.
    static bool bestSlotForPeak(const BucketFile::Bucket *day_buckets,
                                int peak_bucket, float elapsedWeeks,
                                TimeSlot *out_slot);

    // Convert accepted peaks to TimeSlot windows via bestSlotForPeak().
    // Drops peaks with non-positive net benefit. Adjacent peaks whose windows
    // overlap or touch are merged into one wider slot, re-scored via
    // scoreWindow() over the merged window's own full extent (see
    // archive/OnDeviceDollarCostWindowSearch.md) rather than summing the
    // pre-merge values, which would undercount demand outside either
    // original peak's narrow search scope. Returns number of slots written,
    // sorted chronologically.
    static int buildSlots(const BucketFile::Bucket *day_buckets,
                          const Peak *accepted, int n_accepted,
                          float elapsedWeeks, TimeSlot *out_slots);
};
