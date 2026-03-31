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
#include <stdarg.h>

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
      _coldStartQueue(nullptr),
      _taskHandle(nullptr),
      _recomputeRequested(false),
      _taskState(IDLE),
      _recomputeDay(0),
      _lastRecomputeYday(-1),
      _scheduleHandoffMutex(nullptr),
      _newScheduleReady(false),
      _measuredHead(0),
      _learnerDisabled(false)
{
    memset(_measured,            0, sizeof(_measured));
    memset(_weekSlots,           0, sizeof(_weekSlots));
    memset(_weekSlotCount,       0, sizeof(_weekSlotCount));
    memset(_predictedEfficiency, 0, sizeof(_predictedEfficiency));
    memset(_pendingScheduleJSON, 0, sizeof(_pendingScheduleJSON));
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

    Serial.println("NavienLearner: ready");
    return true;
}

// ---------------------------------------------------------------------------
// onNavienState() — cold-start detector (Core 1)
// ---------------------------------------------------------------------------

void NavienLearner::onNavienState(bool consumption_active,
                                   bool recirculation_running,
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
            _recircAtStart   = recirculation_running;
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
}

// ---------------------------------------------------------------------------
// advanceMeasuredWeek() — called at Sunday midnight (Core 0 task)
// ---------------------------------------------------------------------------

void NavienLearner::advanceMeasuredWeek() {
    _measuredHead = (_measuredHead + 1) % 4;
    memset(&_measured[_measuredHead], 0, sizeof(WeekMeasured));
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
    if (_recomputeRequested) {
        _recomputeRequested = false;
        _taskState = RECOMPUTE_LOAD;
        return;
    }

    // Midnight + 2min check: trigger nightly recompute once per day.
    // Phase 6 will insert DECAY_CHECK before RECOMPUTE_LOAD.
    time_t now = time(nullptr);
    if (now > 0) {
        struct tm *t = localtime(&now);
        if (t->tm_hour == 0 && t->tm_min >= 2 &&
            t->tm_yday != _lastRecomputeYday) {
            _lastRecomputeYday = t->tm_yday;
            // Sunday midnight: advance the measured efficiency week slot.
            if (t->tm_wday == 0) {
                advanceMeasuredWeek();
            }
            _taskState = RECOMPUTE_LOAD;
        }
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

    // Phase 7 will compute _predictedEfficiency[] here.

    // Hand off to Core 1 under mutex.  portMAX_DELAY is safe: Core 1 holds
    // this mutex only for a string copy (a few µs), never during NVS writes.
    // pos <= SCHEDULE_JSON_CAPACITY-1 (guaranteed by safeAppend), so
    // memcpy of pos+1 bytes fits within _pendingScheduleJSON[SCHEDULE_JSON_CAPACITY].
    xSemaphoreTake(_scheduleHandoffMutex, portMAX_DELAY);
    memcpy(_pendingScheduleJSON, buf, (size_t)pos + 1);
    _newScheduleReady = true;
    xSemaphoreGive(_scheduleHandoffMutex);

    // Phase 8 will call broadcastUDP() here.

    Serial.println("NavienLearner: recompute complete, new schedule ready");
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
