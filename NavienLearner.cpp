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
      _measuredHead(0),
      _learnerDisabled(false)
{
    memset(_measured, 0, sizeof(_measured));
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

    if (!_store.begin()) {
        Serial.println("NavienLearner: BucketStore init failed — learner disabled");
        _learnerDisabled = true;
        // Queue was created above but is left live; same intentional tradeoff
        // as the task-create failure path below.
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
            // Update measured efficiency counters (Core 1 owned).
            _measured[_measuredHead].total[_runDow]++;
            if (_recircAtStart) {
                _measured[_measuredHead].covered[_runDow]++;
            }

            // Enqueue for Core 0 bucket accumulation.
            PendingColdStart cs;
            cs.dow            = _runDow;
            cs.bucket         = _runBucket;
            cs.demand_weight  = demand_weight;
            cs.recency_weight = RECENCY_WEIGHT_CURRENT;
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
// learnerTask() — Core 0 background task
// ---------------------------------------------------------------------------

void NavienLearner::learnerTask(void *pvParam) {
    NavienLearner *self = static_cast<NavienLearner *>(pvParam);

    for (;;) {
        // IDLE: consume any pending cold-start event from Core 1.
        // xQueueReceive with timeout 0 is non-blocking — if the queue is
        // empty we fall through immediately and sleep for 500ms.
        PendingColdStart cs;
        if (xQueueReceive(self->_coldStartQueue, &cs, 0) == pdTRUE) {
            // Update the in-RAM bucket and persist atomically to LittleFS.
            // Combined weight matches Python: recency_weight × demand_weight.
            if (!self->_store.updateBucket(cs.dow, cs.bucket,
                                           /*raw_delta=*/1,
                                           cs.demand_weight * cs.recency_weight)) {
                Serial.printf("NavienLearner: bucket write failed (dow=%d b=%d)\n",
                              cs.dow, cs.bucket);
            }
        }

        // Phase 5 will replace this stub with a full RECOMPUTE_LOAD transition.
        if (self->_recomputeRequested) {
            self->_recomputeRequested = false;
            // TODO Phase 5: trigger RECOMPUTE_LOAD
        }

        // Phase 6 will add the midnight + 2min check → DECAY_CHECK → RECOMPUTE_LOAD.

        vTaskDelay(pdMS_TO_TICKS(500));
    }
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
