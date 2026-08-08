# Cost-Grounded Recirculation Window Sizing

## Context

`NavienLearner`/`navien_schedule_learner.py` picks up to 3 recirculation windows
per day to balance two costs: a homeowner getting cold water at the tap (water
waste) against recirculation running gas with no one home to use the hot water
(gas waste). `Logger/config.py` already defines the real dollar rates for both
(`GAS_RATE_USD_PER_KCAL`, `WATER_RATE_USD_PER_L`), and
`Logger/navien_schedule_learner.py` derives `COLD_START_WASTE_USD` (~$0.097/cold
start) and `RECIRC_WASTE_USD` (~$0.0024/wasted cycle) from them — but tracing the
code shows these two numbers are never actually weighed against each other where
it matters:

- The per-event `cost_multiplier` in `_extract_demand_events()`
  (`navien_schedule_learner.py:239-255`) normalizes `COLD_START_WASTE_USD`
  against itself (`_COST_REF = COLD_START_WASTE_USD`), so it's always `1.0` (or
  `0.5` for short cold-pipe taps that `demand_weight` already halves). It's dead
  weight — `RECIRC_WASTE_USD` never enters this calculation at all.
- Window width (`PEAK_HALF_WIDTH_MIN = 30`) and the noise-filter thresholds
  (`MIN_WEIGHTED_SCORE = 6.0`, `MIN_SCORE_FLOOR = 3.0`) are fixed constants,
  identical in both `navien_schedule_learner.py` and the on-device
  `PeakFinder.h`/`PeakFinder.cpp` port. They were hand-picked, not derived from
  the ~40:1 real-dollar ratio between a demand event and a wasted cycle.
- The dollar constants *are* used, but only in `_score_day()` /
  `estimate_schedule_cost()` — a **post-hoc reporting** function that scores a
  schedule after `buckets_to_windows()` has already built it. The $ accounting
  exists; it just never feeds back into the decision.

Goal of this change: make window width and slot selection actually driven by
the $0.097-vs-$0.0024 ratio, using the cost accounting that already exists in
`_score_day()`, rather than fixed constants. Per the discussion: implement the
real dynamic $-search in the Python reference script only (`navien_bootstrap.py`
/ `navien_schedule_learner.py`), since it has clean, already-correct dollar
accounting via `historical_days` normalization. The on-device C++ port
(`PeakFinder.cpp`) keeps its existing fixed-width architecture — adding true
dynamic $ search there would require a new "elapsed weeks since last decay"
concept in `BucketFile` to convert accumulating `weighted_score` into a rate
comparable to a flat per-cycle gas cost, which is more embedded-side risk than
this change is meant to take on. Instead, `PeakFinder.h`'s constants get
hand-retuned to match what the new Python search converges to on real data.

`MAX_SLOTS_PER_DAY = 3` is a fixed Eve/HomeKit limit and is not touched.

## Design — Python (`Logger/navien_schedule_learner.py`)

**1. Fix `historical_days` to actually reflect the query window.**
It's currently hardcoded to `16` everywhere it appears (`_score_day`,
`estimate_schedule_cost`, `compare_slot_widths`) regardless of
`--window_weeks`/`--recency_weights`. `16` only happens to be correct for the
CLI defaults (`window_weeks=4`, 2 recency-weight years:
`2 × 4 × 2 = 16`). Since bootstrap mode overrides `window_weeks=52`, and this
value is about to become load-bearing (not just a reporting cosmetic), compute
it properly: `historical_days = 2 * args.window_weeks * len(args.recency_weights)`
and thread it into `buckets_to_windows()` as a new parameter, replacing the
hardcoded default.

**2. Remove the dead `cost_multiplier`.**
In `_extract_demand_events()` (lines 239-255), delete the `cost_multiplier` block
and go back to `combined_weight = recency_weight * demand_weight`. It never did
anything (always evaluated to `1.0`/`0.5`, already captured by `demand_weight`);
keeping it next to the *real* cost-based logic below would be confusing dead
code implying a balance that isn't happening at the event-weighting stage. The
real $ balancing now happens at the window-building stage (next step).

**3. Add a per-peak width search, reusing `_score_day()`.**
New helper, e.g. `_best_slot_for_peak()`:
- Scope `day_raw`/`day_weighted` to a local window around the peak (e.g.
  `±min_peak_separation` minutes) so a neighboring peak's demand doesn't leak
  into this peak's cost evaluation.
