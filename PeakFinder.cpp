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

#include "PeakFinder.h"
#include "HomeSpan.h"
#include <string.h>

// Bucket duration in minutes — 288 buckets × 5 min = 1440 min/day.
static constexpr int BUCKET_MINUTES = 5;

// ---------------------------------------------------------------------------
// roundNearest10() — helper
//
// Implements Python 3's round(x / 10) * 10 (banker's rounding / round-half-
// to-even) for non-negative integers.
//
// At half-step boundaries (x mod 10 == 5), rounds to whichever multiple of
// 10 is even:
//   round(35/10)*10 = 40  (3→4, even)
//   round(25/10)*10 = 20  (2→2, even)
//   round(125/10)*10 = 120 (12→12, even)
//   round(135/10)*10 = 140 (13→14, even)
//
// Only win_end hits half-step boundaries (peak_min + 30 where peak_min is a
// multiple of 5; if peak_min mod 10 == 5 then win_end mod 10 == 5).
// start_min (peak_min - 33, after clamp) has remainder 7 or 2 mod 10 and
// never lands on a half-step, so the +5 trick suffices for start_min.
// ---------------------------------------------------------------------------
static int roundNearest10(int x) {
    int q = x / 10;
    int r = x % 10;
    if (r < 5) return q * 10;        // unambiguously round down
    if (r > 5) return (q + 1) * 10;  // unambiguously round up
    // r == 5: round to even (banker's rounding)
    return ((q % 2) == 0) ? q * 10 : (q + 1) * 10;
}

// ---------------------------------------------------------------------------
// findDaySlots() — public entry point
// Mirrors Python buckets_to_windows() adaptive threshold loop.
// ---------------------------------------------------------------------------

int PeakFinder::findDaySlots(const BucketFile::Bucket *day_buckets,
                              TimeSlot *out_slots, float elapsedWeeks) {
    const int sep_buckets = MIN_PEAK_SEPARATION_MIN / BUCKET_MINUTES; // 9

    // Adaptive threshold: two-phase loop.
    //   Phase 1: step score threshold down, keep MIN_OCCURRENCES.
    //   Phase 2: if score floor reached with < MAX_SLOTS_PER_DAY, also try
    //            MIN_OCCURRENCES-1 (weakest useful signal).
    //
    // Search target is MAX_SLOTS_PER_DAY (3): findDaySlots is now called on a
    // local-day bucket view (not a UTC-day view), so each call sees a true
    // 24 h local window and there is no need to find extra candidates for an
    // adjacent day's portion.  recomputeWrite() prunes to MAX_SLOTS_PER_DAY
    // per local day by score before converting to UTC.
    //
    // n_best / best[] are only updated when findPeaks() returns a non-empty
    // result, so a previously-found set is preserved when qualifying buckets
    // momentarily disappear at a lower threshold.
    int  n_best = 0;
    Peak best[MAX_PEAK_CANDIDATES];

    int occ_floors[2] = { MIN_OCCURRENCES, MIN_OCCURRENCES - 1 };
    if (occ_floors[1] < 1) occ_floors[1] = 1;

    for (int oi = 0; oi < 2 && n_best < MAX_SLOTS_PER_DAY; oi++) {
        int   occ_floor = occ_floors[oi];
        float threshold = MIN_WEIGHTED_SCORE;

        while (threshold >= MIN_SCORE_FLOOR) {
            Peak candidates[MAX_PEAK_CANDIDATES];
            int  n = findPeaks(day_buckets, threshold, occ_floor,
                               sep_buckets, candidates);

            // Only overwrite the best result when we find something non-empty.
            if (n > 0) {
                n_best = n;
                memcpy(best, candidates, n * sizeof(Peak));
            }

            if (n_best >= MAX_SLOTS_PER_DAY) {
                break;
            }
            if (threshold <= MIN_SCORE_FLOOR) {
                break;
            }
            float next = threshold - SCORE_STEP;
            threshold  = (next < MIN_SCORE_FLOOR) ? MIN_SCORE_FLOOR : next;
        }

        if (n_best >= MAX_SLOTS_PER_DAY) {
            break;  // satisfied — don't relax occurrences further
        }
    }

    if (n_best == 0) {
        return 0;
    }

    // Rank by score descending.  No UTC-day cap here — recomputeWrite() prunes
    // to MAX_SLOTS_PER_DAY per LOCAL day.  Sort so scores are deterministic
    // regardless of NMS output order.
    // Uses `<` comparison so equal scores preserve relative (insertion) order —
    // a stable sort, matching Python's timsort stability for equal float scores.
    for (int i = 1; i < n_best; i++) {
        Peak key = best[i];
        int  j   = i - 1;
        while (j >= 0 && best[j].score < key.score) {
            best[j + 1] = best[j];
            j--;
        }
        best[j + 1] = key;
    }
    // No UTC-day cap: all NMS-surviving candidates are returned so that
    // recomputeWrite() can prune to MAX_SLOTS_PER_DAY per LOCAL day instead,
    // correctly handling usage patterns that straddle a UTC-day boundary.
    return buildSlots(day_buckets, best, n_best, elapsedWeeks, out_slots);
}

