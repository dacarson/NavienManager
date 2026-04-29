# UTC Migration Plan: NavienManager Timezone-Independence

## Goal

Make NavienManager timezone-independent at runtime. Currently `localtime()` is used throughout, making the system vulnerable to `guessTimeZone()` writing a wrong TZ when an Eve/HomeKit app connects from a remote timezone (e.g. Hawaii overwrites SF timezone, breaking schedule firing and bucket recording for the duration).

## Target State

| Concern | Before | After |
|---|---|---|
| Schedule slots stored | Local hour:min | UTC hour:min |
| Schedule fire comparison | `localtime()` vs local slot | `gmtime()` vs UTC slot |
| Bucket index | `localtime()` hour:min | `gmtime()` hour:min |
| Midnight recompute | local `tm_hour==0` detection | 24h elapsed timer |
| TZ in NVS | Runtime-critical | Display-only |
| Wrong TZ consequence | Fires at wrong time | Shows wrong time to user only |

## Impact

- A wrong `guessTimeZone()` (remote Eve connection) corrupts only the display of schedule times on the Eve app and webpage — it cannot cause schedules to fire at the wrong time.
- Bucket data will be invalidated by the firmware change and must be re-seeded via `navien_bucket_export.py --push --replace` after flashing.
- After flashing, the user must re-push the schedule once from the Eve app (or via `navien_bootstrap.py --push`) to re-populate NVS with UTC-converted slots.

---

## New Files

### `TimeUtils.h` / `TimeUtils.cpp`

A TZ-free `timegm()` equivalent. The current `timegm()` hack in `FakeGatoScheduler.cpp` temporarily sets `TZ=UTC0` via `setenv`, which is not reentrant and corrupts state if interrupted. Replace with arithmetic:

```cpp
// TimeUtils.h
#pragma once
#include <time.h>
time_t proper_timegm(const struct tm *t);
```

> **Sketch only — do not implement as shown.** The snippet below uses `30.6001` floating-point, which Phase 1 explicitly rejects in favour of integer arithmetic. Follow Phase 1's implementation guidance; the sketch is retained only to illustrate the algorithm structure.

```cpp
// TimeUtils.cpp — ILLUSTRATIVE ONLY; use integer Fliegel-Van Flandern arithmetic in practice
time_t proper_timegm(const struct tm *t) {
    int y = t->tm_year + 1900, m = t->tm_mon + 1;
    if (m <= 2) { y--; m += 12; }
    long days = (long)(365 * y) + (y/4) - (y/100) + (y/400)
              + (long)(30.6001 * (m + 1)) + t->tm_mday - 719591L;  // ← replace with integer
    return (time_t)(days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec);
}
```

Use a **unit-tested** integer calendar→epoch implementation and validate against known `struct tm` ↔ UTC instants on the ESP32 toolchain; `time_t` is signed 32‑bit on many Arduino builds, so document the supported year range.

Include from `SchedulerBase.cpp` and `FakeGatoScheduler.cpp`.

---

## Firmware Changes

### `FakeGatoScheduler.h`

- Add private member `int _lastKnownUtcOffsetMin = 0;`
- Add private method `void convertEveSlotsToUTC(int utcOffsetMin);`

### `FakeGatoScheduler.cpp`

**`timegm()` (lines 34–56)** — Remove entirely. Replace all callers with `proper_timegm()` from `TimeUtils.h`.

**`addMilliseconds()` (lines 177–202)** — Uses `mktime()` / `localtime()` on Eve's wire-format time struct. This struct is display-only (sent back to Eve). Keep as-is; add comment: `// Eve wire time — intentionally local-time`.

**`guessTimeZone()` (lines 204–238)** — No structural change. TZ is still stored in NVS for display. Add comment: `// TZ is now display-only; wrong TZ corrupts Eve display but not schedule firing.`

