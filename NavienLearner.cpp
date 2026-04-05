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

#include "NavienLearner.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <AsyncUDP.h>
#include <ArduinoJson.h>

extern AsyncUDP udp;  // defined in NavienBroadcaster.ino
static constexpr int UDP_BROADCAST_PORT = 2025;

// ---------------------------------------------------------------------------
// safeAppend() — file-private snprintf helper with truncation protection.
//
// Appends a formatted string into buf[bufSize] starting at *pos.  On each
// call *pos is advanced by the number of characters actually written.  If
// vsnprintf reports the output would exceed the remaining space (which can
// happen on newlib even when the buffer is full), *pos is clamped to
// bufSize-1 so the buffer always stays null-terminated and subsequent calls
// become no-ops.  Returns false if truncation occurred.
// ---------------------------------------------------------------------------
static bool safeAppend(char *buf, int bufSize, int *pos, const char *fmt, ...) {
    int remaining = bufSize - *pos;
    if (remaining <= 1) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, (size_t)remaining, fmt, ap);
    va_end(ap);
    if (n < 0) return false;            // encoding error
    if (n >= remaining) {               // truncated: cap and mark full
        *pos      = bufSize - 1;
        buf[*pos] = '\0';
        return false;
    }
    *pos += n;
    return true;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

NavienLearner::NavienLearner()
    : _lastActiveTime(0),
      _inRun(false),
      _runStart(0),
      _runDurationSec(0),
      _runDow(0),
      _runBucket(0),
      _recircAtStart(false),
      _lastRecircActiveTime(0),
      _coldStartQueue(nullptr),
      _taskHandle(nullptr),
      _recomputeRequested(false),
      _taskState(IDLE),
      _recomputeDay(0),
      _lastRecomputeYday(-1),
      _startupDecayDone(false),
      _lastRecomputeTime(0),
      _scheduleHandoffMutex(nullptr),
      _newScheduleReady(false),
      _measuredHead(0),
      _learnerDisabled(false)
{
    memset(_measured,            0, sizeof(_measured));
    memset(_weekSlots,           0, sizeof(_weekSlots));
    memset(_weekSlotCount,       0, sizeof(_weekSlotCount));
    memset(_pendingScheduleJSON, 0, sizeof(_pendingScheduleJSON));
    // NAN cannot be set via memset (its bit pattern is not 0); loop instead.
    // This ensures N/A is displayed before the first recompute completes.
    for (int i = 0; i < BUCKET_DAYS; i++) {
        _predictedEfficiency[i] = NAN;
    }
}

// ---------------------------------------------------------------------------
// begin()
// ---------------------------------------------------------------------------

bool NavienLearner::begin() {
    _coldStartQueue = xQueueCreate(1, sizeof(PendingColdStart));
    if (_coldStartQueue == nullptr) {
        Serial.println("NavienLearner: queue alloc failed — learner disabled");
        _learnerDisabled = true;
        return false;
    }

    _scheduleHandoffMutex = xSemaphoreCreateMutex();
    if (_scheduleHandoffMutex == nullptr) {
        Serial.println("NavienLearner: mutex alloc failed — learner disabled");
        _learnerDisabled = true;
        // Queue is left live but unreachable; same intentional tradeoff as the
        // BucketStore and task-create failure paths below.
        return false;
    }

    if (!_store.begin()) {
        Serial.println("NavienLearner: BucketStore init failed — learner disabled");
        _learnerDisabled = true;
        // Queue and mutex were created above but are left live; same intentional
        // tradeoff as the task-create failure path below.
        return false;
    }

    // Restore measured efficiency window if a prior save exists; silently
    // starts from zero if the file is absent (first boot) or corrupt.
    loadMeasured();

    BaseType_t taskRet = xTaskCreatePinnedToCore(
        NavienLearner::learnerTask,
        "NavienLearner",
        8192,   // stack: headroom for LittleFS internals during file writes
        this,
        1,      // priority 1 on Core 0 — yields readily to higher-priority tasks
        &_taskHandle,
        0       // Core 0
    );
    if (taskRet != pdPASS) {
        Serial.println("NavienLearner: task create failed — learner disabled");
        _learnerDisabled = true;
        // Note: the queue and BucketStore are already initialised at this point.
        // They are left live but unreachable — onNavienState() bails on
        // _learnerDisabled, so no further writes occur. Not a realistic field
        // concern since task create failure requires severe heap exhaustion.
        return false;
    }

    // If buckets were loaded from flash, schedule an immediate recompute so
    // that _predictedEfficiency is populated on startup without waiting for
    // midnight or a POST /buckets upload.
    if (_store.nonZeroCount() > 0)
        _recomputeRequested = true;

    Serial.println("NavienLearner: ready");
    return true;
}