// ---------------------------------------------------------------------------
// findPeaks() — private
// Mirrors Python _find_peaks() called with the filtered hot_weighted dict.
// ---------------------------------------------------------------------------

int PeakFinder::findPeaks(const BucketFile::Bucket *day_buckets,
                           float threshold, int occ_floor, int sep_buckets,
                           Peak *out_accepted) {
    // Rule 2: static arrays live in BSS — no stack pressure.
    // filtered[b] holds the raw weighted_score for qualifying buckets, 0 elsewhere.
    // smoothed[b] holds the sliding-average of filtered[].
    static float filtered[BUCKET_PER_DAY];
    static float smoothed[BUCKET_PER_DAY];

    // --- Step 1: build filtered score array ---
    // Mirrors: hot_weighted = {b: day_weighted[b] for b in day_raw
    //                          if day_raw[b] >= occ_floor
    //                          and day_weighted[b] >= threshold}
    bool any_qualifying = false;
    for (int b = 0; b < BUCKET_PER_DAY; b++) {
        if (day_buckets[b].raw_count >= (uint16_t)occ_floor &&
            day_buckets[b].weighted_score >= threshold) {
            filtered[b] = day_buckets[b].weighted_score;
            any_qualifying = true;
        } else {
            filtered[b] = 0.0f;
        }
    }
    if (!any_qualifying) {
        return 0;
    }

    // --- Step 2: smooth ---
    // Python: smoothed[b] = sum(score_map.get(b + d*5, 0) for d in -r..+r) / (2r+1)
    // Out-of-range neighbors contribute 0; denominator is always 2r+1 = 5.
    const float denom = (float)(2 * SMOOTH_RADIUS + 1);
    for (int b = 0; b < BUCKET_PER_DAY; b++) {
        float sum = 0.0f;
        for (int d = -SMOOTH_RADIUS; d <= SMOOTH_RADIUS; d++) {
            int nb = b + d;
            if (nb >= 0 && nb < BUCKET_PER_DAY) {
                sum += filtered[nb];
            }
            // out-of-range → contributes 0 (implicit)
        }
        smoothed[b] = sum / denom;
    }

    // --- Step 3: find local maxima ---
    // Only consider buckets that passed the filter (filtered[b] > 0).
    // A bucket is a local maximum if its smoothed score is >= every neighbor
    // within ±sep_buckets (neighbors not in filtered[] have smoothed = 0).
    Peak candidates[MAX_PEAK_CANDIDATES];
    int  n_candidates = 0;

    for (int b = 0; b < BUCKET_PER_DAY && n_candidates < MAX_PEAK_CANDIDATES; b++) {
        if (filtered[b] == 0.0f) {
            continue;  // not a qualifying bucket
        }
        float s = smoothed[b];
        if (s == 0.0f) {
            continue;
        }
        bool is_local_max = true;
        for (int d = -sep_buckets; d <= sep_buckets && is_local_max; d++) {
            if (d == 0) continue;
            int   nb   = b + d;
            float nb_s = (nb >= 0 && nb < BUCKET_PER_DAY) ? smoothed[nb] : 0.0f;
            if (nb_s > s) {
                is_local_max = false;
            }
        }
        if (is_local_max) {
            candidates[n_candidates++] = { b, filtered[b] };
        }
    }

    if (n_candidates == 0) {
        return 0;
    }

    // --- Step 4: greedy NMS ---
    // Sort candidates by raw score descending (insertion sort; ≤MAX_PEAK_CANDIDATES
    // elements).
    for (int i = 1; i < n_candidates; i++) {
        Peak key = candidates[i];
        int  j   = i - 1;
        while (j >= 0 && candidates[j].score < key.score) {
            candidates[j + 1] = candidates[j];
            j--;
        }
        candidates[j + 1] = key;
    }

    // Accept all non-suppressed candidates up to MAX_PEAK_CANDIDATES.
    // Python has no hard cap here; MAX_PEAK_CANDIDATES (32) covers the
    // theoretical maximum local maxima for 45-min separation in a 288-bucket
    // day (real-world is 2–5 per day).  Caller sorts by score; local-day
    // pruning to MAX_SLOTS_PER_DAY happens in recomputeWrite().
    int n_accepted = 0;
    for (int i = 0; i < n_candidates; i++) {
        bool ok = true;
        for (int j = 0; j < n_accepted && ok; j++) {
            int dist = candidates[i].bucket - out_accepted[j].bucket;
            if (dist < 0) dist = -dist;
            if (dist < sep_buckets) {
                ok = false;
            }
        }
        if (ok && n_accepted < MAX_PEAK_CANDIDATES) {
            out_accepted[n_accepted++] = candidates[i];
        }
    }

    return n_accepted;
}