**`parseProgramData()` — `CURRENT_TIME` case (line 429)** — After copying `currentTime` into `prog_send_data`, compute and store the UTC offset:
```cpp
struct tm eveUTC = {0};
eveUTC.tm_year = currentTime->year + 100;
eveUTC.tm_mon  = currentTime->month - 1;
eveUTC.tm_mday = currentTime->day;
eveUTC.tm_hour = currentTime->hours;
eveUTC.tm_min  = currentTime->minutes;
time_t evePseudo = proper_timegm(&eveUTC);   // treat Eve local as UTC
time_t sysUTC    = time(nullptr);
_lastKnownUtcOffsetMin = (int)(difftime(sysUTC, evePseudo) / 60.0 + 0.5);
```
Sign: for PST (UTC-8), sysUTC=15:00 and evePseudo=07:00, so `_lastKnownUtcOffsetMin = +480`. UTC = local + offset.

**`parseProgramData()` — `WEEK_SCHEDULE` case (line 411)** — After copying Eve's schedule, call `convertEveSlotsToUTC(_lastKnownUtcOffsetMin)` before `updateSchedulerWeekSchedule()`.

**Packet ordering** — `_lastKnownUtcOffsetMin` is updated in the `CURRENT_TIME` case. If a single `parseProgramData()` buffer can deliver `WEEK_SCHEDULE` before `CURRENT_TIME`, offset may still be stale for that parse pass. Mitigations: (a) process TLVs in an order that applies offset before conversion when both appear in one buffer, or (b) if offset is unknown, defer conversion until after the full buffer is parsed, or (c) on `WEEK_SCHEDULE` only, recompute offset from `guessTimeZone()`’s existing `timegm`/pseudo-UTC vs `time(nullptr)` logic so a lone week packet still converts.

**New `convertEveSlotsToUTC(int utcOffsetMin)`** — Converts `prog_send_data.weekSchedule` slots in-place from Eve local-time to UTC. Algorithm:
1. Snapshot the current `prog_send_data.weekSchedule`; zero-fill a fresh copy.
2. For each Eve day `d` (0=Mon..6=Sun), for each slot where `offset_start != 0xFF`:
   - `local_start = slot.offset_start * 10` (minutes since local midnight)
   - `local_end   = slot.offset_end   * 10`
   - `utc_start   = local_start + utcOffsetMin`
   - `utc_end     = local_end   + utcOffsetMin`
   - Map to SchedulerBase day: `sb_day = (d + 1) % 7`
   - If `utc_start < 0` or `utc_start >= 1440`, adjust `sb_day` by ±1 and normalise minutes.
   - Slot straddles UTC midnight (start and end in different days): truncate at 23:50/00:00 and log a warning.
   - Round to nearest 10-minute boundary; write into first free slot of target day.
3. Overwrite `prog_send_data.weekSchedule` with the UTC-converted copy.

**`updateCurrentScheduleIfNeeded()` (line 474)** — Keeps `localtime()`. This selects which day's schedule to show in the Eve app (display). Add comment: `// Intentionally localtime — selects which local day to show Eve.`

**`stateChange()` log print (line 122)** — Uses `localtime()` for the "Next event scheduled for:" message. Keep as-is; display only.

### `SchedulerBase.cpp`

**`begin()` (lines 319–373)** — Update the log line that says schedules will not run until TZ is set; after migration, TZ is optional for firing (still used for display).

**Schedule migration must not live only here** — `FakeGatoScheduler` overrides `loadScheduleFromStorage()` / `saveScheduleToStorage()` to no-ops and treats **`prog_send_data` in NVS namespace `SAVED_DATA`** as the source of truth. `FakeGatoScheduler::begin()` then calls `updateSchedulerWeekSchedule()`, which copies `prog_send_data.weekSchedule` into in-RAM `weekSchedule[]`. Any `initDefault()` + `saveScheduleToStorage()` sequence in `SchedulerBase::begin()` therefore does **not** persist through boot: the stubbed save is a no-op, and the child `begin()` immediately reapplies the old blob. The UTC migration reset (version key + cleared or default schedule + NVS commit) must run in **`FakeGatoScheduler`’s constructor or `begin()`**, on the **`savedData` (`SAVED_DATA`)** handle, **before** the final `updateSchedulerWeekSchedule()` that applies `prog_send_data` to `weekSchedule[]`.