// ---------------------------------------------------------------------------
// onNavienState() — cold-start detector (Core 1)
// ---------------------------------------------------------------------------

void NavienLearner::onNavienState(bool consumption_active,
                                   bool recirculation_active,
                                   time_t now) {
    if (_learnerDisabled) {
        return;
    }

    // --- consumption_active 0→1: potential cold-start ---
    if (consumption_active && !_inRun) {
        bool isColdStart = (_lastActiveTime == 0) ||
                           ((now - _lastActiveTime) >= (time_t)COLD_GAP_SEC);

        if (isColdStart) {
            // Pin dow and bucket to tap-open time.
            struct tm *t = localtime(&now);
            _runDow    = t->tm_wday;                          // 0=Sun
            _runBucket = (t->tm_hour * 60 + t->tm_min) / 5;  // 0–287

            _runStart        = now;
            _runDurationSec  = 0;
            // Pipes are considered hot if recirculation_active is true now
            // OR was true within the last RECIRC_HOT_WINDOW_SEC (15 min) —
            // matching navien_efficiency.py's RECIRC_WINDOW_MINUTES lookback.
            // This covers taps opened shortly after a recirc slot ends.
            bool recircRecent = recirculation_active ||
                                (_lastRecircActiveTime > 0 &&
                                 (now - _lastRecircActiveTime) < (time_t)RECIRC_HOT_WINDOW_SEC);
            _recircAtStart   = recircRecent;
            _inRun           = true;
            // Event not dispatched yet — duration unknown until run ends.
        } else {
            // Warm restart within an existing run — just track it as active.
            _inRun = true;
        }
    }

    // --- consumption_active == 1 AND in run: accumulate duration ---
    if (consumption_active && _inRun) {
        _runDurationSec = (uint32_t)(now - _runStart);
        _lastActiveTime = now;
    }

    // --- consumption_active 1→0: run ended ---
    if (!consumption_active && _inRun) {
        _inRun = false;

        float demand_weight = computeDemandWeight(_recircAtStart, _runDurationSec);

        if (demand_weight > 0.0f) {
            // Enqueue for Core 0: bucket accumulation and measured-efficiency
            // counter update.  Both operations now happen on Core 0 so that
            // _measured[] and _measuredHead are only ever written by one core.
            PendingColdStart cs;
            cs.dow            = _runDow;
            cs.bucket         = _runBucket;
            cs.demand_weight  = demand_weight;
            cs.recency_weight = RECENCY_WEIGHT_CURRENT;
            cs.recircAtStart  = _recircAtStart;
            xQueueOverwrite(_coldStartQueue, &cs);
        }
    }

    if (recirculation_active) {
        _lastRecircActiveTime = now;
    }
}

// ---------------------------------------------------------------------------
// advanceMeasuredWeek() — called at Sunday midnight (Core 0 task)
// ---------------------------------------------------------------------------

void NavienLearner::advanceMeasuredWeek() {
    _measuredHead = (_measuredHead + 1) % 4;
    memset(&_measured[_measuredHead], 0, sizeof(WeekMeasured));
    saveMeasured();
}

// ---------------------------------------------------------------------------
// saveMeasured() / loadMeasured() — LittleFS persistence for _measured[]
// ---------------------------------------------------------------------------

// On-disk layout for measured.bin
struct MeasuredFile {
    uint32_t    magic;           // 0x4D454153 ("MEAS")
    uint8_t     schema_version;  // bump if struct layout changes
    uint8_t     head;            // _measuredHead (0–3)
    uint8_t     pad[2];
    WeekMeasured measured[4];
};

static constexpr uint32_t MEASURED_MAGIC          = 0x4D454153u;
static constexpr uint8_t  MEASURED_SCHEMA_VERSION = 1;
static constexpr char     MEASURED_FILE[]         = "/navien/measured.bin";
static constexpr char     MEASURED_TMP_FILE[]     = "/navien/measured.tmp";

