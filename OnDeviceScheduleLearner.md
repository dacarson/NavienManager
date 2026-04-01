# On-Device Schedule Learning — Implementation Plan

## Overview

A self-contained `NavienLearner` class that:
- **Detects cold-starts** in real time by observing RS-485 state transitions
- **Accumulates bucket data** into a compact flat file on LittleFS flash
- **Recomputes the schedule** nightly on Core 0, completely non-blocking to Core 1
- **Updates `FakeGatoScheduler`** with the new schedule exactly as the Pi does today via `setWeekScheduleFromJSON()`

After a one-time bootstrap (see Historical Bootstrap below), the Pi's weekly cron job can be disconnected entirely. The device learns autonomously from live data, recomputing its own schedule nightly.

---

## Data Model on LittleFS

Two files only. No directory tree. The LittleFS partition has 131,072 bytes total with ~77,824 bytes free; `buckets.bin` consumes ~12,104 bytes (~16% of free space), leaving ample room for FakeGato history and other data.

LittleFS is preferable to an SD card for this use case: it provides built-in wear leveling (important for a file updated multiple times daily), supports atomic rename, is already mounted, and is faster than SD for small file operations.

### `/navien/buckets.bin`

A fixed-size binary blob held entirely in RAM during recompute and written back as a complete file:

```cpp
struct BucketFile {
    uint32_t magic;          // 0x4E415649 ("NAVI") — detects corruption
    uint16_t schema_version; // bump if struct changes
    uint16_t current_year;   // year buckets[] was last written for

    // 7 days × 288 five-minute buckets (1440 min / 5)
    struct Bucket {
        uint16_t raw_count;      // unweighted cold-start hits
        float    weighted_score; // sum of recency-weighted scores
    } buckets[7][288];           // [dow][bucket_index], dow: 0=Sun
};
// Total: 8 + 7*288*6 = ~12,104 bytes — fits in one write
```

Raw counts and weighted scores are stored separately, matching what the Python script accumulates. The `current_year` field drives the annual decay.

Because LittleFS does not support in-place partial writes (no `fseek` equivalent), the workflow is always: **read full file into RAM → modify struct → write full file back**. At 12KB this takes ~10–15ms on LittleFS, which is acceptable for the Core 0 background task. The in-RAM `BucketFile` struct is the working copy at all times.

### `/navien/buckets.tmp`

Temp file used during atomic write (write → rename). LittleFS supports `LittleFS.rename()`, so the same write-to-tmp-then-rename strategy prevents corruption on power loss.

### LittleFS API Mapping

| POSIX (original plan) | LittleFS equivalent |
|---|---|
| `fopen()` | `LittleFS.open()` |
| `fread()` / `fwrite()` | `File.read()` / `File.write()` |
| `rename()` | `LittleFS.rename()` |
| `fseek()` + partial write | Not needed — always read/modify/write full file |

---

## Cold-Start Detection

Add a `NavienLearner::onNavienState()` method called from wherever RS-485 packets are currently dispatched (the same place `DEV_Navien` observes state). It needs:

```cpp
// Called every time a new RS-485 packet is processed (Core 1)
void NavienLearner::onNavienState(bool consumption_active,
                                  bool recirculation_running,
                                  time_t now);
```

### Internal State Machine

All variables live in RAM — no flash access during packet processing.

| Variable | Type | Purpose |
|---|---|---|
| `_lastActiveTime` | `time_t` | Timestamp of last `consumption_active==true` packet |
| `_inRun` | `bool` | Currently inside an active run |
| `_runStart` | `time_t` | When current run started (tap opened) |
| `_runDurationSec` | `uint32_t` | Elapsed seconds of current run (`now - _runStart`) |
| `_runDow` | `int` | Day-of-week at tap-open time (0=Sun); pinned at run start |
| `_runBucket` | `int` | 5-minute bucket index at tap-open time; pinned at run start |
| `_recircAtStart` | `bool` | Was recirc running at run start? |

`_runDurationSec` is compared against elapsed-second thresholds derived from the Python bucket counts at 10-second resolution:
- `min_duration_genuine` = 60 seconds (Python: 6 × 10s buckets)
- `min_duration_recirc`  = 30 seconds (Python: 3 × 10s buckets)

Using elapsed seconds rather than a packet counter makes the thresholds independent of RS-485 packet cadence, which may not be exactly 10 seconds at all times.

### State Transitions

```
consumption_active goes 0→1 AND (now - _lastActiveTime) >= cold_gap:
    → cold-start detected
    → _recircAtStart = recirculation_running
    → _runStart = now, _runDurationSec = 0, _inRun = true
    → _runDow    = localtime(&now)->tm_wday          // pinned to tap-open time
    → _runBucket = (now_hour * 60 + now_min) / 5     // pinned to tap-open time
    → (event not dispatched yet — duration unknown until run ends)

consumption_active == 1 AND _inRun:
    → _runDurationSec = now - _runStart
    → _lastActiveTime = now

consumption_active goes 1→0 AND _inRun:
    → _inRun = false
    → compute demand_weight from _recircAtStart and _runDurationSec
    → if demand_weight > 0.0: enqueue cold-start via xQueueOverwrite()
       using _runDow and _runBucket (tap-open time, not tap-close time)
```

`dow` and `bucket` are **always derived from `_runStart`** (when the tap opened), not from the time the run ends. This is correct because the schedule predicts when hot water demand begins — a run that opens at 23:58 and closes at 00:03 belongs to the 23:55 bucket of the previous day, not to Sunday midnight. Pinning to run-start at detection time means the values are already captured before any midnight crossing can occur.

### Cross-Core Communication

Cold-start events are the only data flowing from Core 1 to Core 0. They are transported via a **FreeRTOS single-element queue**, which provides full memory ordering guarantees through its internal critical sections — no hand-rolled atomics or `volatile` tricks needed.

```cpp
struct PendingColdStart {
    int      dow;            // day of week (0=Sun)
    int      bucket;         // 5-minute bucket index (0–287)
    float    demand_weight;  // 0.5 or 1.0
    float    recency_weight; // current year's weight multiplier
};

// Created in NavienLearner::begin(), capacity = 1
QueueHandle_t _coldStartQueue = xQueueCreate(1, sizeof(PendingColdStart));
```