**`loop()` TZ guard (line 434)** — Remove `if (getenv("TZ") == 0) { isInitialized = false; return; }`. TZ is no longer required for firing.

**`initializeCurrentState()` (line 390)** — `localtime(&now)` → `gmtime(&now)`.

**`getNextState()` (lines 157, 173, 188, 195, 213, 218):**
- Line 157: `localtime(&endVacationTime)` → `gmtime(&endVacationTime)`
- Line 173: `localtime(&now)` → `gmtime(&now)`
- Lines 194 & 217: `tm_isdst = -1` → `tm_isdst = 0`
- Lines 195 & 218: `mktime(&nextState_tm)` → `proper_timegm(&nextState_tm)`

### `NavienLearner.h`

- Replace `int _lastRecomputeYday` with `time_t _lastRecomputeTime24h`.
- Update constructor initialiser: `_lastRecomputeYday(-1)` → `_lastRecomputeTime24h(0)`.

### `NavienLearner.cpp`

**`onNavienState()` (line 171)** — `localtime(&now)` → `gmtime(&now)` for `_runDow` and `_runBucket`.

**`idleStep()` (lines 436–448)** — Replace `localtime_r` midnight detection with 24h elapsed timer (same `now > 1700000000L` guard as startup decay so bogus pre-NTP epochs are not anchored):
```cpp
time_t now = time(nullptr);
if (now > 1700000000L) {
    if (_lastRecomputeTime24h == 0) {
        // First eligible tick — persist immediately so a reboot within the first
        // 24h window anchors from this point rather than resetting to zero.
        _lastRecomputeTime24h = now;
        saveMeasured();
    }
    // Guard NTP backward jump: a forward jump fires one recompute early at most.
    if (_lastRecomputeTime24h > now) _lastRecomputeTime24h = now;
    if (now - _lastRecomputeTime24h >= 86400L) {
        _lastRecomputeTime24h = now;
        struct tm tm_buf;
        struct tm *t = gmtime_r(&now, &tm_buf);
        if (t->tm_wday == 0)
            advanceMeasuredWeek();  // advanceMeasuredWeek() calls saveMeasured()
        else
            saveMeasured();         // persist updated 24h anchor between weekly saves
        _taskState = DECAY_CHECK;
    }
}
```
Remove `_lastRecomputeYday` usage. `_lastRecomputeTime24h` is persisted in `MeasuredFile` as `int64_t` (future-safe if `time_t` widens; ESP32 platform limit is 2038 regardless) and restored by `loadMeasured()` on boot.

**`decayCheck()` (line 476)** — `localtime_r` → `gmtime_r`.

**`ingestBucketPayload()` (line 656)** — `localtime` → `gmtime`.

**`appendStatusHTML()` (line 784)** — Keep `localtime_r`; display only. Add comment.

### `BucketStore.cpp`

**`begin()` (line 67)** — `localtime(&now)` → `gmtime`. Year is TZ-independent for any reasonable offset, but consistency with `decayCheck()` requires both use the same reference.

Also bump `BUCKET_SCHEMA_VERSION` (in `BucketStore.h`) from 1 to 2 to force a clean reset of `buckets.bin` on first boot after firmware flash.

### `NavienManager.ino`

**`loop()` (lines 114–121)** — Replace TZ+`getLocalTime()` gate with epoch check:
```cpp
if (!timeInit) {
    if (time(nullptr) > 1700000000L) {
        timeInit = true;
        homeSpan.assumeTimeAcquired();
    }
}
```

---

## What Stays Local-Time (Display Only)