bool NavienLearner::saveMeasured() {
    MeasuredFile mf;
    mf.magic          = MEASURED_MAGIC;
    mf.schema_version = MEASURED_SCHEMA_VERSION;
    mf.head           = _measuredHead;
    mf.pad[0]         = 0;
    mf.pad[1]         = 0;
    memcpy(mf.measured, _measured, sizeof(_measured));

    File f = LittleFS.open(MEASURED_TMP_FILE, "w");
    if (!f) {
        Serial.println("NavienLearner: failed to open measured.tmp for writing");
        return false;
    }
    size_t written = f.write(reinterpret_cast<const uint8_t *>(&mf), sizeof(mf));
    f.close();

    if (written != sizeof(mf)) {
        Serial.printf("NavienLearner: short write to measured.tmp (%u/%u)\n",
                      (unsigned)written, (unsigned)sizeof(mf));
        LittleFS.remove(MEASURED_TMP_FILE);
        return false;
    }

    if (!LittleFS.rename(MEASURED_TMP_FILE, MEASURED_FILE)) {
        Serial.println("NavienLearner: rename measured.tmp -> measured.bin failed");
        LittleFS.remove(MEASURED_TMP_FILE);
        return false;
    }

    Serial.println("NavienLearner: measured window saved");
    return true;
}

bool NavienLearner::loadMeasured() {
    if (!LittleFS.exists(MEASURED_FILE)) {
        return false;   // first boot — silently start from zero
    }

    File f = LittleFS.open(MEASURED_FILE, "r");
    if (!f) {
        Serial.println("NavienLearner: failed to open measured.bin for reading");
        return false;
    }

    MeasuredFile mf;
    size_t got = f.read(reinterpret_cast<uint8_t *>(&mf), sizeof(mf));
    f.close();

    if (got != sizeof(mf)) {
        Serial.printf("NavienLearner: measured.bin size mismatch (%u/%u)\n",
                      (unsigned)got, (unsigned)sizeof(mf));
        return false;
    }
    if (mf.magic != MEASURED_MAGIC) {
        Serial.printf("NavienLearner: measured.bin bad magic 0x%08X\n", mf.magic);
        return false;
    }
    if (mf.schema_version != MEASURED_SCHEMA_VERSION) {
        Serial.printf("NavienLearner: measured.bin schema mismatch (%u)\n",
                      mf.schema_version);
        return false;
    }
    if (mf.head >= 4) {
        Serial.printf("NavienLearner: measured.bin bad head %u\n", mf.head);
        return false;
    }

    memcpy(_measured, mf.measured, sizeof(_measured));
    _measuredHead = mf.head;
    Serial.printf("NavienLearner: measured window loaded (head=%u)\n", mf.head);
    return true;
}

// ---------------------------------------------------------------------------
// learnerTask() — Core 0 background task state machine
// ---------------------------------------------------------------------------

