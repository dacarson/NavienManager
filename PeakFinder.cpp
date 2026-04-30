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
#include <stdio.h>
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
                              TimeSlot *out_slots) {
    const int sep_buckets = MIN_PEAK_SEPARATION_MIN / BUCKET_MINUTES; // 9

    // Adaptive threshold: two-phase loop matching Python exactly.
    //   Phase 1: step score threshold down, keep MIN_OCCURRENCES.
    //   Phase 2: if score floor reached with < MAX_SLOTS_PER_DAY, also try
    //            MIN_OCCURRENCES-1 (weakest useful signal).
    //
    // n_best / best[] mirror Python's `peaks` variable: they are only updated
    // when findPeaks() returns a non-empty result, so a previously-found set
    // of peaks is preserved if a later threshold iteration finds no qualifying
    // buckets — matching Python's `if hot_weighted: peaks = _find_peaks(...)`.
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
            // This preserves a prior non-zero result when qualifying buckets
            // momentarily disappear at a new (higher) starting threshold in
            // the occ_floor=2 pass — matching Python's `if hot_weighted:` guard.
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

    // Rank by score descending, keep top MAX_SLOTS_PER_DAY.
    // Mirrors Python: ranked = sorted(peaks, key=score, reverse=True)[:MAX_SLOTS_PER_DAY]
    // Explicit sort here rather than relying on NMS output order, so that
    // "top N by score" is a provable guarantee independent of NMS internals.
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
    if (n_best > MAX_SLOTS_PER_DAY) {
        int original_count = n_best;
        int pruned_count   = original_count - MAX_SLOTS_PER_DAY;
        float kept_min_score    = best[MAX_SLOTS_PER_DAY - 1].score;
        float dropped_best_score = best[MAX_SLOTS_PER_DAY].score;
        float dropped_worst_score = best[original_count - 1].score;
        printf("LEARNER PeakFinder pruned slots: candidates=%d kept=%d pruned=%d kept_min=%.2f dropped_best=%.2f dropped_worst=%.2f\n",
               original_count, MAX_SLOTS_PER_DAY, pruned_count,
               kept_min_score, dropped_best_score, dropped_worst_score);
        n_best = MAX_SLOTS_PER_DAY;
    }

    return buildSlots(best, n_best, out_slots);
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
    // day (real-world is 2–5 per day).  Caller explicitly sorts by score and
    // truncates to MAX_SLOTS_PER_DAY after the adaptive loop.
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
// buildSlots() — private
// Mirrors the Python window-building loop inside buckets_to_windows():
//
//   win_start = max(0,    peak_bucket - peak_half_width)   [minutes]
//   win_end   = min(1439, peak_bucket + peak_half_width)
//   start_min = max(0,    win_start   - preheat_minutes)
//   start_min = round(start_min / 10) * 10
//   win_end   = min(1430, round(win_end / 10) * 10)
//
// Rounding notes:
//   win_end: uses roundNearest10() (banker's rounding) to match Python's
//   round() exactly.  For odd bucket indices, win_end mod 10 == 5 so the
//   tie-breaking rule matters.
//
//   start_min: peak_min - 33 has remainder 7 (even bucket) or 2 (odd bucket)
//   mod 10 — never a half-step boundary — so +5 integer rounding matches
//   Python's round() in all reachable cases.
// ---------------------------------------------------------------------------

int PeakFinder::buildSlots(const Peak *accepted, int n_accepted,
                            TimeSlot *out_slots) {
    // Sort accepted peaks chronologically (ascending bucket).
    // Copy to a local array so we can sort without modifying the caller's.
    // n_accepted must be <= MAX_SLOTS_PER_DAY; enforced by findDaySlots().
    Peak chrono[MAX_SLOTS_PER_DAY];
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

    for (int i = 0; i < n_accepted; i++) {
        int peak_min = chrono[i].bucket * BUCKET_MINUTES;  // minute-of-day

        int win_start = peak_min - PEAK_HALF_WIDTH_MIN;
        if (win_start < 0) win_start = 0;

        int win_end = peak_min + PEAK_HALF_WIDTH_MIN;
        if (win_end > 1439) win_end = 1439;

        // Apply preheat (start recirc early to warm the pipes).
        int start_min = win_start - PREHEAT_MINUTES;
        if (start_min < 0) start_min = 0;

        // Round start_min to nearest 10-min boundary.
        // start_min mod 10 is always 7 or 2 (never 5), so +5 rounding
        // matches Python's round() exactly.
        start_min = ((start_min + 5) / 10) * 10;

        // Round win_end to nearest 10-min boundary using banker's rounding
        // to match Python's round() exactly, then cap at 1430.
        win_end = roundNearest10(win_end);
        if (win_end > 1430) win_end = 1430;

        out_slots[i].start_min = (uint16_t)start_min;
        out_slots[i].end_min   = (uint16_t)win_end;
        out_slots[i].score     = chrono[i].score;
    }

    return n_accepted;
}