| Location | Data | Reason |
|---|---|---|
| `addMilliseconds()` / `prog_send_data.currentTime` | Eve wire time struct | Sent back to Eve for display |
| `updateCurrentScheduleIfNeeded()` | Day-of-week selection | Which local day Eve shows |
| `stateChange()` log message | Next event timestamp | Human-readable serial |
| `appendStatusHTML()` | Last recompute timestamp | Webpage display |
| `TelnetCommands.cpp` (all) | All time displays | Human-readable output |
| `guessTimeZone()` / NVS TZ | TZ string | Convert stored UTC slots → local for Eve readback |
| `HomeSpanWeb.ino` | Status strings | Display-only local labels (optional later: UTC labels or explicit “local”) |

---

## NVS / Storage Migration

| Store | Key | Change |
|---|---|---|
| NVS `SAVED_DATA` / `PROG_SEND_DATA` (`prog_send_data`, includes `weekSchedule`) | Existing local-time Eve slots | Reset or clear on boot if `schedVersion != 1` (see **FakeGatoScheduler** above); re-push from Eve or Python required |
| NVS `SAVED_DATA` / `schedVersion` (new) | `uint8_t` | Prefer this namespace so it ships with the blob the firmware actually reloads; set to `1` after migration reset |
| NVS `SCHEDULER` / `weekSchedule` | Legacy blob | `FakeGatoScheduler` does not load/save this path today; optional cleanup or bump for hygiene, but **not sufficient alone** for migration |
| `/navien/buckets.bin` | `BUCKET_SCHEMA_VERSION` bump to 2 | Forces `initEmpty()` on first boot; re-seed via `navien_bucket_export.py --push --replace` |
| `/navien/measured.bin` | `MEASURED_SCHEMA_VERSION` bump to 2 | Clean reset; 4-week window repopulates within a week |

---

## Python Changes

### `navien_schedule_learner.py`

**`_extract_cold_starts()` / `fetch_consumption_events()`:**
- Remove `local_tz` parameter.
- Remove `dt_utc.astimezone(local_tz)` conversion; keep timestamps as UTC `datetime`.
- `dow` and `minute_of_day` now derived from UTC datetime.

**`events_to_minutes()`:**
- `dt` is now UTC. `minute_of_day = dt.hour * 60 + dt.minute` gives UTC minute. Correct for UTC buckets.

**`main()`:**
- Remove `local_tz, tz_name = detect_local_timezone()` and the timezone print.
- Update `fetch_consumption_events(args, local_tz)` → `fetch_consumption_events(args)`.
- Update display strings from local-time labels to UTC.

**`POST /schedule` output** — `buckets_to_windows()` result is now UTC-minute-indexed; `week` JSON contains UTC hours/minutes. No schema change; `setWeekScheduleFromJSON()` on the firmware stores verbatim (already UTC).

### `navien_bootstrap.py`

- Remove `local_tz, tz_name = nsl.detect_local_timezone()` (line 101) and print.
- Update `nsl.fetch_consumption_events(args, local_tz)` → `nsl.fetch_consumption_events(args)`.

### `navien_bucket_export.py`

- Remove `local_tz` from `build_bucket_payload()` signature (line 36).
- Remove `nsl.detect_local_timezone()` call (line 175) and print.
- Update `build_bucket_payload(args, local_tz, ...)` → `build_bucket_payload(args, ...)`.
- Send `schema_version: 2` in the payload to match the bumped `BUCKET_SCHEMA_VERSION`.

---

## Protocol Change Summary

| Endpoint | Before | After |
|---|---|---|
| `POST /schedule` (Python) | local-time hour:min | UTC hour:min (no schema change) |
| `POST /buckets` (Python) | local-time dow/bucket | UTC dow/bucket; `schema_version: 2` |
| Eve BLE `WEEK_SCHEDULE` write | local-time → stored local | local-time → converted to UTC → stored UTC |
| Eve BLE schedule read | stored local returned verbatim | stored UTC → converted to local → returned (requires explicit encode path in firmware wherever `ProgramData` is assembled for readback — verify all read paths) |

---

## Implementation Phases

The migration is split into four independently flashable phases. Each phase can be verified before starting the next. Phases 2 and 3 each require a post-flash step; Phase 4 is Python-only and requires no firmware flash.

