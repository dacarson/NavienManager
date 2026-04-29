# Branch: Remove-timezone-dependence — Merge Description

## Problem

`localtime()` was used throughout the firmware for schedule firing, bucket recording, and midnight recompute triggering. The Eve app's `guessTimeZone()` writes a TZ string to NVS whenever any Eve client connects. A remote Eve connection (e.g., from Hawaii while the device is in San Francisco) overwrites the stored TZ, causing schedules to fire at the wrong wall-clock time and bucket data to be recorded into the wrong day/slot for the duration of the bad TZ.

## Solution

Make all runtime-critical time comparisons UTC-native. TZ is now stored for display only — a wrong TZ corrupts the Eve schedule display but cannot cause schedules to fire at the wrong time or bucket data to land in the wrong slot.

## Changes by Phase

### Phase 1 — Foundation (`TimeUtils.h` / `TimeUtils.cpp`)

Added `proper_timegm()`: a TZ-free, reentrant `timegm()` using integer-arithmetic calendar→epoch conversion. Replaces the previous hack that temporarily set `TZ=UTC0` via `setenv` (not reentrant; corrupts state if interrupted). A host-side test harness (`TimeUtils_test.cpp`) validates epoch=0, 2025-01-01 00:00 UTC, PST/PDT crossover, UTC+9:30 half-hour zone, and the year-2038 boundary.

### Phase 2 — Learner & Buckets

- `NavienLearner::onNavienState()`: `localtime` → `gmtime` for `_runDow` and `_runBucket`. Bucket dow/bucket indices are now UTC.
- `NavienLearner::idleStep()`: replaced `localtime_r` midnight detection with a 24-hour elapsed timer (`_lastRecomputeTime24h`), gated on `now > 1700000000L` to ignore bogus pre-NTP epochs. The anchor is persisted to `measured.bin` so it survives reboots.
- `NavienLearner::decayCheck()` / `ingestBucketPayload()`: `localtime_r` → `gmtime_r`.
- `BucketStore::begin()`: `localtime` → `gmtime`.
- `BUCKET_SCHEMA_VERSION` bumped to 2; `MEASURED_SCHEMA_VERSION` bumped to 2. Both force a clean reset on first boot after flashing.
- Added `_scheduleIsUtc` gate in `FakeGatoScheduler` so UTC-indexed learner output cannot reach the still-local-time firing path until Phase 3 is applied.

### Phase 3 — Schedule System