- For each candidate half-width (5-min steps, e.g. 10..60, capped at
  `min_peak_separation` so windows can't run into a neighbor by construction),
  build the slot dict exactly as today's `buildSlots`-equivalent logic does
  (peak ± half-width, minus `preheat_minutes`, rounded to 10-min boundaries),
  and call `_score_day(local_raw, local_weighted, [slot], historical_days,
  hot_window_min)` to get `total_waste_usd`.
- Also evaluate the zero-slot baseline (`_score_day(..., [], ...)`) for that
  same local window.
- Return the half-width with the lowest `total_waste_usd`, plus
  `net_savings_usd = baseline_cost - best_cost`.

**4. Replace fixed-width `buildSlots` + score-based ranking in
`buckets_to_windows()`.**
For each day: keep the existing adaptive-threshold peak-detection loop
unchanged (occurrence floor + score threshold + smoothing + NMS remain the
statistical noise filter — that's a different job from $ balancing and doesn't
need to change). For each accepted peak, run the width search from step 3.
Drop any peak whose best `net_savings_usd <= 0` (not worth a slot at all — this
subsumes some of what the score floor was approximating, but now grounded in
real dollars). Rank the survivors by `net_savings_usd` descending (not raw
`weighted_score`) and keep the top `MAX_SLOTS_PER_DAY`.

**5. Update `archive/OnDeviceScheduleLearner.md`.**
The "What This Does Not Attempt" bullet — *"No cost multipliers
(`COLD_START_WASTE_USD` / `RECIRC_WASTE_USD` scaling). These are constant
multipliers that wash out in the normalized score."* — is no longer accurate
for the Python reference script once this lands. Update it to describe the new
reality: the reference script now uses both rates to size and rank windows;
the on-device port still uses fixed, hand-tuned constants for the reasons
above.

## Design — On-device (`PeakFinder.h`) — done

No architecture change to `PeakFinder.cpp`/`NavienLearner.cpp`, as planned.
Ran `navien_bootstrap.py` (dry run) against a full year of real InfluxDB
history at several `--peak_half_width` caps (30, 40, 60) to find where the
per-peak $-search actually converges rather than guessing:

| Cap tried | Kept-slot half-widths (21 slots = 3/day × 7 days) |
|---|---|
| 30 | 16×30 (ceiling), 4×25, 1×20 — mostly hitting the cap |
| 40 | 14×40 (ceiling), 7×35 — still mostly hitting the cap |
| 60 | 1×35, 5×40, 8×45, 5×50, 2×55 — spread, no longer pinned to the cap |

At cap 60 the search stops hugging the ceiling and settles into a real
distribution: mode and mean both land at **~45 minutes**. This makes sense —
`_best_slot_for_peak()`'s local search scope is bounded at
`±min_peak_separation` (45min), and real demand trickles in across a wider
span than a sharp single-bucket spike, so covered demand keeps growing out
toward that full scope for most peaks rather than plateauing early.

`PeakFinder.h`'s `PEAK_HALF_WIDTH_MIN` was retuned from 30 to **45** to match,
with a comment recording this convergence data and a caveat: since it now
equals `MIN_PEAK_SEPARATION_MIN`, two accepted peaks at exactly the minimum
allowed separation could produce heavily overlapping fixed windows (already a
latent risk at 30, just less severe) — not observed in practice, since real
kept peaks were consistently 70+ min apart. `MIN_WEIGHTED_SCORE`/
`MIN_SCORE_FLOOR` were left unchanged: `threshold=6.0` (the unrelaxed default)
held for all 7 days across the full-year run, so the adaptive threshold never
needed to relax — a full year of data doesn't stress that noise filter.

The Python side's own `--peak_half_width` default was also raised from 30 to
45 in both `navien_schedule_learner.py` and `navien_bootstrap.py` — leaving it
at 30 would have artificially capped the search below its own empirical
optimum on every normal (non-override) run.

## Verification — done

- Ran `python3 navien_bootstrap.py` (dry run, no `--push`) against real
  InfluxDB history at multiple `--peak_half_width` caps; compared chosen
  window widths and `net_savings` per peak (see table above). Confirmed via
  `python3 navien_schedule_learner.py` (default ±4-week window) that the
  post-fix search produces sensible, non-empty schedules — the pre-fix
  baseline-comparison bug (see `_best_slot_for_peak()`'s docstring) caused
  every day to come back with zero slots before the formula was corrected.
- InfluxDB was reachable from this environment for the whole session, so the
  synthetic-fixture fallback wasn't needed for the Python side.
- C++ side: `arduino-cli` is not available in this environment, so the actual
  ESP32 build was not compiled with it. A real Arduino IDE build *was* tried
  and caught a real mistake: `PeakFinder_test.cpp` was first placed directly
  in the sketch root (repo root) alongside `TimeUtils_test.cpp`. The
  Arduino/ESP32 builder auto-compiles every `.cpp` file it finds there into
  the firmware, so two host-test files each defining `main()` collided at
  link time (`multiple definition of 'main'`). Fixed by moving
  `PeakFinder_test.cpp` and its `test_stubs/HomeSpan.h` stub into a `test/`
  subfolder, which the Arduino builder does not auto-sweep. `TimeUtils_test.cpp`
  remains in the sketch root (pre-existing, harmless on its own since it was
  the only stray `main()` there) — worth relocating too at some point, but
  that's a separate, un-requested change.
- Added `test/PeakFinder_test.cpp` — a host-side g++ test harness (same
  pattern as `TimeUtils_test.cpp`), built with a synthetic 288-bucket day
  (one strong peak, one moderate, one at the score floor, plus filtered
  noise) and a stub `test/test_stubs/HomeSpan.h` since `PeakFinder.cpp`'s
  `#include "HomeSpan.h"` is unused dead weight it never actually calls
  into. Confirms `PeakFinder::findDaySlots()` still compiles and produces
  well-formed, non-overlapping slots after the `PEAK_HALF_WIDTH_MIN` retune.
  Run (from repo root):
  `g++ -std=c++14 -I . -I test/test_stubs -o test/PeakFinder_test test/PeakFinder_test.cpp PeakFinder.cpp && ./test/PeakFinder_test`.
  Not a correctness oracle against the Python reference — the on-device port
  intentionally keeps a fixed-width architecture, so exact output parity was
  never the goal.