**Core 1** (water packet callback):
```cpp
PendingColdStart cs = { dow, bucket, demand_weight, recency_weight };
xQueueOverwrite(_coldStartQueue, &cs);  // non-blocking; replaces any unread entry
```

**Core 0** (IDLE state, every 500ms):
```cpp
PendingColdStart cs;
if (xQueueReceive(_coldStartQueue, &cs, 0) == pdTRUE) {
    // safe to read cs — FreeRTOS guarantees full visibility
    _buckets.buckets[cs.dow][cs.bucket].raw_count++;
    _buckets.buckets[cs.dow][cs.bucket].weighted_score +=
        cs.demand_weight * cs.recency_weight;
    writeBucketsToLittleFS();
}
```

`xQueueOverwrite()` means if Core 0 is busy and misses a cold-start event, the next one replaces it rather than blocking Core 1. Cold-starts happen at most a few times per day; losing one in a theoretical burst is acceptable. **A burst of cold-starts closer than 500ms apart cannot occur** — they require at least `cold_gap` (10 minutes) of inactivity between them by definition.

RAM cost: ~100 bytes for a 1-element FreeRTOS queue handle and internal structure.

### Queue Failure Handling

**Creation failure** (`xQueueCreate` returns `nullptr`): checked in `NavienLearner::begin()`. If the queue cannot be allocated, `begin()` logs an error via `WEBLOG`, sets an internal `_learnerDisabled` flag, and returns false. All subsequent calls to `onNavienState()` check this flag and return immediately — the learner is silently inactive but does not crash. The rest of the firmware continues normally; the existing NVS/Eve schedule remains in effect.

```cpp
bool NavienLearner::begin() {
    _coldStartQueue = xQueueCreate(1, sizeof(PendingColdStart));
    if (_coldStartQueue == nullptr) {
        WEBLOG("NavienLearner: queue alloc failed — learner disabled");
        _learnerDisabled = true;
        return false;
    }
    // ... rest of init
    return true;
}
```

**Enqueue failure** (`xQueueOverwrite` return value): `xQueueOverwrite` is documented to always succeed for queues of length ≥ 1 (it overwrites the existing item if full rather than failing). The return value is `pdPASS` by definition. No additional error handling is required.

### Demand Weight Logic

Matches the `demand_weight` component of the Python script's `_extract_cold_starts()`. Two components present in the Python script are **intentionally omitted** as documented in the What This Does Not Attempt section — this is an algorithm fork, not an exact port:

- **Seasonal window** (±`window_weeks` band): omitted because incremental on-device accumulation replaces the need to limit query scope.
- **`cost_multiplier`** (dollar-value scaling): omitted because it is a constant multiplier that normalises to 1.0 for all genuine demand events and has no net effect on peak ranking.

The on-device `combined_weight` per cold-start is therefore `recency_weight × demand_weight` rather than the Python's `recency_weight × demand_weight × cost_multiplier`. For genuine demand events (the common case) the result is identical. For short cold-pipe taps (`demand_weight = 0.5`) the cost multiplier would have further halved the weight in Python — on-device these events are slightly over-weighted relative to Python, a conservative bias that may produce marginally wider windows. This is an acceptable and understood delta.

| Condition | `demand_weight` |
|---|---|
| `recirc_on`, duration < `min_duration_recirc` | 0.0 (discard) |
| `recirc_on`, duration >= `min_duration_recirc` | 1.0 |
| `recirc_off`, duration < `min_duration_genuine` | 0.5 |
| `recirc_off`, duration >= `min_duration_genuine` | 1.0 |

