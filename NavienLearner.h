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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "BucketStore.h"
#include "PeakFinder.h"

// Arduino String (WString.h) — forward declare so this header does not depend
// on Arduino.h; translation units that call checkNewSchedule() must include it.
class String;

// ---------------------------------------------------------------------------
// Cold-start event transported from Core 1 → Core 0
// ---------------------------------------------------------------------------

struct PendingColdStart {
    int   dow;             // day of week at tap-open time (0=Sun)
    int   bucket;          // 5-minute bucket index at tap-open time (0–287)
    float demand_weight;   // 0.5 (short cold-pipe tap) or 1.0 (genuine demand)
    float recency_weight;  // current year's recency weight multiplier (3.0)
    bool  recircAtStart;   // was recirc running when the run started?
    //                        (used by Core 0 to update measured-efficiency counters)
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

    // Signal the Core 0 task to run a recompute immediately (e.g. after
    // POST /buckets seeds new bucket data).  Safe to call from any core.
    void requestRecompute() { _recomputeRequested = true; }

    // Ingest a sparse bucket payload from POST /buckets (called from Core 1
    // during bootstrap only).  Parses JSON, merges or replaces _buckets in
    // RAM, writes atomically to LittleFS, then sets _recomputeRequested.
    // Returns the number of individual buckets written, or -1 on error
    // (schema mismatch, parse failure, or save failure).
    // 'replaced' is set to reflect the value of the "replace" field.
    int ingestBucketPayload(const char *json, bool &replaced);

    // Called from FakeGatoScheduler::loop() on Core 1.  Non-blocking: returns
    // false immediately if no new schedule is ready or the mutex is held.
    // Returns true and fills out_json with the schedule JSON if ready.
    bool checkNewSchedule(String &out_json);

    // Wall time of the last completed RECOMPUTE_WRITE (0 = never).
    time_t lastRecomputeTime() const { return _lastRecomputeTime; }

    // Per-day predicted efficiency from the last recompute (NAN if insufficient
    // bucket data).  Index 0=Sunday .. 6=Saturday.
    const float *predictedEfficiency() const { return _predictedEfficiency; }

    // Append a Learner Status HTML section to page.  Called from the web status
    // callback on Core 1; reads _measured[] as coarse stats without locking.
    void appendStatusHTML(String &page) const;

    bool isDisabled() const { return _learnerDisabled; }

    // Cold-start detection thresholds (seconds)
    static constexpr uint32_t COLD_GAP_SEC             = 600; // 10 min
    static constexpr uint32_t MIN_DURATION_GENUINE_SEC  = 60;  // 6 × 10s
    static constexpr uint32_t MIN_DURATION_RECIRC_SEC   = 30;  // 3 × 10s

    // Recency weight for live (current-year) data, matching Python [3, 2]
    static constexpr float RECENCY_WEIGHT_CURRENT = 3.0f;

    // Core 0 task entry point — launched by begin().
    static void learnerTask(void *pvParam);

private:
    // Task state machine states (Core 0).
    enum TaskState { IDLE, DECAY_CHECK, RECOMPUTE_LOAD, RECOMPUTING, RECOMPUTE_WRITE };

    // Compute demand_weight from run characteristics.
    // Returns 0.0 if the event should be discarded.
    float computeDemandWeight(bool recircAtStart, uint32_t durationSec) const;

    // Core 0 state machine helpers.
    void idleStep();        // called every IDLE tick: queue drain, midnight check
    void decayCheck();      // apply annual weighted_score decay if year has rolled over
    void recomputeWrite();  // builds JSON and hands off to Core 1 via mutex
    void broadcastUDP();    // broadcasts learner JSON packet over UDP (Phase 8)

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

    // --- Core 0 task ---
    TaskHandle_t  _taskHandle;
    volatile bool _recomputeRequested;  // set from any core, cleared on Core 0
    TaskState     _taskState;
    int           _recomputeDay;        // 0–6; current day being processed in RECOMPUTING
    int           _lastRecomputeYday;   // tm_yday of last midnight recompute trigger (-1 = never)
    bool          _startupDecayDone;    // true once the one-shot startup decay check has run
    time_t        _lastRecomputeTime;   // wall time of last RECOMPUTE_WRITE (0 = never)

    // Capacity for the schedule JSON buffer.
    // Worst case: 7 days × 3 slots, fully expanded ≈ 1309 bytes; 1400 gives margin.
    static constexpr int SCHEDULE_JSON_CAPACITY = 1400;

    // --- Schedule handoff (Core 0 writes, Core 1 reads — guarded by mutex) ---
    SemaphoreHandle_t _scheduleHandoffMutex;
    char              _pendingScheduleJSON[SCHEDULE_JSON_CAPACITY];
    bool              _newScheduleReady;

    // --- Recompute results (Core 0 only) ---
    TimeSlot _weekSlots[7][MAX_SLOTS_PER_DAY];  // slots per day from last recompute
    int      _weekSlotCount[7];                  // slot count per day (0–MAX_SLOTS_PER_DAY)
    float    _predictedEfficiency[7];            // per-day predicted efficiency (Phase 7)

    // --- Measured efficiency rolling window (Core 0 writes only) ---
    // Updated in idleStep() when consuming cold-start events from the queue.
    // Phase 7 Telnet/UI readers on Core 1 should treat these as coarse stats
    // (no lock needed for read-only display, but values may be mid-update).
    WeekMeasured _measured[4];
    uint8_t      _measuredHead;  // index of current week slot (0–3)

    // --- Persistent bucket storage (Core 0 reads/writes) ---
    BucketStore _store;

    bool _learnerDisabled;
};