// ---------------------------------------------------------------------------
// bestSlotForPeak() — private
// Mirrors navien_schedule_learner.py's _best_slot_for_peak(): search
// candidate half-widths around one peak and pick the one maximizing net
// dollar benefit. See archive/OnDeviceDollarCostWindowSearch.md.
//
// Window construction for each candidate half-width mirrors the original
// fixed-width buildSlots() exactly:
//   win_start = max(0,    peak_bucket - half_width)   [minutes]
//   win_end   = min(1439, peak_bucket + half_width)
//   start_min = max(0,    win_start   - preheat_minutes), rounded to 10min
//   win_end   = min(1430, round(win_end / 10) * 10)   (banker's rounding)
// ---------------------------------------------------------------------------

bool PeakFinder::bestSlotForPeak(const BucketFile::Bucket *day_buckets,
                                  int peak_bucket, float elapsedWeeks,
                                  TimeSlot *out_slot) {
    const int sep_buckets = MIN_PEAK_SEPARATION_MIN / BUCKET_MINUTES;
    const int lo_bucket = (peak_bucket - sep_buckets < 0)
                          ? 0 : peak_bucket - sep_buckets;
    const int hi_bucket = (peak_bucket + sep_buckets >= BUCKET_PER_DAY)
                          ? BUCKET_PER_DAY - 1 : peak_bucket + sep_buckets;
    const int peak_min = peak_bucket * BUCKET_MINUTES;

    float best_benefit = 0.0f;  // not worth a slot unless some width beats "no slot"
    int   best_start   = -1;
    int   best_end     = -1;

    for (int half_width = WIDTH_STEP_MIN; half_width <= PEAK_HALF_WIDTH_MIN;
         half_width += WIDTH_STEP_MIN) {
        int win_start = peak_min - half_width;
        if (win_start < 0) win_start = 0;
        int win_end = peak_min + half_width;
        if (win_end > 1439) win_end = 1439;

        int start_min = win_start - PREHEAT_MINUTES;
        if (start_min < 0) start_min = 0;
        start_min = ((start_min + 5) / 10) * 10;

        int end_min = roundNearest10(win_end);
        if (end_min > 1430) end_min = 1430;

        // covered_raw: sum raw_count for buckets inside this candidate slot,
        // restricted to the local scope (±MIN_PEAK_SEPARATION_MIN) so a
        // neighbouring peak's demand doesn't distort this peak's evaluation.
        uint32_t covered_raw = 0;
        for (int b = lo_bucket; b <= hi_bucket; b++) {
            int bmin = b * BUCKET_MINUTES;
            if (bmin >= start_min && bmin < end_min) {
                covered_raw += day_buckets[b].raw_count;
            }
        }
        float covered_per_week = (float)covered_raw / elapsedWeeks;

        // gas_waste_usd: empty 15-min sub-windows inside the slot (every
        // bucket in that sub-window has weighted_score == 0), same local
        // scope restriction.
        int wasted_cycles = 0;
        for (int m = start_min; m < end_min; m += 15) {
            int  sub_end     = m + 15;
            bool has_demand  = false;
            for (int b = lo_bucket; b <= hi_bucket; b++) {
                int bmin = b * BUCKET_MINUTES;
                if (bmin >= m && bmin < sub_end &&
                    day_buckets[b].weighted_score > 0.0f) {
                    has_demand = true;
                    break;
                }
            }
            if (!has_demand) wasted_cycles++;
        }
        float gas_waste_usd = (float)wasted_cycles * RECIRC_WASTE_USD;

        float net_benefit = covered_per_week * COLD_START_WASTE_USD - gas_waste_usd;
        if (net_benefit > best_benefit) {
            best_benefit = net_benefit;
            best_start   = start_min;
            best_end     = end_min;
        }
    }

    if (best_start < 0) {
        return false;  // no candidate width beat "no slot"
    }

    out_slot->start_min = (uint16_t)best_start;
    out_slot->end_min   = (uint16_t)best_end;
    out_slot->score     = best_benefit;
    return true;
}

