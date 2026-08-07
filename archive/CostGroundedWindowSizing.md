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

## Design — On-device (`PeakFinder.h`)

No architecture change to `PeakFinder.cpp`/`NavienLearner.cpp`. After the
Python change lands, run `navien_bootstrap.py` (dry run) against real InfluxDB
history and observe the half-widths and effective thresholds the new $-search
converges to across representative days (strong daily peaks vs. sparse/weekend
ones). Hand-adjust `PeakFinder.h`'s `PEAK_HALF_WIDTH_MIN` (and
`MIN_WEIGHTED_SCORE`/`MIN_SCORE_FLOOR` only if the data clearly warrants it) to
sit in that observed range, with a comment citing the $0.097-vs-$0.0024 ratio
as the reasoning, e.g.:

```cpp
// Retuned toward the $-optimal widths navien_schedule_learner.py's per-peak
// cost search converges to on real usage data (demand event ≈$0.097 vs wasted
// recirc cycle ≈$0.0024 — see archive/OnDeviceScheduleLearner.md Goal section).
static constexpr int PEAK_HALF_WIDTH_MIN = ...;
```

## Verification

- Run `python3 navien_bootstrap.py` (dry run, no `--push`) before and after the
  change against real InfluxDB history; compare the printed per-day schedule,
  chosen window widths, and `total_waste_usd` from `estimate_schedule_cost`.
  Expect: windows widen around strong/reliable peaks, shrink or disappear
  around marginal/sparse ones, and total predicted waste (missed + gas) drops
  or stays flat relative to the old fixed-width schedule for the same data.
- If InfluxDB isn't reachable from this environment, build a small synthetic
  `raw_counts`/`weighted_scores` fixture (a couple of strong daily peaks, a
  couple of marginal ones, one pure-noise bucket) and call
  `buckets_to_windows()` directly to sanity-check the search picks sensible
  widths and correctly drops the noise peak.
- No existing automated tests cover `navien_schedule_learner.py` (none found
  under `Logger/`) — this stays a manual/dry-run verification.
- C++ side: constants-only change, no new logic — confirm it still compiles
  (existing build) and, if feasible, compare `PeakFinder::findDaySlots()`
  output against the same synthetic fixture used above for a sanity cross-check
  against the Python result shape (not required to match exactly, since the
  on-device version intentionally stays fixed-width).