void NavienLearner::learnerTask(void *pvParam) {
    NavienLearner *self = static_cast<NavienLearner *>(pvParam);

    for (;;) {
        switch (self->_taskState) {

            case IDLE:
                self->idleStep();
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case DECAY_CHECK:
                // Check for year rollover; apply weighted_score decay if needed.
                // Always proceeds to RECOMPUTE_LOAD regardless of whether decay ran.
                self->decayCheck();
                self->_taskState = RECOMPUTE_LOAD;
                break;

            case RECOMPUTE_LOAD:
                // Data is already in RAM (BucketStore holds it).
                // Reset per-day counters and begin processing from day 0.
                memset(self->_weekSlotCount, 0, sizeof(self->_weekSlotCount));
                self->_recomputeDay = 0;
                self->_taskState    = RECOMPUTING;
                // No delay — start RECOMPUTING immediately on next iteration.
                break;

            case RECOMPUTING: {
                int day = self->_recomputeDay;
                self->_weekSlotCount[day] = PeakFinder::findDaySlots(
                    self->_store.data().buckets[day],
                    self->_weekSlots[day]);
                self->_recomputeDay++;
                if (self->_recomputeDay >= BUCKET_DAYS) {
                    self->_taskState = RECOMPUTE_WRITE;
                }
                // Yield between days so higher-priority Core 0 work is not starved.
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }

            case RECOMPUTE_WRITE:
                self->recomputeWrite();
                self->_taskState = IDLE;
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// idleStep() — private; called once per IDLE tick (Core 0)
// ---------------------------------------------------------------------------

void NavienLearner::idleStep() {
    // One-shot startup decay: on the first tick where the clock is valid,
    // call decayCheck() so that year-rollover data is aged before any
    // recompute or new cold-starts are accumulated.  This handles the case
    // where the device was powered off over New Year and reboots mid-January.
    if (!_startupDecayDone) {
        time_t now = time(nullptr);
        if (now > 0) {
            _startupDecayDone = true;
            decayCheck();  // no-op and no I/O if same year; saves only if year changed
        }
    }

    // Consume any pending cold-start event from Core 1.
    PendingColdStart cs;
    if (xQueueReceive(_coldStartQueue, &cs, 0) == pdTRUE) {
        // Update measured-efficiency counters here on Core 0, not in
        // onNavienState() on Core 1, so _measured[] and _measuredHead are
        // written by exactly one core — no synchronisation needed.
        _measured[_measuredHead].total[cs.dow]++;
        if (cs.recircAtStart) {
            _measured[_measuredHead].covered[cs.dow]++;
        }

        // Update bucket store.  Combined weight matches Python: recency × demand.
        if (!_store.updateBucket(cs.dow, cs.bucket,
                                 /*raw_delta=*/1,
                                 cs.demand_weight * cs.recency_weight)) {
            Serial.printf("NavienLearner: bucket write failed (dow=%d b=%d)\n",
                          cs.dow, cs.bucket);
        }
    }

    // External recompute request (e.g. after POST /buckets).
    // Routes via DECAY_CHECK so a year-boundary recompute (e.g. seeding new
    // buckets right after New Year) ages the data before recomputing.
    if (_recomputeRequested) {
        _recomputeRequested = false;
        _taskState = DECAY_CHECK;
        return;
    }

    // Midnight + 2min check: trigger nightly recompute once per day.
    // Goes via DECAY_CHECK first so year-rollover decay is applied before
    // the new schedule is computed.
    time_t now = time(nullptr);
    if (now > 0) {
        struct tm tm_buf;
        struct tm *t = localtime_r(&now, &tm_buf);
        if (t->tm_hour == 0 && t->tm_min >= 2 &&
            t->tm_yday != _lastRecomputeYday) {
            _lastRecomputeYday = t->tm_yday;
            // Sunday midnight: advance the measured efficiency week slot.
            if (t->tm_wday == 0) {
                advanceMeasuredWeek();
            }
            _taskState = DECAY_CHECK;
        }
    }
}

// ---------------------------------------------------------------------------
// decayCheck() — private; apply annual weighted_score decay if needed (Core 0)
//
// Compares the year stored in the BucketFile header against the current
// calendar year.  If they differ, all weighted_scores are multiplied by 2/3
// to age last-year data from recency weight 3 → 2 (matching Python's
// recency_weights = [3, 2]).  raw_count is never decayed — it is an
// occurrence filter that is independent of recency.
//
// When the year has changed, current_year in the header is updated and the
// file is persisted.  If the year matches, the function returns immediately
// with no I/O.  If the clock is unavailable, the function returns without
// any change; the startup one-shot check will retry on the next tick, and
// the midnight path will retry at the next midnight crossing.
// ---------------------------------------------------------------------------

void NavienLearner::decayCheck() {
    time_t now = time(nullptr);
    if (now <= 0) {
        // Clock not yet set — return without changes; see block comment above.
        return;
    }

    struct tm  tm_buf;
    struct tm *t        = localtime_r(&now, &tm_buf);
    uint16_t  this_year = (uint16_t)(t->tm_year + 1900);
    uint16_t  stored_year = _store.data().current_year;

    if (stored_year == this_year) {
        return;  // same year — nothing to do
    }

    if (stored_year != 0) {
        // Year has rolled over: scale every weighted_score by 2/3.
        // Derivation: data was accumulated during stored_year at recency ×3;
        // it is now "last year" data at recency ×2, so divide by 3/2 = ×(2/3).
        // Multiple-year gaps also apply exactly one decay step (acceptable
        // simplification for long power-off periods).
        BucketFile &bf = _store.data();
        for (int dow = 0; dow < BUCKET_DAYS; dow++) {
            for (int b = 0; b < BUCKET_PER_DAY; b++) {
                bf.buckets[dow][b].weighted_score *= (2.0f / 3.0f);
            }
        }
        Serial.printf("NavienLearner: annual decay applied (%u → %u)\n",
                      stored_year, this_year);
    }

    // Update the year in the header and persist (with or without decay).
    _store.data().current_year = this_year;
    if (!_store.save()) {
        Serial.println("NavienLearner: decay save failed");
    }
}

// ---------------------------------------------------------------------------
// recomputeWrite() — private; builds schedule JSON and hands off (Core 0)
// ---------------------------------------------------------------------------

void NavienLearner::recomputeWrite() {
    // Build JSON in setWeekScheduleFromJSON() format:
    // {"schedule":[{"slots":[{"startHour":H,"startMinute":M,
    //                         "endHour":H,"endMinute":M},...]},...]}
    // Array index 0=Sunday .. 6=Saturday (SchedulerBase order).
    //
    // safeAppend() clamps pos to SCHEDULE_JSON_CAPACITY-1 on overflow so
    // neither buf nor _pendingScheduleJSON can be overrun regardless of data.
    char buf[SCHEDULE_JSON_CAPACITY];
    int  pos     = 0;
    bool ok      = true;

    ok &= safeAppend(buf, SCHEDULE_JSON_CAPACITY, &pos, "{\"schedule\":[");
    for (int dow = 0; dow < BUCKET_DAYS && ok; dow++) {
        ok &= safeAppend(buf, SCHEDULE_JSON_CAPACITY, &pos,
                         "%s{\"slots\":[", dow > 0 ? "," : "");
        for (int s = 0; s < _weekSlotCount[dow] && ok; s++) {
            int sh = _weekSlots[dow][s].start_min / 60;
            int sm = _weekSlots[dow][s].start_min % 60;
            int eh = _weekSlots[dow][s].end_min   / 60;
            int em = _weekSlots[dow][s].end_min   % 60;
            ok &= safeAppend(buf, SCHEDULE_JSON_CAPACITY, &pos,
                             "%s{\"startHour\":%d,\"startMinute\":%d,"
                             "\"endHour\":%d,\"endMinute\":%d}",
                             s > 0 ? "," : "", sh, sm, eh, em);
        }
        ok &= safeAppend(buf, SCHEDULE_JSON_CAPACITY, &pos, "]}");
    }
    ok &= safeAppend(buf, SCHEDULE_JSON_CAPACITY, &pos, "]}");

    if (!ok) {
        // Truncated JSON would fail setWeekScheduleFromJSON() parse — skip handoff
        // entirely so the existing schedule stays in effect and no apply cycle fires.
        Serial.println("NavienLearner: schedule JSON truncated — SCHEDULE_JSON_CAPACITY too small; handoff skipped");
        return;
    }

    // Compute predicted efficiency from the new schedule and bucket data.
    // A bucket with raw_count > 0 is "schedulable" if it falls inside a slot
    // or within HOT_WINDOW_MIN minutes after a slot ends (the pipe stays hot
    // briefly after recirculation stops).
    // predicted% = covered_schedulable / total_schedulable × 100
    static constexpr int HOT_WINDOW_MIN = 15;
    for (int dow = 0; dow < BUCKET_DAYS; dow++) {
        int covered = 0, schedulable = 0;
        for (int b = 0; b < BUCKET_PER_DAY; b++) {
            if (_store.data().buckets[dow][b].raw_count == 0) continue;
            int  bucket_min = b * 5;  // minute-of-day for this bucket
            bool in_slot    = false;
            bool near_after = false;
            for (int s = 0; s < _weekSlotCount[dow]; s++) {
                int start = (int)_weekSlots[dow][s].start_min;
                int end   = (int)_weekSlots[dow][s].end_min;
                if (bucket_min >= start && bucket_min < end) {
                    in_slot = true;
                    break;  // slots don't overlap; no need to check further
                }
                if (bucket_min >= end && bucket_min < end + HOT_WINDOW_MIN) {
                    near_after = true;
                    // keep checking — bucket might fall inside a later slot
                }
            }
            if (in_slot || near_after) {
                schedulable++;
                if (in_slot) covered++;
            }
        }
        _predictedEfficiency[dow] = (schedulable > 0)
            ? (covered * 100.0f / schedulable)
            : NAN;
    }
    _lastRecomputeTime = time(nullptr);

    // Hand off to Core 1 under mutex.  portMAX_DELAY is safe: Core 1 holds
    // this mutex only for a string copy (a few µs), never during NVS writes.
    // pos <= SCHEDULE_JSON_CAPACITY-1 (guaranteed by safeAppend), so
    // memcpy of pos+1 bytes fits within _pendingScheduleJSON[SCHEDULE_JSON_CAPACITY].
    xSemaphoreTake(_scheduleHandoffMutex, portMAX_DELAY);
    memcpy(_pendingScheduleJSON, buf, (size_t)pos + 1);
    _newScheduleReady = true;
    xSemaphoreGive(_scheduleHandoffMutex);

    broadcastUDP();

    Serial.println("NavienLearner: recompute complete, new schedule ready");
}

// ---------------------------------------------------------------------------
// ingestBucketPayload() — public; called from Core 1 during bootstrap only.
// Parses sparse JSON, merges or replaces _buckets in RAM, writes atomically
// to LittleFS, then sets _recomputeRequested so Core 0 runs peak-finding
// immediately rather than waiting for midnight.
//
// Returns the number of individual buckets written, or -1 on error.
// 'replaced' reflects the value of the "replace" field in the payload.
// ---------------------------------------------------------------------------

int NavienLearner::ingestBucketPayload(const char *json, bool &replaced) {
    replaced = false;

    if (_learnerDisabled) {
        Serial.println(F("[learner] POST /buckets: learner disabled — ingest skipped"));
        return -1;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[learner] POST /buckets: JSON parse error: %s\n", err.c_str());
        return -1;
    }

    int schema = doc["schema_version"] | -1;
    if (schema != BUCKET_SCHEMA_VERSION) {
        Serial.printf("[learner] POST /buckets: schema mismatch %d != %d\n",
                      schema, BUCKET_SCHEMA_VERSION);
        return -1;
    }

    int current_year = doc["current_year"] | 0;
    replaced = doc["replace"] | false;
    // finalize=true (default) triggers an immediate recompute after ingest.
    // Set to false by navien_bucket_export.py for all but the last day chunk
    // so that intermediate POST /buckets requests don't kick off a recompute
    // on partial data.
    bool finalize = doc["finalize"] | true;

    // Apply replace/merge to the in-RAM BucketFile directly, then save once
    // at the end rather than calling updateBucket() (which would write flash
    // per bucket — far too slow for a full bootstrap payload).
    //
    // Cross-core note: this method runs on Core 1 (Arduino loop) and mutates
    // _store.data() directly.  Core 0 owns the BucketStore for all normal
    // operations; no mutex guards this path.  Bootstrap must be run while the
    // device is quiet — no active recompute and no concurrent cold-start flush
    // in progress — to avoid a data race on the in-RAM BucketFile.
    BucketFile &bf = _store.data();
    if (replaced) {
        memset(bf.buckets, 0, sizeof(bf.buckets));
        bf.magic          = BUCKET_MAGIC;
        bf.schema_version = BUCKET_SCHEMA_VERSION;
        // Always write an explicit year on replace — same clock fallback as
        // BucketStore::begin() so the header is never left at a stale value.
        if (current_year <= 0) {
            time_t now = time(nullptr);
            struct tm *t = localtime(&now);
            current_year = (t && t->tm_year > 100) ? (t->tm_year + 1900) : 2025;
        }
        bf.current_year = (uint16_t)current_year;
    } else if (current_year > 0) {
        bf.current_year = (uint16_t)current_year;
    }

    int count = 0;
    for (JsonObject day : doc["days"].as<JsonArray>()) {
        int dow = day["dow"] | -1;
        if (dow < 0 || dow > 6) continue;
        for (JsonObject bkt : day["buckets"].as<JsonArray>()) {
            int b = bkt["b"] | -1;
            if (b < 0 || b >= BUCKET_PER_DAY) continue;
            uint16_t raw   = (uint16_t)(bkt["raw"]   | 0);
            float    score = bkt["score"] | 0.0f;
            bf.buckets[dow][b].raw_count      += raw;
            bf.buckets[dow][b].weighted_score += score;
            count++;
        }
    }

    if (!_store.save()) {
        Serial.println(F("[learner] POST /buckets: save failed"));
        return -1;
    }

    // Signal Core 0 to run peak-finding immediately with the new data.
    // Set after save() so Core 0 reads a fully consistent LittleFS image.
    // Suppressed for intermediate chunk requests (finalize=false) to avoid
    // running peak-finding on partial data when bootstrap sends one day at a time.
    if (finalize)
        _recomputeRequested = true;

    Serial.printf("[learner] POST /buckets: wrote %d buckets, replaced=%s, finalize=%s\n",
                  count, replaced ? "true" : "false", finalize ? "true" : "false");
    return count;
}

// ---------------------------------------------------------------------------
// broadcastUDP() — private; emits a "type":"learner" JSON packet over UDP.
// Called from recomputeWrite() on Core 0 after mutex handoff.
// Uses ArduinoJson (same pattern as NavienBroadcaster.ino).
// ---------------------------------------------------------------------------

void NavienLearner::broadcastUDP() {
    // 3-letter day prefixes used to build flat field names (e.g. "sun_slots").
    // Flat top-level keys allow navien_listener.py to pass payload['fields'] = data
    // directly to InfluxDB without any flattening — nested arrays/objects would
    // fail InfluxDB line protocol.
    static const char *dayPfx[] = {
        "sun","mon","tue","wed","thu","fri","sat"
    };

    JsonDocument doc;
    doc["type"] = "learner";
    doc["last_recompute"] = (long)_lastRecomputeTime;
    doc["bucket_fill_pct"] = serialized(
        String(_store.nonZeroCount() * 100.0f / (BUCKET_DAYS * BUCKET_PER_DAY), 1));

    char key[32];
    for (int dow = 0; dow < BUCKET_DAYS; dow++) {
        // Encode slots as compact string "HH:MM-HH:MM,..." (empty if no slots).
        char slotStr[MAX_SLOTS_PER_DAY * 12] = "";  // "HH:MM-HH:MM," per slot
        int  spos = 0;
        for (int s = 0; s < _weekSlotCount[dow]; s++) {
            spos += snprintf(slotStr + spos, (int)sizeof(slotStr) - spos,
                             "%s%02d:%02d-%02d:%02d",
                             s > 0 ? "," : "",
                             _weekSlots[dow][s].start_min / 60,
                             _weekSlots[dow][s].start_min % 60,
                             _weekSlots[dow][s].end_min   / 60,
                             _weekSlots[dow][s].end_min   % 60);
        }
        snprintf(key, sizeof(key), "%s_slots", dayPfx[dow]);
        doc[key] = slotStr;  // ArduinoJson v7 copies both key and value

        float pred = _predictedEfficiency[dow];
        if (!isnan(pred)) {
            snprintf(key, sizeof(key), "%s_predicted_pct", dayPfx[dow]);
            doc[key] = serialized(String(pred, 1));
        }

        uint32_t measTotal = 0, measCovered = 0;
        for (int w = 0; w < 4; w++) {
            measTotal   += _measured[w].total[dow];
            measCovered += _measured[w].covered[dow];
        }
        if (measTotal > 0) {
            float meas = measCovered * 100.0f / measTotal;
            snprintf(key, sizeof(key), "%s_measured_pct", dayPfx[dow]);
            doc[key] = serialized(String(meas, 1));
            if (!isnan(pred)) {
                snprintf(key, sizeof(key), "%s_gap_pct", dayPfx[dow]);
                doc[key] = serialized(String(pred - meas, 1));
            }
        }

        snprintf(key, sizeof(key), "%s_cold_starts_4wk", dayPfx[dow]);
        doc[key] = (int)measTotal;
    }

    doc["debug"] = "";

    String json;
    serializeJson(doc, json);
    udp.broadcastTo(json.c_str(), UDP_BROADCAST_PORT);
}

// ---------------------------------------------------------------------------
// appendStatusHTML() — build Learner Status section for the web status page.
// Called from navienStatus() on Core 1; reads _measured[] as coarse stats
// without locking (see header comment on _measured).
// ---------------------------------------------------------------------------

void NavienLearner::appendStatusHTML(String &page) const {
    static const char *dayNames[] = {
        "Sunday","Monday","Tuesday","Wednesday",
        "Thursday","Friday","Saturday"
    };

    page += "<h2>Learner Status</h2>";

    // Last recompute timestamp and age.
    if (_lastRecomputeTime > 0) {
        char tbuf[32];
        struct tm  tm_buf;
        struct tm *t = localtime_r(&_lastRecomputeTime, &tm_buf);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", t);
        time_t elapsed = time(nullptr) - _lastRecomputeTime;
        char abuf[32];
        if (elapsed >= 3600) {
            snprintf(abuf, sizeof(abuf), "%dh ago", (int)(elapsed / 3600));
        } else {
            snprintf(abuf, sizeof(abuf), "%dmin ago", (int)(elapsed / 60));
        }
        page += "<p>Last recompute: ";
        page += tbuf;
        page += " (";
        page += abuf;
        page += ")</p>";
    } else {
        page += "<p>Last recompute: never</p>";
    }

    // Bucket fill.
    int nonZero = _store.nonZeroCount();
    int total   = BUCKET_DAYS * BUCKET_PER_DAY;
    {
        char fbuf[64];
        snprintf(fbuf, sizeof(fbuf),
                 "<p>Bucket fill: %d / %d non-zero (%.1f%%)</p>",
                 nonZero, total, nonZero * 100.0f / total);
        page += fbuf;
    }

    // Per-day efficiency table.
    page += "<table style='margin:auto;border-collapse:collapse;color:white;font-size:14px'>"
            "<tr>"
            "<th style='padding:4px 12px;text-align:left'>Day</th>"
            "<th style='padding:4px 12px'>Predicted</th>"
            "<th style='padding:4px 12px'>Measured</th>"
            "<th style='padding:4px 12px'>Gap</th>"
            "<th style='padding:4px 12px'>Cold-starts (4wk)</th>"
            "</tr>";

    float sumPred = 0.0f, sumMeas = 0.0f;
    int   cntPred = 0,    cntMeas = 0;

    for (int dow = 0; dow < BUCKET_DAYS; dow++) {
        uint32_t tot = 0, cov = 0;
        for (int w = 0; w < 4; w++) {
            tot += _measured[w].total[dow];
            cov += _measured[w].covered[dow];
        }
        float measPct = (tot > 0) ? (cov * 100.0f / tot) : NAN;
        float predPct = _predictedEfficiency[dow];

        char predStr[12], measStr[12], gapStr[16];
        const char *gapColor = "white";

        if (!isnan(predPct)) {
            snprintf(predStr, sizeof(predStr), "%.1f%%", predPct);
            sumPred += predPct;
            cntPred++;
        } else {
            snprintf(predStr, sizeof(predStr), "N/A");
        }
        if (!isnan(measPct)) {
            snprintf(measStr, sizeof(measStr), "%.1f%%", measPct);
            sumMeas += measPct;
            cntMeas++;
        } else {
            snprintf(measStr, sizeof(measStr), "N/A");
        }
        if (!isnan(predPct) && !isnan(measPct)) {
            float gap = predPct - measPct;
            snprintf(gapStr, sizeof(gapStr), "%+.1f%%", gap);
            float absGap = fabsf(gap);
            gapColor = (absGap < 10.0f) ? "#28a745"
                     : (absGap < 25.0f) ? "#ffc107"
                                        : "#dc3545";
        } else {
            snprintf(gapStr, sizeof(gapStr), "N/A");
        }

        page += "<tr><td style='padding:4px 12px;text-align:left'>";
        page += dayNames[dow];
        page += "</td><td style='padding:4px 12px;text-align:center'>";
        page += predStr;
        page += "</td><td style='padding:4px 12px;text-align:center'>";
        page += measStr;
        page += "</td><td style='padding:4px 12px;text-align:center;color:";
        page += gapColor;
        page += "'>";
        page += gapStr;
        page += "</td><td style='padding:4px 12px;text-align:center'>";
        page += String((uint32_t)tot);
        page += "</td></tr>";
    }

    // Weekly average row.
    char avgPred[12] = "N/A", avgMeas[12] = "N/A";
    if (cntPred > 0) snprintf(avgPred, sizeof(avgPred), "%.1f%%", sumPred / cntPred);
    if (cntMeas > 0) snprintf(avgMeas, sizeof(avgMeas), "%.1f%%", sumMeas / cntMeas);
    page += "<tr style='border-top:1px solid #555'>"
            "<td style='padding:4px 12px;text-align:left'><b>Weekly avg</b></td>"
            "<td style='padding:4px 12px;text-align:center'><b>";
    page += avgPred;
    page += "</b></td><td style='padding:4px 12px;text-align:center'><b>";
    page += avgMeas;
    page += "</b></td><td></td><td></td></tr>";
    page += "</table>";
}

// ---------------------------------------------------------------------------
// checkNewSchedule() — called from FakeGatoScheduler::loop() on Core 1
// ---------------------------------------------------------------------------

bool NavienLearner::checkNewSchedule(String &out_json) {
    if (_scheduleHandoffMutex == nullptr) return false;
    if (xSemaphoreTake(_scheduleHandoffMutex, 0) != pdTRUE) return false;
    bool has_new = _newScheduleReady;
    if (has_new) {
        out_json = String(_pendingScheduleJSON);
        _newScheduleReady = false;
    }
    xSemaphoreGive(_scheduleHandoffMutex);
    return has_new;
}

// ---------------------------------------------------------------------------
// computeDemandWeight() — private
// ---------------------------------------------------------------------------

float NavienLearner::computeDemandWeight(bool recircAtStart,
                                          uint32_t durationSec) const {
    if (recircAtStart) {
        // Recirc was running: only count if tap lasted long enough to
        // represent real demand (not just a sensor blip).
        return (durationSec >= MIN_DURATION_RECIRC_SEC) ? 1.0f : 0.0f;
    } else {
        // No recirc: cold-pipe drain.  Short taps get half weight.
        if (durationSec >= MIN_DURATION_GENUINE_SEC) {
            return 1.0f;
        } else {
            return 0.5f;  // short cold-pipe tap — over-weighted vs Python
                          // (cost_multiplier omitted; see spec §Demand Weight)
        }
    }
}