> **Why this order?** Phase 1 proves `proper_timegm()` before anything depends on it. Phase 2 makes learner/buckets UTC-native and gates the learner→scheduler apply path so UTC-indexed slots cannot reach the still-local-time firing path; schedule behavior is unchanged. Phase 3 is the highest-risk change and is tackled last on a proven foundation. Phase 4 must be applied before Phase 2's post-flash step 3 (`navien_bucket_export.py --push --replace`) — the device will reject a `schema_version: 1` payload after `BUCKET_SCHEMA_VERSION` is bumped to 2, and the old script produces local-time bucket indices that would contradict the UTC firmware.

---

### Phase 1 — Foundation
**Goal:** Add `proper_timegm()` with no behavioral change. Independently verifiable.

**Files:**
- `TimeUtils.h` *(new)*
- `TimeUtils.cpp` *(new)*

**Work:**
- Implement integer-arithmetic `proper_timegm()` (no `30.6001` float; no TZ env touch).
- Write table-driven test harness covering: epoch=0, 2025-01-01 00:00 UTC, PST/PDT crossover, UTC+9:30 half-hour zone, year 2038 boundary.
- Document supported `time_t` year range (ESP32 Arduino: signed 32-bit, overflows 2038).

**Post-flash verification:** Compile-only; no flash needed. Run test harness on host or ESP32 serial.

---

### Phase 2 — Learner & Buckets
**Goal:** Bucket recording and daily recompute become UTC-based. Schedule firing is unchanged: the learner→scheduler apply path is gated behind `_scheduleIsUtc` (set false until Phase 3) so UTC-indexed learner output cannot reach the still-local-time firing path.

**Files:**
- `NavienLearner.cpp`
- `NavienLearner.h`
- `BucketStore.cpp`
- `BucketStore.h`
- `FakeGatoScheduler.h` *(add `_scheduleIsUtc = false` gate)*
- `FakeGatoScheduler.cpp` *(guard learner handoff on `_scheduleIsUtc`)*

**Work:**
- `onNavienState()` line 171: `localtime` → `gmtime` for `_runDow` and `_runBucket`.
- `idleStep()` lines 436–448: replace `localtime_r` midnight detection with 24h elapsed timer; gate the whole block on `now > 1700000000L` (plausible NTP-synced epoch); UTC Sunday check for `advanceMeasuredWeek()`.
- `decayCheck()` line 476: `localtime_r` → `gmtime_r`.
- `ingestBucketPayload()` line 656: `localtime` → `gmtime`.
- `BucketStore::begin()` line 67: `localtime` → `gmtime`.
- Bump `BUCKET_SCHEMA_VERSION` to 2 (forces `initEmpty()` on first boot).
- Bump `MEASURED_SCHEMA_VERSION` to 2 (clean reset; 4-week window repopulates within a week).
- Replace `_lastRecomputeYday` with `_lastRecomputeTime24h` in `NavienLearner.h` and constructor.
- Add `int64_t last_recompute_24h` to `MeasuredFile`; persist in `saveMeasured()`, restore in `loadMeasured()`.
- Call `saveMeasured()` whenever the 24h branch fires (non-Sunday path); `advanceMeasuredWeek()` already calls it on Sunday.
- Add `_scheduleIsUtc = false` and `setScheduleUtcMode(bool)` to `FakeGatoScheduler`; gate learner→`setWeekScheduleFromJSON` handoff on `_scheduleIsUtc` in `loop()`. A `// TODO(Phase 3)` comment in `begin()` marks where to call `setScheduleUtcMode(true)` after the `schedVersion` migration check.

> **Mixed-state note:** Between flashing Phase 2 and Phase 3, buckets are UTC-indexed while schedule firing remains local-time. The learner→scheduler auto-apply path (`FakeGatoScheduler::loop()` → `setWeekScheduleFromJSON()`) is **suppressed** until Phase 3 sets `_scheduleIsUtc = true` — learner recomputes run and produce UTC slots, but those slots are not applied to the firing path until the scheduler is also UTC-aware. This prevents UTC-indexed learner output from being interpreted as local-time and firing at the wrong wall-clock time.

