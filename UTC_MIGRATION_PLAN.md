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

// TimeUtils.cpp
time_t proper_timegm(const struct tm *t) {
    int y = t->tm_year + 1900, m = t->tm_mon + 1;
    if (m <= 2) { y--; m += 12; }
    long days = (long)(365 * y) + (y/4) - (y/100) + (y/400)
              + (long)(30.6001 * (m + 1)) + t->tm_mday - 719591L;
    return (time_t)(days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec);
}
```

Use a **unit-tested** calendar→epoch implementation (integer mudane / Fliegel–Van Flandern style is preferable to `30.6001` floating‑point) and validate against known `struct tm` ↔ UTC instants on the ESP32 toolchain; `time_t` is signed 32‑bit on many Arduino builds, so document the supported year range.

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

**`idleStep()` (lines 436–448)** — Replace `localtime_r` midnight detection with 24h elapsed timer:
```cpp
if (_lastRecomputeTime24h == 0) _lastRecomputeTime24h = now;
if (now - _lastRecomputeTime24h >= 86400L) {
    _lastRecomputeTime24h = now;
    struct tm tm_buf;
    struct tm *t = gmtime_r(&now, &tm_buf);
    if (t->tm_wday == 0)          // UTC Sunday
        advanceMeasuredWeek();
    _taskState = DECAY_CHECK;
}
```
Remove `_lastRecomputeYday` usage.

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

## Recommended Implementation Order

1. Create `TimeUtils.h` / `TimeUtils.cpp` with `proper_timegm()`
2. `FakeGatoScheduler.cpp` — replace `timegm()` hack; add `_lastKnownUtcOffsetMin`; add `convertEveSlotsToUTC()`; wire into `parseProgramData()`; NVS `schedVersion` + migration reset on `SAVED_DATA` before `updateSchedulerWeekSchedule()` in `begin()`; Eve readback UTC→local
3. `SchedulerBase.cpp` — `gmtime()` + `proper_timegm()` in firing path; remove TZ guard; relax TZ-missing messaging in `begin()`
4. `NavienLearner.cpp` / `NavienLearner.h` — `gmtime()` for buckets; 24h elapsed timer
5. `BucketStore.cpp` / `BucketStore.h` — `gmtime()` for year; bump `BUCKET_SCHEMA_VERSION`
6. `NavienManager.ino` — epoch-based clock gate
7. Python: `navien_schedule_learner.py` — UTC bucket path; drop `local_tz`
8. Python: `navien_bootstrap.py` + `navien_bucket_export.py` — drop `local_tz`; send `schema_version: 2`

---

## Post-Flash Checklist

After flashing new firmware:
1. Verify device boots and connects to WiFi/NTP normally.
2. Check weblog for the migration reset message (logged from `FakeGatoScheduler` / `SAVED_DATA` path) — confirms slot reset.
3. Open Eve app → Programs tab → re-save the week schedule (triggers `parseProgramData()` → `convertEveSlotsToUTC()` → NVS write).
4. Run `python3 navien_bucket_export.py --push --replace` to re-seed UTC buckets from InfluxDB.
5. Run `python3 navien_bootstrap.py --push` if you want the computed schedule re-derived from InfluxDB in UTC.
6. Verify telnet `schedule` command shows correct local times (confirms UTC→local display conversion).
7. Verify Eve shows correct schedule times.

---

## CLAUDE.md Additions (after implementation)

Add to Architecture notes:

**UTC-native internal storage**
- `SchedulerBase::weekSchedule[]` stores UTC hour:minute. `getNextState()` and `initializeCurrentState()` use `gmtime()` and `proper_timegm()` (from `TimeUtils.h`).
- `NavienLearner` cold-start bucket assignment uses `gmtime()`. `buckets.bin` dow/bucket indices are UTC.
- TZ is stored in NVS for display only. A wrong TZ (e.g. from a remote Eve connection) corrupts the Eve schedule display but cannot cause schedules to fire at the wrong time.
- The nightly recompute fires on a 24h elapsed timer (`_lastRecomputeTime24h`), not localtime midnight detection.
- `proper_timegm()` is the TZ-free `timegm()` equivalent. Use it whenever a UTC `struct tm` must be converted to `time_t`.