- `FakeGatoScheduler`: removed the `setenv`-based `timegm()` hack; all callers now use `proper_timegm()`.
- Eve write path (`parseProgramData` / `WEEK_SCHEDULE`): `convertEveSlotsToUTC(_lastKnownUtcOffsetMin)` converts local-time Eve slots to UTC before storage. If `_utcOffsetKnown` is false when `WEEK_SCHEDULE` arrives, the packet is discarded (Eve resends after `CURRENT_TIME` establishes the offset).
- Eve read path (`loop()`): `getEffectiveOffsetMin()` converts stored UTC slots back to local time before sending to Eve. This prevents Eve from double-converting on the next write-back.
- `getEffectiveOffsetMin()`: prefers `_lastKnownUtcOffsetMin` (set from Eve's `CURRENT_TIME` packet); falls back to system TZ derived from `localtime_r`/`gmtime_r`.
- `sanitizeScheduleToLocalLimit()`: called only from `convertEveSlotsToUTC()` (Eve→device path). **Never** called from `setWeekScheduleFromJSON()` (JSON push path already UTC).
- `SchedulerBase`: `getNextState()` and `initializeCurrentState()` use `gmtime()` and `proper_timegm()`; removed `getenv("TZ") == 0` guard from `loop()`; updated TZ-missing log message.
- `NavienManager.ino`: replaced TZ+`getLocalTime()` gate with raw epoch check (`time(nullptr) > 1700000000L`).
- NVS `schedVersion` key added to `SAVED_DATA` namespace; checked in `FakeGatoScheduler::begin()` before applying `prog_send_data` to `weekSchedule[]`. Version != 1 clears slots and writes version 1; user must re-push schedule from Eve or Python after first boot.
- `setScheduleUtcMode(true)` called in `begin()` after migration check, enabling the learner→scheduler handoff suppressed during Phase 2.

### Phase 3 Bug Fix — `setWeekScheduleFromJSON()` slot-drop

`sanitizeScheduleToLocalLimit()` was incorrectly called from `setWeekScheduleFromJSON()`. The round-trip (UTC→local→UTC via `convertSlotsOffset`) iterated Eve days 0→6; a cross-day UTC slot from day N that rolled back into local day N-1 consumed one of the three available slot positions on local day N-1 before that day's own native UTC slots were processed, causing the last native slot to be silently dropped.

Example: Monday UTC 04:00 → Sunday local 21:00 (PDT) stole `slot[0]` of Sunday's local view. Sunday's UTC slots 13:50, 16:40, 18:00 filled `slot[1]`, `slot[2]`, and hit the 3-slot cap — Sunday UTC 18:00-19:00 was dropped and never fired.

Fix: `sanitizeScheduleToLocalLimit()` is now called only from `convertEveSlotsToUTC()`.

### Phase 3 Telnet Enhancement — `TelnetCommands.cpp`

The `scheduler` command now displays each slot as both local time and UTC: `HH:MM-HH:MM (UTC HH:MM-HH:MM)`. When the UTC day differs from the local calendar day (e.g. Monday UTC 04:00 stored as Sunday local 21:00 in PDT), `prev day` is appended to the annotation. Slots are sorted by local start time for readability.

### Phase 4 — Python Tools

- `navien_schedule_learner.py`: removed `local_tz` parameter from `_extract_cold_starts()` and `fetch_consumption_events()`; `dow` and `minute_of_day` now derived from UTC datetimes; removed `detect_local_timezone()` call from `main()`.
- `navien_bootstrap.py`: removed `detect_local_timezone()` call; updated `fetch_consumption_events()` call signature.
- `navien_bucket_export.py`: removed `local_tz` from `build_bucket_payload()` signature; sends `schema_version: 2` with UTC-indexed buckets.

## Post-Flash Steps Required

1. Flash firmware.
2. Verify weblog shows `schedVersion` migration reset message.
3. Apply Phase 4 Python changes (already done on this branch).
4. Run `python3 navien_bucket_export.py --push --replace` to re-seed UTC buckets from InfluxDB.
5. Open Eve app → Programs tab → re-save the week schedule (triggers `convertEveSlotsToUTC()` → NVS write).
6. Verify `telnet scheduler` shows correct local times with UTC annotation.
7. Verify Eve shows correct schedule times.
8. Wait for a scheduled recirc event and confirm it fires at the correct local wall-clock time.

## Files Changed

| File | Change |
|---|---|
| `TimeUtils.h` / `TimeUtils.cpp` | New — `proper_timegm()` implementation |
| `TimeUtils_test.cpp` | New — host-side unit test harness |
| `FakeGatoScheduler.h` / `.cpp` | UTC conversion, offset tracking, sanitize fix |
| `SchedulerBase.cpp` | `gmtime` / `proper_timegm`; remove TZ guard |
| `NavienLearner.h` / `.cpp` | UTC buckets; 24h elapsed recompute timer |
| `BucketStore.h` / `.cpp` | `gmtime`; schema version bump to 2 |
| `NavienManager.ino` | Epoch-check time-init gate |
| `TelnetCommands.cpp` | Local+UTC slot display with `prev day` annotation |
| `Logger/navien_schedule_learner.py` | Remove `local_tz`; UTC-native events |
| `Logger/navien_bootstrap.py` | Remove `local_tz` |
| `Logger/navien_bucket_export.py` | Remove `local_tz`; send `schema_version: 2` |