**Post-flash steps:**
1. Verify device boots and weblog shows bucket schema reset.
2. **Prerequisite: Phase 4 must be applied first.** `navien_bucket_export.py` must send `schema_version: 2` with UTC-indexed buckets. The pre-Phase-4 script sends local-time indices and `schema_version: 1`, which the device will reject (schema mismatch). Apply Phase 4 Python changes before running this step.
3. Run `python3 navien_bucket_export.py --push --replace` to re-seed UTC buckets from InfluxDB.
4. Monitor for 24–48 hours: confirm recompute fires roughly 24h after startup (not at wrong local hour). Note: `_lastRecomputeTime24h` is persisted to `measured.bin` whenever the 24h branch fires (and on every `advanceMeasuredWeek()` / OTA / `saveLearner`), so the anchor survives reboots and the first post-reboot interval is at most 24h from the last persisted value.

---

### Phase 3 — Schedule System
**Goal:** Schedule slots stored and fired in UTC. Eve write path converts local→UTC; Eve read path converts UTC→local. TZ becomes display-only.

**Must ship as a single flash** — changing the firing path to expect UTC slots and the write path to store UTC slots must be atomic.

**Files:**
- `TimeUtils.h` / `TimeUtils.cpp` *(Phase 1 prerequisite)*
- `FakeGatoScheduler.h`
- `FakeGatoScheduler.cpp`
- `SchedulerBase.cpp`
- `NavienManager.ino`

**Work:**

*`FakeGatoScheduler.h`:*
- Add `int _lastKnownUtcOffsetMin = 0;`
- Add `bool _utcOffsetKnown = false;`
- Add `void convertEveSlotsToUTC(int utcOffsetMin);`
- Add `int getEffectiveOffsetMin() const;`
- Add `void sanitizeScheduleToLocalLimit(int offsetMin);`

*`FakeGatoScheduler.cpp`:*
- Remove `timegm()` TZ-swap hack (lines 34–56); replace all callers with `proper_timegm()`.
- `parseProgramData()` `CURRENT_TIME` case: compute and store `_lastKnownUtcOffsetMin`; set `_utcOffsetKnown = true`; clamp to ±720 min; log warning if out of range.
- `parseProgramData()` `WEEK_SCHEDULE` case: call `convertEveSlotsToUTC(_lastKnownUtcOffsetMin)` before `updateSchedulerWeekSchedule()`. If `_utcOffsetKnown` is false, discard the received schedule and log a warning — Eve will re-send after a `CURRENT_TIME` packet establishes the offset.
- Packet ordering mitigation: if `WEEK_SCHEDULE` arrives before `CURRENT_TIME` in the same buffer, recompute offset inline from `prog_send_data.currentTime` vs `time(nullptr)` rather than using the stale member. Note: this is only reliable if `prog_send_data.currentTime` was refreshed in the same or a recent session; a stale value from a prior connection is the same residual risk noted elsewhere in the plan.
- Implement `convertEveSlotsToUTC()` with day-rollover and midnight-truncation logic. Calls `sanitizeScheduleToLocalLimit()` after converting.
- Add `getEffectiveOffsetMin()` helper: returns `_lastKnownUtcOffsetMin` if `_utcOffsetKnown`, otherwise derives the offset from `localtime_r` / `gmtime_r` on the current epoch (system TZ fallback). Returns `INT_MIN` if neither is available.
- Add `sanitizeScheduleToLocalLimit(int offsetMin)`: round-trips `prog_send_data.weekSchedule` through UTC→local→UTC via `convertSlotsOffset`. Any UTC slot whose local-time projection would exceed the 3-slot Eve UI limit on its local day is dropped from storage, preventing it from firing silently. **Called only from `convertEveSlotsToUTC()` (the Eve→device path). Never called from `setWeekScheduleFromJSON()`.** See post-implementation note below.
- `convertSlotsOffset()`: enforce 3-slot cap per output day (Eve UI limit is 3, not 4); round slot offsets to nearest 10-minute boundary (`(val + 5) / 10`) instead of truncating to avoid 1-minute jitter in `_lastKnownUtcOffsetMin` shifting a slot to the wrong 10-minute bucket.
- `FakeGatoScheduler::begin()`: add `schedVersion` check on `SAVED_DATA` handle before `updateSchedulerWeekSchedule()`; if version != 1, clear slots and write version 1. After confirming slots are UTC-valid, call `setScheduleUtcMode(true)` to enable the learner→scheduler handoff (replaces the `// TODO(Phase 3)` stub from Phase 2).
- `loop()` Eve readback: use `getEffectiveOffsetMin()` instead of `_lastKnownUtcOffsetMin` directly. This ensures the device sends correct local-time slots to Eve on first boot before any `CURRENT_TIME` packet has established `_lastKnownUtcOffsetMin` — without the fallback, `_lastKnownUtcOffsetMin` defaults to 0 and Eve receives raw UTC times, which it then double-converts on the next write-back.
- `addMilliseconds()`: add comment — intentionally local-time (Eve wire struct).
- `guessTimeZone()`: add comment — TZ is now display-only.
- `updateCurrentScheduleIfNeeded()`: add comment — intentionally `localtime()`.