Thresholds (elapsed seconds, derived from Python's 10-second bucket counts):
- `min_duration_genuine` = 60 seconds (Python: 6 buckets × 10s)
- `min_duration_recirc`  = 30 seconds (Python: 3 buckets × 10s)
- `cold_gap`             = 600 seconds (10 minutes)

---

## Background Recompute — Core 0 Task

Launched once in `NavienLearner::begin()`:

```cpp
xTaskCreatePinnedToCore(
    NavienLearner::learnerTask,
    "NavienLearner",
    8192,      // stack — peak during recompute: buckets in RAM = ~12KB
    this,
    1,         // priority — below Arduino's priority on Core 1
    &_taskHandle,
    0          // Core 0
);
```

### Task State Machine

```
IDLE
  ├─ Every 500ms: xQueueReceive(_coldStartQueue, &cs, 0)
  │     If received → read buckets.bin into RAM,
  │                   update bucket struct (cs.dow, cs.bucket),
  │                   write full file back (~10–15ms)
  ├─ If midnight + 2min crossed → DECAY_CHECK
  └─ If recompute requested externally → RECOMPUTE_LOAD

DECAY_CHECK
  ├─ Read current_year from buckets.bin header
  ├─ If year changed → apply annual decay to all weighted_scores
  │     Write buckets.bin atomically (LittleFS.open() to .tmp → LittleFS.rename())
  └─ → RECOMPUTE_LOAD

RECOMPUTE_LOAD
  ├─ Read entire buckets.bin into RAM (~12KB, ~5ms)
  └─ → RECOMPUTE_DAY_0

RECOMPUTE_DAY_0 .. RECOMPUTE_DAY_6
  ├─ Run peak-finding for one day of the week
  ├─ vTaskDelay(10) between each day to yield to other Core 0 work
  └─ → RECOMPUTE_WRITE (after day 6)

RECOMPUTE_WRITE
  ├─ Build JSON string in setWeekScheduleFromJSON() format
  ├─ Acquire _scheduleHandoffMutex
  ├─ Copy JSON string into _pendingScheduleJSON
  ├─ Set _newScheduleReady = true
  ├─ Release _scheduleHandoffMutex
  ├─ Compute predicted efficiency → cache in _predictedEfficiency[7]
  ├─ Call broadcastUDP()
  └─ → IDLE
```

`FakeGatoScheduler::loop()` on Core 1 is the **sole** applier of the schedule. Core 0 never calls `setWeekScheduleFromJSON()` directly. The flag `_newScheduleReady` is only ever read or written while holding `_scheduleHandoffMutex` — there is no pre-lock check. The public method `checkNewSchedule(String &out_json)` encapsulates the non-blocking mutex take, JSON copy, and flag clear; it returns `true` only when a complete (non-truncated) schedule is ready.

```
FakeGatoScheduler::loop() (Core 1):
  ├─ [existing loop work]
  └─ xSemaphoreTake(_scheduleHandoffMutex, 0)  // non-blocking
       If acquired:
         If _newScheduleReady:
           Copy _pendingScheduleJSON into localCopy
           _newScheduleReady = false
         xSemaphoreGive(_scheduleHandoffMutex)
         If copied: Call setWeekScheduleFromJSON(localCopy)
```

The zero-timeout `xSemaphoreTake` means if Core 0 holds the mutex at that instant, `loop()` skips and retries on the next iteration. Missing one check is harmless — the schedule is applied on the next loop pass.

Recompute triggers at **midnight + 2 minutes** local time, detected by polling `localtime(now)` inside the task loop. This is the right moment: the previous day's cold-starts are complete, and the day-of-week index has just rolled over.

---

## Annual Decay at Year Rollover

On Jan 1 (detected by `current_year` mismatch in the file header):

```cpp
// Scale all weighted_scores by weight[1]/weight[0] = 2.0/3.0
// Matches Python recency_weights = [3, 2]:
//   current year data was written at ×3; aging it to "last year" = ×2
//   so multiply by 2/3 to correct the contribution
for (int dow = 0; dow < 7; dow++)
    for (int b = 0; b < 288; b++)
        buckets[dow][b].weighted_score *= (2.0f / 3.0f);

// raw_count is NOT decayed — it represents real occurrence count
// for the min_occurrences noise filter, regardless of year
```

If the device is powered off over New Year and powers on in January, the year mismatch is detected on first boot and decay is applied before any new data is written.

---

## Peak-Finding on Device (C++ Port)

The Python `_find_peaks()` and `buckets_to_windows()` functions port cleanly. All inputs are the 288-bucket arrays already in RAM.

### Algorithm

1. **Smooth**: sliding average ±2 buckets (5 additions per bucket × 288 buckets)
2. **Local maxima**: single pass, O(288 × separation/5)
3. **Greedy NMS**: sort up to ~10 candidates; accept in descending score order, suppressing neighbours within `min_peak_separation`

### Stack/Heap Usage

- 288 smoothed floats = 1,152 bytes — use a static buffer, not heap
- Peak candidate list: fixed array of 10 structs, ~80 bytes
- No dynamic allocation anywhere in the recompute path

### Parameters (matching Python defaults)

| Parameter | Value |
|---|---|
| `peak_half_width` | 30 minutes |
| `min_peak_separation` | 45 minutes |
| `preheat_minutes` | 3 minutes (= `COLD_PIPE_DRAIN_MINUTES` from `config.py`) |
| `min_weighted_score` | 6.0 |
| `min_score_floor` | 3.0 |
| `smooth_radius` | 2 buckets |
| `MAX_SLOTS_PER_DAY` | 3 |

### Adaptive Threshold

Matches the Python adaptive loop exactly:
1. Start at `min_weighted_score` = 6.0
2. Step down by 1.0 until `MAX_SLOTS_PER_DAY` peaks found or `min_score_floor` reached
3. If still fewer peaks, relax `min_occurrences` by 1 and retry

---

## Concurrency and Mutex

Two separate synchronisation mechanisms, each with a single clear purpose:

**1. FreeRTOS queue — cold-start handoff (Core 1 → Core 0)**

`_coldStartQueue` (capacity 1, see Cross-Core Communication above) carries `PendingColdStart` structs from the water packet callback on Core 1 to the IDLE state handler on Core 0. No mutex needed — the queue's internal critical sections provide all required memory ordering.

**2. Mutex — schedule JSON handoff (Core 0 → Core 1)**

```cpp
// Guards _pendingScheduleJSON and _newScheduleReady
SemaphoreHandle_t _scheduleHandoffMutex;
```

Core 0 (`RECOMPUTE_WRITE`) acquires this to write `_pendingScheduleJSON` and set `_newScheduleReady`. Core 1 (`FakeGatoScheduler::loop()`) acquires this to read and clear them. The mutex is held only for the string copy — never during the `setWeekScheduleFromJSON()` call itself, which may take tens of milliseconds (NVS write). This keeps the mutex window minimal.

`setWeekScheduleFromJSON()` is called **only on Core 1**, consistent with all other writes to `prog_send_data` and NVS. No mutex is needed around those writes.

All other `NavienLearner` operations (bucket reads/writes to LittleFS, peak-finding, efficiency computation) are owned entirely by Core 0 and require no synchronisation.

---

## Integration Points in Existing Code

| Location | Change |
|---|---|
| `NavienManager.ino` | Global `NavienLearner *learner`; instantiated and `begin()` called early in `setup()`, independent of HomeSpan/HomeKit |
| `FakeGatoScheduler.h` | Add `SemaphoreHandle_t _scheduleHandoffMutex` |
| `FakeGatoScheduler.cpp` — `parseProgramData()` | No mutex change needed — `setWeekScheduleFromJSON()` is already Core 1 only |
| `FakeGatoScheduler.cpp` — `begin()` | Create `_scheduleHandoffMutex` |
| `FakeGatoScheduler.cpp` — `loop()` | Non-blocking `xSemaphoreTake(_scheduleHandoffMutex, 0)`; if acquired and `_newScheduleReady`, copy JSON, clear flag, release mutex, call `setWeekScheduleFromJSON(localCopy)` — sole apply path; no pre-lock flag check |
| `NavienBroadcaster.ino` — `onWaterPacket()` | Calls `learner->onNavienState(water->consumption_active, water->recirculation_running, time(nullptr))` at the top of the callback |
| HTTP `POST /schedule` (port 8080, raw `WiFiServer`) | No change — receives finished schedule from `navien_bootstrap.py` (Step 1) |
| HTTP `POST /buckets` (port 8080, raw `WiFiServer`) | New path in existing `loopScheduleEndpoint()` dispatcher — calls `_learner->ingestBucketPayload(json)`; uses its own 6KB static buffer; triggers immediate recompute |
| Telnet `learnerStatus` command | New command in existing `setupTelnetCommands()` — prints bucket fill, last recompute, and per-day predicted/measured/gap table |
| UDP broadcast (port 2025, existing `AsyncUDP`) | Call `_learner->broadcastUDP()` after `RECOMPUTE_WRITE`; emits a `"type":"learner"` JSON packet consistent with existing packet types |
| Web status page (`navienStatus` callback) | Call `_learner->appendStatusHTML(page)` within existing `navienStatus` callback to append the Learner Status section |

---

## Efficiency Tracking

Two efficiency metrics are maintained continuously. Both are cheap enough to update in real time rather than on demand.

### Predicted Efficiency

Computed at the end of every `RECOMPUTE_WRITE` pass from `_buckets` and the newly produced schedule. For each day, walks the 288 buckets and checks each against up to 3 slot boundaries:

```
covered_schedulable = buckets that fall inside a slot
total_schedulable   = buckets that fall inside a slot OR within hot_window_min (15min) after slot end
predicted%          = covered_schedulable / total_schedulable × 100
```

**Cost:** 288 × 3 = 864 integer comparisons per day, 6,048 total across the week. A few milliseconds at most. Result cached as `float _predictedEfficiency[7]` (28 bytes).

### Measured Efficiency

Tracks what actually happened: of all cold-starts the device observed, what fraction had recirculation already running at the moment the tap opened (i.e. the schedule fired in time).

The cold-start detector in `onNavienState()` already captures `_recircAtStart` at tap-open time.  That value is forwarded to Core 0 via the `PendingColdStart` queue struct (field `recircAtStart`).  Core 0's `idleStep()` increments the counters when it consumes each event — keeping `_measured[]` and `_measuredHead` written by exactly one core with no synchronisation needed.

**Rolling window storage** — 4 weeks × 7 days × 2 counters:

```cpp
struct WeekMeasured {
    uint16_t total[7];    // cold-starts per day-of-week this week
    uint16_t covered[7];  // cold-starts with recirc already running
};
WeekMeasured _measured[4];  // 112 bytes total, lives in RAM
uint8_t      _measuredHead; // index of current week (0–3), rotates Sunday midnight
```

On Core 0, in `idleStep()`, after consuming each `PendingColdStart` from the queue:
```cpp
// Use cs.dow (tap-open day), not localtime(&now) (tap-close day).
// Consistent with cs.bucket — demand timestamp semantics are always anchored
// to when the cold-start began, not when the run ended.
_measured[_measuredHead].total[cs.dow]++;
if (cs.recircAtStart)
    _measured[_measuredHead].covered[cs.dow]++;
```

On Sunday midnight (same moment as nightly recompute):
```cpp
_measuredHead = (_measuredHead + 1) % 4;
memset(&_measured[_measuredHead], 0, sizeof(WeekMeasured)); // clear oldest week
```

**Computing the rolling figure** when `learnerStatus` is called — sum across all 4 weeks per day:
```cpp
for (int dow = 0; dow < 7; dow++) {
    uint32_t tot = 0, cov = 0;
    for (int w = 0; w < 4; w++) {
        tot += _measured[w].total[dow];
        cov += _measured[w].covered[dow];
    }
    measuredPct[dow] = (tot > 0) ? (cov * 100.0f / tot) : NAN;
}
```

**Cost:** 7 × 4 = 28 additions and 7 divides. Negligible.

### The Gap Metric

The difference between predicted and measured efficiency per day is the most actionable signal the system produces:

```
gap[dow] = predictedEfficiency[dow] - measuredEfficiency[dow]
```

| Gap | Interpretation |
|---|---|
| < 10% | Schedule and habits are well aligned |
| 10–25% | Normal drift — nightly recompute should self-correct within days |
| > 25% | Habits have shifted significantly; consider rerunning bootstrap |
| Predicted N/A | Insufficient bucket data for this day — schedule inherited from bootstrap |
| Measured NaN | No cold-starts observed yet for this day in the rolling window |

### `learnerStatus` Telnet Output

```
Learner Status
  Last recompute:  2025-03-30 00:02  (14h ago)
  Bucket fill:     142 / 2016 non-zero (7.0%)

  Day        Predicted   Measured    Gap    Cold-starts (4wk)
  ─────────────────────────────────────────────────────────
  Sunday       72.3%      68.1%     -4.2%   23
  Monday       88.5%      91.2%     +2.7%   31
  Tuesday      85.1%      79.4%     -5.7%   29
  Wednesday    83.6%      82.0%     -1.6%   28
  Thursday     86.2%      77.3%     -8.9%   30
  Friday       81.4%      74.8%     -6.6%   27
  Saturday     69.7%      61.2%     -8.5%   19
  ─────────────────────────────────────────────────────────
  Weekly avg   80.9%      76.3%     -4.6%
```

### Web Status Page

The same data is rendered as a section on the existing web status page. `NavienLearner` exposes a `appendStatusHTML(String &page)` method that the web handler calls when building the page — the same pattern used by other device components.

The gap column is colour-coded to make drift visible at a glance:

| Gap | Colour |
|---|---|
| < 10% | Green |
| 10–25% | Amber |
| > 25% | Red |

Example rendering:

```
Learner Status
Last recompute: 2025-03-30 00:02 (14h ago)   Bucket fill: 142 / 2016 (7.0%)

Day          Predicted   Measured    Gap        Cold-starts
Sunday         72.3%      68.1%     -4.2%  ●    23
Monday         88.5%      91.2%     +2.7%  ●    31
Tuesday        85.1%      79.4%     -5.7%  ●    29
Wednesday      83.6%      82.0%     -1.6%  ●    28
Thursday       86.2%      77.3%     -8.9%  ●    30
Friday         81.4%      74.8%     -6.6%  ●    27
Saturday       69.7%      61.2%     -8.5%  ●    19
Weekly avg     80.9%      76.3%     -4.6%  ●
```

No additional RAM is required — the HTML string is built on demand into the existing page buffer when the web request arrives, then discarded.

### Memory Impact

```
_predictedEfficiency[7]:    28 bytes   (floats, updated at recompute)
_measured[4] rolling window: 112 bytes  (uint16_t counters, updated per cold-start)
_measuredHead:               1 byte
─────────────────────────────────────────────────────────────────────
Total added RAM:             141 bytes
```

All in the `NavienLearner` object on the heap. No stack impact.

---

## UDP Broadcast

After every `RECOMPUTE_WRITE` the device broadcasts the new schedule and efficiency figures over UDP. This uses the **existing `AsyncUDP` broadcast on port 2025** — the same socket, same JSON format, and same throttle infrastructure already used for `water`, `gas`, `command`, and `announce` packets. No new socket, library, or port is needed.

### Trigger Points

The UDP broadcast fires exactly once per nightly recompute cycle, immediately after `RECOMPUTE_WRITE` completes. The `learnerStatus` Telnet command displays the same data locally but does not trigger a broadcast.

| Trigger | What is sent |
|---|---|
| `RECOMPUTE_WRITE` completes (nightly, midnight + 2min) | Full schedule + predicted + measured efficiency snapshot |

### JSON Packet Format

A new packet type `"learner"` consistent with the existing packet types. The Pi's InfluxDB logger already receives all packet types on port 2025 and handles the new type the same way.

```json
{
  "type": "learner",
  "last_recompute": 1743292920,
  "bucket_fill_pct": 7.0,
  "schedule": [
    {
      "dow": 0,
      "dow_name": "Sunday",
      "slots": [
        { "start_min": 360, "end_min": 480 },
        { "start_min": 1080, "end_min": 1170 }
      ],
      "predicted_pct": 72.3,
      "measured_pct": 68.1,
      "gap_pct": -4.2,
      "cold_starts_4wk": 23
    },
    {
      "dow": 1,
      "dow_name": "Monday",
      "slots": [
        { "start_min": 390, "end_min": 510 },
        { "start_min": 1110, "end_min": 1200 }
      ],
      "predicted_pct": 88.5,
      "measured_pct": 91.2,
      "gap_pct": 2.7,
      "cold_starts_4wk": 31
    }
  ],
  "debug": ""
}
```

Fields:

| Field | Type | Description |
|---|---|---|
| `last_recompute` | int | Unix timestamp of last recompute |
| `bucket_fill_pct` | float | Percentage of 2016 buckets that are non-zero |
| `schedule[].dow` | int | Day of week, 0=Sunday .. 6=Saturday |
| `schedule[].dow_name` | string | Human-readable day name |
| `schedule[].slots[].start_min` | int | Slot start in minutes since midnight |
| `schedule[].slots[].end_min` | int | Slot end in minutes since midnight |
| `schedule[].predicted_pct` | float | Predicted efficiency % from bucket analysis |
| `schedule[].measured_pct` | float | Measured efficiency % from rolling 4-week window; omitted if no data yet |
| `schedule[].gap_pct` | float | `predicted - measured`; omitted if measured not yet available |
| `schedule[].cold_starts_4wk` | int | Cold-starts observed in rolling 4-week window for this day |
| `debug` | string | Empty string (satisfies existing packet contract) |

All 7 days are always present in the `schedule` array. Days with no schedule slots have an empty `slots` array. `measured_pct` and `gap_pct` are omitted for days with no cold-starts in the rolling window rather than sent as zero, preventing false readings in Grafana before data accumulates. Slot times in minutes since midnight are unambiguous integers, directly queryable without string parsing.

### Total Payload Size

A complete `learner` packet with 7 days and up to 3 slots per day is approximately **900–1,100 bytes** — well within a single UDP datagram.

### Suggested InfluxDB Measurements (Pi Logger)

The Pi logger handles the new `learner` packet type and writes two measurements per packet:

| Measurement | Tags | Fields |
|---|---|---|
| `navien_schedule` | `dow` | `slot1_start`, `slot1_end`, `slot2_start`, `slot2_end`, `slot3_start`, `slot3_end` |
| `navien_efficiency` | `dow` | `predicted`, `measured`, `gap`, `cold_starts` |

### Memory Impact

No additional heap allocation. The JSON payload is built into a transient `String` on the Core 0 task stack during `broadcastUDP()` and released immediately after transmission.

---

## Bootstrapping and Fallback

- **First boot** (no `buckets.bin`): create empty file with zeroed buckets. Recompute finds no peaks → empty slots. Existing NVS schedule (Pi-pushed or Eve-set) remains active unchanged.
- **Bootstrap**: run the two-step bootstrap process (see Historical Bootstrap section) after first flash. This seeds both the active schedule and `buckets.bin` simultaneously, allowing the Pi cron job to be disconnected immediately after.
- **Stabilization without bootstrap**: if bootstrap is skipped, meaningful peaks emerge after ~2 weeks of live data; schedule stabilizes after ~4 weeks.
- **Debug**: Telnet `learnerStatus` reports predicted efficiency, measured efficiency, and the gap between them per day — see Efficiency Tracking section. A gap > 25% on any day signals habits have shifted and a bootstrap re-run may be warranted.

---

## What This Does Not Attempt

- **No seasonal window** (the ±4-week rolling band from the Python script). All same-day-of-week data accumulates with recency weighting doing the discrimination. The seasonal window was mainly useful to limit InfluxDB query size; incremental on-device updates don't need it.
- **No cost multipliers** (`COLD_START_WASTE_USD` / `RECIRC_WASTE_USD` scaling). These are constant multipliers that wash out in the normalized score. The `demand_weight` (0.5 / 1.0) is retained.
- **No ongoing Pi schedule publishing**. After the one-time bootstrap the Pi cron job is disconnected. The device maintains its own schedule autonomously from live cold-start data.

---

## Historical Bootstrap

Without seeding, the device starts cold: `buckets.bin` is empty and the nightly recompute produces no useful schedule for ~2–4 weeks. The bootstrap process eliminates this gap with two commands run once from the Pi, after which the Pi cron job can be disconnected permanently.

```
Step 1 — Push finished schedule:   navien_bootstrap.py --push
Step 2 — Seed bucket history:      navien_bucket_export.py --push
```

**Why both steps are needed:**

Step 1 gives the device a correct working schedule immediately. But `buckets.bin` is still empty, so the first nightly recompute at midnight would overwrite it with an empty schedule derived from zero data.

Step 2 seeds `buckets.bin` with the same historical data that produced the Step 1 schedule. The nightly recompute then runs against a full dataset from day one, producing a schedule consistent with Step 1 and improving incrementally from live data thereafter.

---

### Step 1 — `navien_bootstrap.py`

Queries full InfluxDB history, runs peak-finding on the Pi, and POSTs a finished schedule to the existing `/schedule` endpoint. No new ESP32 code needed.

The only difference from `navien_schedule_learner.py` is `window_weeks=52`, which queries the full year of data per recency entry rather than the default ±4-week seasonal band.

**Usage:**
```bash
# Dry run — review the historically-learned schedule before pushing
python3 navien_bootstrap.py

# Push the finished schedule to the ESP32
python3 navien_bootstrap.py --push

# Control years of history and their weights
python3 navien_bootstrap.py --recency_weights 3 2 1 --push
```

**Script:**
```python
# navien_bootstrap.py
#
# Step 1 of bootstrap: compute a schedule from full InfluxDB history and
# push it to the ESP32 via POST /schedule.
#
# Uses navien_schedule_learner with window_weeks=52 (full year per recency
# entry) instead of the default rolling ±4-week seasonal window.
#
# Run once after first flash or whenever buckets.bin is wiped.
# Always run navien_bucket_export.py immediately after.

import sys
import navien_schedule_learner as nsl
from datetime import date as _date

def main():
    import argparse, json
    import config

    parser = argparse.ArgumentParser(
        description="Bootstrap ESP32 schedule from full InfluxDB history",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--influxdb_host",        default=config.INFLUX_HOST)
    parser.add_argument("--influxdb_port",        default=config.INFLUX_PORT, type=int)
    parser.add_argument("--influxdb_user",        default=None)
    parser.add_argument("--influxdb_pass",        default=None)
    parser.add_argument("--influxdb_db",          default=config.INFLUX_DB)
    parser.add_argument("--recency_weights",      default=nsl.DEFAULT_RECENCY_WEIGHTS,
                        type=int, nargs="+", metavar="W")
    parser.add_argument("--esp32_host",           default="10.0.1.149")
    parser.add_argument("--esp32_port",           default=8080, type=int)
    parser.add_argument("--cold_gap_minutes",     default=nsl.DEFAULT_COLD_GAP, type=int)
    parser.add_argument("--min_duration_genuine", default=nsl.DEFAULT_MIN_DURATION_GENUINE, type=int)
    parser.add_argument("--min_duration_recirc",  default=nsl.DEFAULT_MIN_DURATION_RECIRC, type=int)
    parser.add_argument("--preheat_minutes",      default=nsl.DEFAULT_PREHEAT_MINUTES, type=int)
    parser.add_argument("--gap_minutes",          default=nsl.DEFAULT_GAP_MINUTES, type=int)
    parser.add_argument("--min_occurrences",      default=nsl.DEFAULT_MIN_OCCURRENCES, type=int)
    parser.add_argument("--min_weighted_score",   default=nsl.DEFAULT_MIN_WEIGHTED_SCORE, type=float)
    parser.add_argument("--min_score_floor",      default=nsl.DEFAULT_MIN_SCORE_FLOOR, type=float)
    parser.add_argument("--peak_half_width",      default=30, type=int)
    parser.add_argument("--min_peak_separation",  default=45, type=int)
    parser.add_argument("--push",    action="store_true")
    parser.add_argument("--dry_run", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    # Full-year window so all available history is included
    args.window_weeks = 52

    years = [_date.today().year - i for i in range(len(args.recency_weights))]
    print(f"Bootstrap mode: window_weeks=52 (full year per recency entry)")
    print(f"Years: {years}  Weights: {args.recency_weights}")

    local_tz, tz_name = nsl.detect_local_timezone()
    print(f"Timezone: {tz_name}")

    events = nsl.fetch_consumption_events(args, local_tz)
    if not events:
        print("No events found. Check InfluxDB connection.")
        sys.exit(1)
    print(f"Found {len(events)} cold-start events across full history.")

    raw_counts, weighted_scores = nsl.events_to_minutes(events, verbose=args.verbose)
    week = nsl.buckets_to_windows(
        raw_counts, weighted_scores,
        gap_minutes=args.gap_minutes,
        min_occurrences=args.min_occurrences,
        preheat_minutes=args.preheat_minutes,
        peak_half_width=args.peak_half_width,
        min_peak_separation=args.min_peak_separation,
        min_weighted_score=args.min_weighted_score,
        min_score_floor=args.min_score_floor,
        verbose=args.verbose,
    )
    nsl.print_schedule(week, raw_counts=raw_counts, weighted_scores=weighted_scores,
                       verbose=args.verbose, args=args)

    if args.push and not args.dry_run:
        nsl.push_schedule(week, args)
        print("Step 1 complete. Now run: python3 navien_bucket_export.py --push")
    else:
        print("[dry-run] Not pushing. Pass --push to send to ESP32.")
        print(json.dumps({"schedule": week}, indent=2))

if __name__ == "__main__":
    main()
```

---

### Step 2 — `navien_bucket_export.py` and `POST /buckets`

Seeds `buckets.bin` on the device with the raw bucket data (cold-start counts and weighted scores) extracted from InfluxDB history. This gives the nightly on-device recompute a full dataset to work from immediately rather than starting from zero.

This is a **one-time bootstrap tool**, not an ongoing interface. After bootstrap the Pi plays no further role in schedule management.

#### Pi — `navien_bucket_export.py`

Reuses `fetch_consumption_events()` and `events_to_minutes()` from `navien_schedule_learner.py` to build the bucket data, then serialises it as a sparse JSON payload and POSTs it to `POST /buckets`.

**Usage:**
```bash
# Dry run — print the JSON payload that would be sent
python3 navien_bucket_export.py

# Seed buckets.bin from full history
python3 navien_bucket_export.py --push

# Reseed cleanly from scratch (zeroes existing buckets first)
python3 navien_bucket_export.py --push --replace
```

**Script:**
```python
# navien_bucket_export.py
#
# Step 2 of bootstrap: extract raw bucket data from InfluxDB history and
# POST it to the ESP32 POST /buckets endpoint to seed buckets.bin.
#
# Always run navien_bootstrap.py (Step 1) first.
# After this step the Pi cron job can be disconnected.

import argparse, json, sys
import navien_schedule_learner as nsl
import config
from datetime import date as _date

def build_bucket_payload(args, local_tz, replace=False):
    events = nsl.fetch_consumption_events(args, local_tz)
    if not events:
        print("No events found. Check InfluxDB connection.")
        sys.exit(1)
    raw_counts, weighted_scores = nsl.events_to_minutes(events)

    days = []
    for dow in range(7):
        day_raw      = raw_counts.get(dow, {})
        day_weighted = weighted_scores.get(dow, {})
        buckets = []
        for b in sorted(day_raw):
            if day_raw[b] > 0 or day_weighted.get(b, 0.0) > 0:
                buckets.append({
                    "b":     b // 5,   # minute offset → 5-min bucket index
                    "raw":   day_raw[b],
                    "score": round(day_weighted.get(b, 0.0), 4),
                })
        if buckets:
            days.append({"dow": dow, "buckets": buckets})

    return {
        "schema_version": 1,
        "current_year":   _date.today().year,
        "replace":        replace,
        "days":           days,
    }

def push_buckets(payload, args):
    import requests
    url  = f"http://{args.esp32_host}:{args.esp32_port}/buckets"
    body = json.dumps(payload)
    print(f"[push] POST {url}  ({len(body)} bytes)")
    resp = requests.post(url, data=body,
                         headers={"Content-Type": "application/json"},
                         timeout=30)
    if resp.status_code == 200:
        print(f"[push] Accepted: {resp.json()}")
        print("Bootstrap complete. Pi cron job can now be disabled.")
    else:
        print(f"[push] Error {resp.status_code}: {resp.text}")
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(
        description="Seed ESP32 buckets.bin from InfluxDB history (bootstrap step 2)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--influxdb_host",        default=config.INFLUX_HOST)
    parser.add_argument("--influxdb_port",        default=config.INFLUX_PORT, type=int)
    parser.add_argument("--influxdb_user",        default=None)
    parser.add_argument("--influxdb_pass",        default=None)
    parser.add_argument("--influxdb_db",          default=config.INFLUX_DB)
    parser.add_argument("--recency_weights",      default=nsl.DEFAULT_RECENCY_WEIGHTS,
                        type=int, nargs="+", metavar="W")
    parser.add_argument("--esp32_host",           default="10.0.1.149")
    parser.add_argument("--esp32_port",           default=8080, type=int)
    parser.add_argument("--cold_gap_minutes",     default=nsl.DEFAULT_COLD_GAP, type=int)
    parser.add_argument("--min_duration_genuine", default=nsl.DEFAULT_MIN_DURATION_GENUINE, type=int)
    parser.add_argument("--min_duration_recirc",  default=nsl.DEFAULT_MIN_DURATION_RECIRC, type=int)
    parser.add_argument("--replace", action="store_true",
                        help="Zero all buckets on device before seeding")
    parser.add_argument("--push",    action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    # Full-year window, matching navien_bootstrap.py
    args.window_weeks    = 52
    args.preheat_minutes = nsl.DEFAULT_PREHEAT_MINUTES
    args.gap_minutes     = nsl.DEFAULT_GAP_MINUTES

    local_tz, tz_name = nsl.detect_local_timezone()
    print(f"Timezone: {tz_name}")

    payload = build_bucket_payload(args, local_tz, replace=args.replace)
    total   = sum(len(d["buckets"]) for d in payload["days"])
    print(f"Built payload: {len(payload['days'])} days, "
          f"{total} non-zero buckets, "
          f"~{len(json.dumps(payload)) // 1024} KB")

    if args.push:
        push_buckets(payload, args)
    else:
        print("[dry-run] Not pushing. Pass --push to send to ESP32.")
        print(json.dumps(payload, indent=2))

if __name__ == "__main__":
    main()
```

#### ESP32 — `POST /buckets` Endpoint

Accepts the sparse JSON payload, merges or replaces `_buckets`, writes atomically to LittleFS, and triggers an immediate recompute so the updated buckets take effect before the next midnight.

**Request:**
```
POST /buckets
Content-Type: application/json

{
  "schema_version": 1,
  "current_year": 2025,
  "replace": false,
  "days": [
    {
      "dow": 0,
      "buckets": [
        { "b": 72, "raw": 5, "score": 12.0 },
        { "b": 73, "raw": 3, "score": 7.5 }
      ]
    }
  ]
}
```

| Field | Type | Description |
|---|---|---|
| `schema_version` | int | Must match ESP32's `BUCKET_SCHEMA_VERSION` — rejected if mismatched |
| `current_year` | int | Written into the `BucketFile` header |
| `replace` | bool | `false` = add to existing (default); `true` = zero all buckets first |
| `days[].dow` | int | Day of week, 0=Sunday .. 6=Saturday |
| `days[].buckets[].b` | int | 5-minute bucket index (0–287) |
| `days[].buckets[].raw` | int | Unweighted cold-start count to add |
| `days[].buckets[].score` | float | Weighted score to add |

**Response:**
```json
{ "status": "ok", "buckets_written": 142, "replaced": false }
```

**Merge logic** (`replace=false`):
```cpp
_buckets.buckets[dow][b].raw_count      += incoming.raw;
_buckets.buckets[dow][b].weighted_score += incoming.score;
```

**Replace logic** (`replace=true`): zero the entire `_buckets` struct before applying incoming data. Safe to re-run if the first upload was incorrect.

After writing, sets `_recomputeRequested = true` so the Core 0 task runs peak-finding immediately rather than waiting for midnight.

---

### When to Run Bootstrap

| Situation | Commands |
|---|---|
| First flash | `navien_bootstrap.py --push` then `navien_bucket_export.py --push` |
| LittleFS wiped / device replaced | Same as first flash |
| Corrupt `buckets.bin` suspected | `navien_bucket_export.py --push --replace` |
| Re-tune parameters | `navien_bootstrap.py --peak_half_width 25 --push` then `navien_bucket_export.py --push --replace` |

After bootstrap the Pi cron job is disabled. The device owns the schedule from this point forward.

---

## Memory Budget

Validated against runtime measurements: **118,532 bytes free heap**, **69,620 bytes max contiguous allocatable block**.

### Heap Allocations

| Allocation | Size | Lifetime |
|---|---|---|
| `BucketFile _buckets` (class member) | 12,104 bytes | Permanent |
| `NavienLearner` object itself | ~100 bytes | Permanent |
| `_predictedEfficiency[7]` + measured rolling window | 141 bytes | Permanent |
| Core 0 task stack (FreeRTOS heap-backed) | 8,192 bytes | Permanent |
| JSON string in `RECOMPUTE_WRITE` | ~1,250 bytes | Transient |
| ArduinoJson doc during `POST /buckets` ingest | ~2,000 bytes | Transient (bootstrap only) |
| **Peak simultaneous heap cost** | **~23,787 bytes** | **20% of free heap** |

`BucketFile` at 12,104 bytes is well within the 69,620-byte max contiguous block (17% of that limit). The `POST /buckets` ArduinoJson allocation is transient and only occurs once during bootstrap.

### Two Non-Negotiable Implementation Rules

**Rule 1 — `BucketFile` must be a class member, never a local variable.**
If `BucketFile _buckets` is declared on the stack anywhere in the call chain, the task stack overflows silently. It must be allocated once as a `NavienLearner` member so it lives on the heap for the object's lifetime.

```cpp
// CORRECT — heap, allocated at construction
class NavienLearner {
    BucketFile _buckets;   // 12,104 bytes on heap as part of object
    ...
};

// WRONG — stack overflow
void NavienLearner::recompute() {
    BucketFile buckets;    // 12,104 bytes on stack — DO NOT DO THIS
}
```

**Rule 2 — `smoothed[288]` must be `static` inside the peak-finding function.**
288 floats = 1,152 bytes. Declaring it `static` places it in BSS (zero-initialized at boot) rather than on the task stack, eliminating the largest remaining stack pressure in the recompute path.

```cpp
// CORRECT — BSS, no stack cost
void NavienLearner::findPeaks(...) {
    static float smoothed[288];
    ...
}

// ACCEPTABLE BUT UNNECESSARY RISK — 1,152 bytes on stack
void NavienLearner::findPeaks(...) {
    float smoothed[288];
    ...
}
```

### Revised Stack Requirement

With both rules applied, the worst-case stack frame during `RECOMPUTE_DAY_x` is:

```
smoothed[288] floats (static — not on stack):      0 bytes
Peak candidates array (10 × 8 bytes):             80 bytes
Loop locals and return addresses:                ~200 bytes
FreeRTOS task overhead:                          ~500 bytes
─────────────────────────────────────────────────────────
Worst-case stack usage:                        ~780 bytes
```

The 8,192-byte task stack allocation is retained as specified — the headroom is intentional and absorbs any additional call depth from LittleFS internals during file writes within the task.

---

## BEHAVIOR_SPEC Compliance Notes

Constraints derived from `BEHAVIOR_SPEC.md` that govern how this feature is implemented:

**No new libraries.** `NavienLearner` uses only libraries already linked: ArduinoJson (bucket payload parsing), `AsyncUDP` (broadcast), `<WiFi.h>` `WiFiServer` (HTTP endpoints), ESPTelnet (Telnet command), and LittleFS (file I/O). No new dependencies are introduced.

**`POST /buckets` shares port 8080 via existing `loopScheduleEndpoint()`.** The raw `WiFiServer` on port 8080 already dispatches `POST /schedule`. Path-based dispatch is added to the same handler for `POST /buckets`. The existing 2KB static buffer is sufficient for `/schedule`; `/buckets` uses a separate 6KB static buffer allocated only when that path is matched, avoiding any impact on the existing endpoint.

**UDP broadcast uses existing `AsyncUDP` on port 2025.** The `"learner"` packet type follows the same JSON structure as `"water"`, `"gas"`, `"command"`, and `"announce"` packets. The existing throttle infrastructure (`resetPreviousValues()` every 5 seconds) applies unchanged.

**Web status page uses existing `navienStatus` callback.** `NavienLearner::appendStatusHTML()` is called from within the existing `navienStatus` callback, consistent with how other device components contribute to the status page.

**Telnet command registered via existing `setupTelnetCommands()`.** `learnerStatus` is added to the existing `std::map`-based command registry alongside `memory`, `fsStat`, and other diagnostic commands.

**`onNavienState()` is wired via RS485 packet callback, not direct call.** The cold-start detector hooks into the water packet callback registered in `setupNavienBroadcaster()`, consistent with how all other packet consumers observe RS485 state.

**Monitor mode awareness.** `onNavienState()` fires and cold-starts are recorded regardless of mode — the learner accumulates data whether or not the device is in control. The nightly recompute result is **always applied** via `setWeekScheduleFromJSON()` unconditionally: it persists to NVS, updates `prog_send_data`, and triggers Eve EV sync. Heater control commands (recirculation on/off, power) are already gated behind `controlAvailable()` inside `stateChange()` — no additional guard is needed in the learner. The schedule is authoritative program state independent of whether the device is currently controlling the heater.

---

## Build Order

| Phase | Deliverable | Goal |
|---|---|---|
| 1 | `BucketStore` class | LittleFS read/write/atomic-update of `buckets.bin`. Test standalone. |
| 2 | Cold-start detector | `onNavienState()` state machine. Unit-test with synthetic packet sequences. |
| 3 | Core 0 task skeleton | IDLE → consume pending cold-start → write bucket file. Confirm zero RS-485 impact. |
| 4 | Peak-finding C++ port | Port `_find_peaks()` and `buckets_to_windows()`. Validate against known Python output. |
| 5 | Full recompute integration | Wire RECOMPUTE states; Core 0 writes `_pendingScheduleJSON` + sets `_newScheduleReady`; Core 1 `loop()` is sole applier via `setWeekScheduleFromJSON()`. |
| 6 | Annual decay | Year-check on startup and midnight transition. |
| 7 | Efficiency tracking | Predicted efficiency in `RECOMPUTE_WRITE`; measured rolling window updated on Core 0 in `idleStep()` (via `recircAtStart` field in `PendingColdStart`); `learnerStatus` Telnet command. |
| 8 | UDP broadcast | `broadcastUDP()` called after `RECOMPUTE_WRITE` only (nightly); emits `"type":"learner"` JSON packet; verify receipt in InfluxDB. |
| 9 | `POST /buckets` endpoint | ESP32 ingest handler: parse sparse JSON, merge/replace `_buckets`, write LittleFS, trigger recompute. |
| 10 | `navien_bootstrap.py` | Pi Step 1: full-history peak-finding → push finished schedule via `POST /schedule`. |
| 11 | `navien_bucket_export.py` | Pi Step 2: full-history bucket extraction → push raw bucket data via `POST /buckets`. Then disable Pi cron. |

**Phase 1 Note:** Phase 1 introduced `BucketStore` as a standalone class (`BucketStore.h` / `BucketStore.cpp`) that encapsulates all LittleFS file I/O. `NavienLearner` owns a `BucketStore _store` member rather than managing LittleFS directly.