// ---------------------------------------------------------------------------
// buildSlots() — private
// For each statistically-accepted peak (from findPeaks()'s adaptive
// threshold), runs bestSlotForPeak() to search its $-optimal window and
// drops it entirely if no width has positive net benefit. Adjacent peaks
// whose windows overlap or touch are merged into one wider slot (summing
// net benefit) rather than kept as separate, redundant slots — mirrors
// navien_schedule_learner.py's _merge_overlapping_candidates(). Windows are
// produced in non-decreasing start order by construction (peak centers are
// always >= MIN_PEAK_SEPARATION_MIN apart, which is >= PEAK_HALF_WIDTH_MIN),
// so a single linear pass against the immediately-preceding output slot
// suffices — no need to re-sort after merging.
// ---------------------------------------------------------------------------

int PeakFinder::buildSlots(const BucketFile::Bucket *day_buckets,
                            const Peak *accepted, int n_accepted,
                            float elapsedWeeks, TimeSlot *out_slots) {
    // Sort accepted peaks chronologically (ascending bucket) so surviving
    // slots come out chronologically ordered, matching findDaySlots()'s
    // documented return contract.
    // n_accepted must be <= MAX_PEAK_CANDIDATES; findDaySlots() no longer caps
    // at MAX_SLOTS_PER_DAY — pruning now happens per local day in recomputeWrite().
    Peak chrono[MAX_PEAK_CANDIDATES];
    memcpy(chrono, accepted, n_accepted * sizeof(Peak));

    // Insertion sort by bucket index.
    for (int i = 1; i < n_accepted; i++) {
        Peak key = chrono[i];
        int  j   = i - 1;
        while (j >= 0 && chrono[j].bucket > key.bucket) {
            chrono[j + 1] = chrono[j];
            j--;
        }
        chrono[j + 1] = key;
    }

    int n_slots = 0;
    for (int i = 0; i < n_accepted; i++) {
        TimeSlot slot;
        if (!bestSlotForPeak(day_buckets, chrono[i].bucket, elapsedWeeks, &slot)) {
            continue;
        }
        if (n_slots > 0 && slot.start_min <= out_slots[n_slots - 1].end_min) {
            // Overlaps (or touches) the previous slot — merge instead of
            // appending. net benefit is approximated as the sum of the two
            // parts' (not a fresh evaluation of the wider window) — same
            // category of approximation as elsewhere in this search; see
            // archive/OnDeviceDollarCostWindowSearch.md.
            TimeSlot &prev = out_slots[n_slots - 1];
            if (slot.end_min > prev.end_min) prev.end_min = slot.end_min;
            prev.score += slot.score;
        } else {
            out_slots[n_slots++] = slot;
        }
    }

    return n_slots;
}