*`SchedulerBase.cpp`:*
- `initializeCurrentState()` line 390: `localtime` → `gmtime`.
- `getNextState()` lines 157, 173: `localtime` → `gmtime`.
- `getNextState()` lines 194, 217: `tm_isdst = -1` → `0`; `mktime` → `proper_timegm`.
- `loop()` line 434: remove `getenv("TZ") == 0` guard.
- `begin()`: update TZ-missing log message to reflect display-only status.

*`NavienManager.ino`:*
- `loop()` lines 114–121: replace TZ+`getLocalTime()` gate with raw epoch check (`time(nullptr) > 1700000000L`).

**Post-flash steps:**
1. Verify weblog shows `schedVersion` migration reset message.
2. Open Eve app → Programs tab → re-save the week schedule (triggers `parseProgramData()` → `convertEveSlotsToUTC()` → NVS write).
3. Verify telnet `schedule` command shows correct local times with UTC annotation (confirms UTC→local display conversion).
4. Verify Eve shows correct schedule times.
5. Wait for a scheduled recirc event and confirm it fires at the correct local wall-clock time.

**Post-Phase-3 additions (not in original plan):**

*Telnet `scheduler` command enhancements (`TelnetCommands.cpp`):*
- Slots are now displayed as both local time and UTC: `HH:MM-HH:MM (UTC HH:MM-HH:MM)`.
- When a UTC slot's local time falls on the previous calendar day (e.g. Monday UTC 04:00 = Sunday local 21:00 in PDT), the display appends `prev day` to the UTC annotation. Slots are sorted by local start time for readability.

*Bug fix — `setWeekScheduleFromJSON()` incorrectly called `sanitizeScheduleToLocalLimit()`:*

When `navien_bucket_export.py --push` pushes a JSON schedule, slots are already in UTC with ≤3 per UTC day. The sanitize round-trip (UTC→local→UTC via `convertSlotsOffset`) was nevertheless applied, and it dropped valid same-day UTC slots. The failure mode:

`convertSlotsOffset` iterates Eve days 0→6. A cross-day UTC slot from day N that rolls back into local day N-1 is processed *before* day N-1's own native slots. It fills one of the three available slot positions on local day N-1. When day N-1's native UTC slots are subsequently processed, the 3-slot cap is hit one slot early and the last native slot is silently dropped.

Example: Monday UTC 04:00 → Sunday local 21:00 (PDT). This steals `slot[0]` of Sunday's local view. Sunday's own UTC slots 13:50, 16:40, 18:00 then fill `slot[1]`, `slot[2]`, and attempt `slot[3]` — which is capped. Sunday UTC 18:00-19:00 is dropped from storage and never fires.

