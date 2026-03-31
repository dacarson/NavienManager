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

#pragma once

#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "BucketStore.h"

// ---------------------------------------------------------------------------
// Cold-start event transported from Core 1 → Core 0
// ---------------------------------------------------------------------------

struct PendingColdStart {
    int   dow;             // day of week at tap-open time (0=Sun)
    int   bucket;          // 5-minute bucket index at tap-open time (0–287)
    float demand_weight;   // 0.5 (short cold-pipe tap) or 1.0 (genuine demand)
    float recency_weight;  // current year's recency weight multiplier (3.0)
};

// ---------------------------------------------------------------------------
// Rolling measured-efficiency window — 4 weeks × 7 days
// ---------------------------------------------------------------------------

struct WeekMeasured {
    uint16_t total[7];    // cold-starts observed per day-of-week this week
    uint16_t covered[7];  // cold-starts where recirc was already running
};

// ---------------------------------------------------------------------------
// NavienLearner
// ---------------------------------------------------------------------------

class NavienLearner {
public:
    NavienLearner();

    // Initialise: create FreeRTOS queue and BucketStore.
    // Returns false if queue allocation fails; the learner is then silently
    // inactive but does not crash.  Must be called before onNavienState().
    bool begin();

    // Called from the water packet callback on Core 1 for every RS-485 packet.
    // Detects cold-starts and enqueues PendingColdStart events for Core 0.
    // Returns immediately if the learner is disabled.
    void onNavienState(bool consumption_active,
                       bool recirculation_running,
                       time_t now);

    // Access to the cross-core cold-start queue (consumed by Core 0 task).
    QueueHandle_t coldStartQueue() const { return _coldStartQueue; }

    // Access to the BucketStore for Core 0 updates.
    BucketStore &bucketStore() { return _store; }

    // Rolling measured efficiency — 4-week window.
    // Indexed [week_slot][dow]; week_slot rotates on Sunday midnight.
    const WeekMeasured *measuredWindow() const { return _measured; }
    uint8_t measuredHead() const { return _measuredHead; }

    // Advance the rolling window to a new week (called at Sunday midnight).
    void advanceMeasuredWeek();

    bool isDisabled() const { return _learnerDisabled; }

    // Cold-start detection thresholds (seconds)
    static constexpr uint32_t COLD_GAP_SEC             = 600; // 10 min
    static constexpr uint32_t MIN_DURATION_GENUINE_SEC  = 60;  // 6 × 10s
    static constexpr uint32_t MIN_DURATION_RECIRC_SEC   = 30;  // 3 × 10s

    // Recency weight for live (current-year) data, matching Python [3, 2]
    static constexpr float RECENCY_WEIGHT_CURRENT = 3.0f;

private:
    // Compute demand_weight from run characteristics.
    // Returns 0.0 if the event should be discarded.
    float computeDemandWeight(bool recircAtStart, uint32_t durationSec) const;

    // --- Cold-start detector state (Core 1 only) ---
    time_t   _lastActiveTime;    // last time consumption_active was true
    bool     _inRun;             // currently inside an active run
    time_t   _runStart;          // when the current run started
    uint32_t _runDurationSec;    // elapsed seconds of current run
    int      _runDow;            // day-of-week pinned at run start (0=Sun)
    int      _runBucket;         // 5-min bucket index pinned at run start
    bool     _recircAtStart;     // was recirc running when run started?

    // --- Cross-core queue (capacity 1, Core 1 writes, Core 0 reads) ---
    QueueHandle_t _coldStartQueue;

    // --- Measured efficiency rolling window (Core 1 writes) ---
    WeekMeasured _measured[4];
    uint8_t      _measuredHead;  // index of current week slot (0–3)

    // --- Persistent bucket storage (Core 0 reads/writes) ---
    BucketStore _store;

    bool _learnerDisabled;
};
