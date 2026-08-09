# On-Device Dollar-Cost-Aware Window Search

## Context

The firmware currently does zero dollar-cost accounting. `PeakFinder.h`'s
`PEAK_HALF_WIDTH_MIN = 45` is a static approximation of where
`navien_schedule_learner.py`'s per-peak `$`-search (`_best_slot_for_peak()`,
see `archive/CostGroundedWindowSizing.md`) empirically converged on real data
— every peak gets the same fixed width, and `NavienLearner::recomputeWrite()`
still ranks which peaks earn one of the 3 daily slots by raw `weighted_score`,
not dollar value.

As established when this was scoped out originally: running the real search
on-device is computationally trivial (tens of thousands of float ops per
weekly recompute, negligible on a 240MHz dual-core chip already doing more
work than that in `RECOMPUTING`). The actual blocker is that `weighted_score`
accumulates without bound within a year (no rolling window on-device, unlike
Python's ±4-week band), so it can't be compared against a *flat* per-cycle
gas cost without knowing how much elapsed time it built up over. This plan
adds a small elapsed-time reference to `BucketFile` to fix that, then ports
the real search to `PeakFinder.cpp`.

## Design

### 1. `BucketFile` schema change (`BucketStore.h` / `BucketStore.cpp`)

- Add `uint32_t accumulation_start_epoch;` to the `BucketFile` header, after
  `current_year`.
- Bump `BUCKET_SCHEMA_VERSION` 2 → 3. Per your call: **no migration path** —
  `BucketStore::load()`'s existing schema-mismatch handling already rejects
  the file and falls through to `initEmpty()`, exactly like today's "corrupt
  file" case. `buckets.bin` starts fresh on first boot with the new firmware;
  re-bootstrap with `navien_bootstrap.py --push` +
  `navien_bucket_export.py --push --replace` if you want it reseeded
  immediately instead of waiting ~2-4 weeks to re-stabilize.
- `initEmpty()` / `zeroBuckets()`: set `accumulation_start_epoch = time(nullptr)`
  (leave at 0 if the clock isn't valid yet — pre-NTP boot path already exists
  elsewhere in this codebase for exactly this reason).

### 2. Reset point: `NavienLearner::decayCheck()` (`NavienLearner.cpp`)

When the year rolls over, alongside the existing `weighted_score *= 2/3`
decay, also reset `accumulation_start_epoch = now`. This is the natural,
already-existing hook — decay already means "this data now counts as last
year's," so resetting the accumulation clock at the same moment keeps the two
concepts in sync.

### 3. Snapshot elapsed weeks per recompute pass (`NavienLearner.h` / `.cpp`)

- New member `float _recomputeElapsedWeeks;`
- Computed once in the `RECOMPUTE_LOAD` state — same place `_recomputeOffsetMin`
  is already snapshotted for the whole pass (`NavienLearner.cpp` ~line 356-372):
  ```cpp
  int64_t deltaSec = (int64_t)now - (int64_t)_store.data().accumulation_start_epoch;
  self->_recomputeElapsedWeeks = std::max(1.0f, (float)deltaSec / 604800.0f);
  ```
  Use signed 64-bit arithmetic for the subtraction (not a bare `uint32_t`
  difference) to avoid wraparound if `accumulation_start_epoch` is 0 or the
  clock has done something unexpected; clamp to a 1-week minimum either way.
- Pass `self->_recomputeElapsedWeeks` into the `PeakFinder::findDaySlots()`
  calls made during `RECOMPUTING` (`NavienLearner.cpp` ~line 389).

### 4. On-device dollar constants (`PeakFinder.h`)

Add as `constexpr float`, derived the same way as `Logger/config.py` +
`navien_schedule_learner.py`:
```cpp
// Mirrors Logger/config.py + navien_schedule_learner.py — no shared source
// between Python and C++, keep these in sync by hand if the rates change.
static constexpr float COLD_START_WASTE_USD = 0.097f;  // COLD_PIPE_DRAIN_MINUTES * AVG_FLOW_LPM * WATER_RATE_USD_PER_L
static constexpr float RECIRC_WASTE_USD     = 0.0024f; // RECIRC_CYCLE_MINUTES/60 * RECIRC_PARTIAL_KCAL_HR * GAS_RATE_USD_PER_KCAL
```

### 5. Port the per-peak width search (`PeakFinder.h` / `PeakFinder.cpp`)

- `findDaySlots()` gains a `float elapsedWeeks` parameter.
- **Keep the adaptive-threshold peak-detection loop unchanged** (occurrence
  floor + score threshold + smoothing + NMS) — same statistical noise filter
  as today, a different job from `$` balancing.
- Replace `buildSlots()`'s fixed `±PEAK_HALF_WIDTH_MIN` construction with a
  per-peak search mirroring `_best_slot_for_peak()`, but scored directly from
  `day_buckets` array indexing (no dict needed on-device — simpler than the
  Python version):
  - Loop candidate half-widths `5..PEAK_HALF_WIDTH_MIN` step 5.
  - Build the candidate slot using the existing `win_start`/`win_end`/
    `preheat_minutes`/10-min-rounding logic, reused as-is.
  - `covered_raw` = sum of `raw_count` for buckets inside the slot, scanning
    only `±MIN_PEAK_SEPARATION_MIN` around the peak (matches Python's local
    scoping rationale — a neighboring peak's demand shouldn't distort this
    peak's evaluation).
  - `covered_per_week = covered_raw / elapsedWeeks`.
  - `gas_waste_usd` = (count of empty 15-min sub-windows inside the slot,
    i.e. every bucket in that sub-window has `weighted_score == 0`) ×
    `RECIRC_WASTE_USD`.
  - `net_benefit = covered_per_week * COLD_START_WASTE_USD - gas_waste_usd`.
  - Keep the best-scoring half-width; **store `net_benefit` into
    `TimeSlot::score`**, replacing the raw peak score it holds today.
  - Drop peaks whose best `net_benefit <= 0` — don't emit a `TimeSlot` at all.

**Key insight — minimal blast radius:** `NavienLearner::recomputeWrite()`'s
existing `MAX_SLOTS_PER_DAY` pruning loop (`NavienLearner.cpp` ~line 587-617)
already sorts `_weekSlots[local_dow]` by `.score` descending and keeps the
top 3. Since `.score` now holds net dollar benefit instead of raw occurrence
score, that pruning becomes `$`-based automatically — **no changes needed in
`recomputeWrite()` itself.**

### 6. Test harness (`test/PeakFinder_test.cpp`)

Extend the synthetic fixture (lives outside the sketch root — see the file's
own header comment for why) to pass `elapsedWeeks`, and add cases that
exercise the new behavior:
- A peak with a high raw score but evaluated at a long `elapsedWeeks` (so its
  per-week rate is modest) should come out narrower than the old fixed 45min,
  or possibly get dropped if `net_benefit <= 0`.
- A peak that would have been marginal under the old raw-score ranking but
  has clearly positive net benefit should still get a slot.
- Confirm slots stay well-formed and non-overlapping, same checks as today.

### Known approximation (document in code comments)

Right after a year-rollover decay, `weighted_score` still contains a mix of
"old, now-scaled-by-2/3" and "new, since-reset" contributions, while
`elapsedWeeks` resets to ~0 (clamped to 1 week). `covered_per_week` is
therefore transiently overstated right after rollover and self-corrects as
the new year's weeks accumulate — the same category of approximation already
accepted for the Python-side simplifications documented in
`archive/CostGroundedWindowSizing.md`.

## Verification — done

- Implemented as designed: `BucketFile::accumulation_start_epoch` (schema
  v2→v3, no migration — see Deployment note), reset in both
  `BucketStore::initEmpty()`/`zeroBuckets()` and `NavienLearner::decayCheck()`
  on year rollover; `NavienLearner::_recomputeElapsedWeeks` snapshotted once
  per pass in `RECOMPUTE_LOAD`; `PeakFinder::bestSlotForPeak()` added,
  `buildSlots()` rewritten to call it per accepted peak and drop peaks with
  non-positive net benefit; `TimeSlot::score` repurposed to hold net dollar
  benefit, so `NavienLearner::recomputeWrite()`'s existing sort-by-score
  `MAX_SLOTS_PER_DAY` pruning became `$`-based with no changes needed there,
  exactly per the "minimal blast radius" design.
- Host-side: `test/PeakFinder_test.cpp` extended with an `elapsedWeeks`
  sensitivity check — same raw demand at `elapsedWeeks=1` vs `elapsedWeeks=52`
  must report a substantially smaller net dollar benefit (the test requires
  at least a 10x drop). Result: `$3.4920 → $0.0672`, a ~52x drop, matching
  the formula almost exactly (the gas-cost term is negligible at this narrow
  a width, so the ratio comes through nearly undiluted). An initial attempt
  at also asserting the chosen *width* narrows under dilution was dropped —
  it required either a fixture dense enough to trip a real, pre-existing
  quirk in `findPeaks()`'s smoothing (closely-spaced qualifying buckets can
  produce an unqualifying "shoulder" bucket with a higher smoothed score than
  the true peak, suppressing peak detection entirely — not something this
  change introduced or fixed), or numbers small enough that `RECIRC_WASTE_USD`
  ($0.0024/cycle) meaningfully competes with `COLD_START_WASTE_USD`
  ($0.097/event), which isn't reliably reproducible in a handful of
  hand-crafted buckets. The dollar-value assertion is the more fundamental,
  robust property and is what's checked instead.
  Run: `g++ -std=c++14 -I . -I test/test_stubs -o test/PeakFinder_test test/PeakFinder_test.cpp PeakFinder.cpp && ./test/PeakFinder_test`.
- Full ESP32 build: **not yet confirmed for this change.** The earlier
  Arduino IDE build you confirmed was for the previous commit (the
  `PEAK_HALF_WIDTH_MIN` retune, before this on-device `$`-search work
  started) — this feature's actual firmware build still needs to be
  attempted before flashing.
- Not yet done: on-device confirmation after flashing. `learnerStatus`
  (Telnet) and the web status page will show the new per-day predicted
  efficiency using the retuned windows; the existing
  `WEBLOG("LEARNER local-day prune: ...")` line already logs each kept/dropped
  slot's score, now showing dollar-scaled values — useful for sanity-checking
  on a live device without new instrumentation.

## Deployment note

Per your call on the schema-bump question: flashing this wipes
`buckets.bin` (same as today's existing "corrupt file" handling, unchanged
behavior — just now also triggered by the version bump). Either let it
re-learn over ~2-4 weeks, or run `navien_bootstrap.py --push` then
`navien_bucket_export.py --push --replace` right after flashing to reseed
immediately.

### Reseeding correctly incorporates and decays previous years — with one fix required

Tracing this end-to-end after writing it surfaced two real bugs in the
reseed path, both now fixed:

1. **`navien_bucket_export.py` sent `"schema_version": 2`, now hardcoded to 3.**
   With `BUCKET_SCHEMA_VERSION` bumped to 3, every `POST /buckets` would have
   been rejected outright (`ingestBucketPayload()`'s schema check returns -1
   on mismatch) — the exact reseed command in this deployment note would have
   failed immediately.
2. **`accumulation_start_epoch` was left at "now" on a `--replace` reseed.**
   `BucketStore::initEmpty()` sets it fresh at boot, but the bootstrap payload
   then injects `raw`/`score` values representing real history from
   `navien_bucket_export.py`'s `±window_weeks` band (default ±8 weeks,
   chosen to stay within one DST period — see that script) `×
   len(recency_weights)` years (default 2) — up to ~32 weeks of real data. Left
   untouched, the next recompute would have computed `elapsedWeeks ≈ 1` (the
   true time since boot) and divided that 32-weeks' worth of `raw_count` by 1,
   inflating `covered_per_week` by roughly 32x and producing abnormally wide
   windows until the next annual decay corrected it. Fixed by adding a
   `weeks_represented` field to the payload (`2 * window_weeks *
   len(recency_weights)`, the same "instances of a weekday observed" logic as
   `navien_schedule_learner.py`'s `historical_days`) that `ingestBucketPayload()`
   uses to seed `accumulation_start_epoch = now - weeks_represented weeks` on
   replace. See the updated `POST /buckets` JSON format in `BEHAVIOR_SPEC.md`.

**Does year-weighting carry through correctly?** Yes, independent of the two
bugs above: `navien_bucket_export.py`'s exported `score` field already comes
from `events_to_minutes()`, which sums `combined_weight = recency_weight *
demand_weight` per event — `recency_weight` being the same per-year
multiplier (current year ×3, previous year ×2 by default) the live on-device
decay applies over time. So a reseed's `weighted_score` values are already
recency-weighted exactly as if the device had accumulated and decayed that
history itself; no separate decay step is needed or missing on ingest.

**Residual approximation, unchanged from the rest of this design:**
`weeks_represented` is a single flat number applied to a `weighted_score` that
internally blends multiple recency-weighted years together (current year at
full weight, prior years already discounted) — same category of imprecision
as the "known approximation" above for the ordinary post-decay case. It's a
reasonable single number for the on-device rate calculation, not an exact
per-year reconstruction, and self-corrects the same way: at the next annual
decay, `accumulation_start_epoch` resets to a precise `now` and the
approximation window shrinks back to zero.