**Fix:** `sanitizeScheduleToLocalLimit()` is called only from `convertEveSlotsToUTC()` (the Eve→device path, where Eve sends local-time slots that genuinely require sanitization after UTC conversion). It is never called from `setWeekScheduleFromJSON()`. JSON-pushed UTC schedules need no sanitization: they are already within the 3-slot-per-UTC-day limit, and the Monday 04:00 UTC slot is fully visible in Eve (displayed as 21:00 on Monday's tab in local time) — the cross-day mapping is display-only, not a storage problem.

---

### Phase 4 — Python Tools
**Goal:** Bootstrap and bucket-export scripts generate UTC indices; no local timezone dependency.

**No firmware flash required.** Must be applied before Phase 2's post-flash step 3 — `navien_bucket_export.py --push --replace` must send `schema_version: 2` with UTC-indexed buckets or the device will reject the payload. Can otherwise be applied independently of Phase 3.

**Files:**
- `Logger/navien_schedule_learner.py`
- `Logger/navien_bootstrap.py`
- `Logger/navien_bucket_export.py`

**Work:**

*`navien_schedule_learner.py`:*
- `_extract_cold_starts()`: remove `local_tz` parameter; keep timestamps as UTC `datetime`.
- `fetch_consumption_events()`: remove `local_tz` parameter.
- `events_to_minutes()`: `dow` and `minute_of_day` now derived from UTC `datetime`. Update display strings from local-time labels to UTC.
- `main()`: remove `detect_local_timezone()` call and timezone print.

*`navien_bootstrap.py`:*
- Remove `nsl.detect_local_timezone()` call (line 101) and print.
- Update `nsl.fetch_consumption_events(args, local_tz)` → `nsl.fetch_consumption_events(args)`.

*`navien_bucket_export.py`:*
- Remove `local_tz` from `build_bucket_payload()` signature (line 36).
- Remove `nsl.detect_local_timezone()` call (line 175) and print.
- Update `build_bucket_payload(args, local_tz, ...)` → `build_bucket_payload(args, ...)`.
- Send `schema_version: 2` in payload.

**Post-run verification:**
- Dry-run `navien_bootstrap.py` and confirm peak times look correct in UTC (SF morning peaks should appear around 14:00–16:00 UTC).
- Run `navien_bucket_export.py --push --replace` to re-seed with UTC buckets.

---

## CLAUDE.md Additions (after implementation)

Add to Architecture notes:

**UTC-native internal storage**
- `SchedulerBase::weekSchedule[]` stores UTC hour:minute. `getNextState()` and `initializeCurrentState()` use `gmtime()` and `proper_timegm()` (from `TimeUtils.h`).
- `NavienLearner` cold-start bucket assignment uses `gmtime()`. `buckets.bin` dow/bucket indices are UTC.
- TZ is stored in NVS for display only. A wrong TZ (e.g. from a remote Eve connection) corrupts the Eve schedule display but cannot cause schedules to fire at the wrong time.
- The nightly recompute fires on a 24h elapsed timer (`_lastRecomputeTime24h`), not localtime midnight detection.
- `proper_timegm()` is the TZ-free `timegm()` equivalent. Use it whenever a UTC `struct tm` must be converted to `time_t`.
- `getEffectiveOffsetMin()` is the authoritative source for the UTC↔local offset. It prefers the Eve-confirmed `_lastKnownUtcOffsetMin` (set when Eve sends a `CURRENT_TIME` packet), falling back to the system TZ derived from `localtime_r`/`gmtime_r`. Use it anywhere the UTC offset is needed rather than reading `_lastKnownUtcOffsetMin` directly.
- `sanitizeScheduleToLocalLimit()` is called only from `convertEveSlotsToUTC()` (Eve→device path). Never call it on JSON-pushed schedules: the round-trip incorrectly drops native same-day UTC slots when cross-day slots from adjacent UTC days fill their slot positions first.
