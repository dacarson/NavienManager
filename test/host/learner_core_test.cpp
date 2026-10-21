// Host-side test of the learner core behind the platform seam
// (LearnerPlatform.h): proves NavienLearner, PeakFinder, BucketStore,
// SchedulerBase and TimeUtils build with no Arduino, HomeSpan, LittleFS or
// FreeRTOS, and exercises the seams themselves (storage, clock, UTC offset,
// struct schedule handoff, taskStep). The sketch's own behaviour is unchanged
// by the seam; this is the regression net for the files that other firmware
// (navien-bridge) embeds.
//
// Run with `python test/host/run.py`.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "LearnerPlatform.h"
#include "NavienLearner.h"
#include "SchedulerBase.h"
#include "TimeUtils.h"

namespace learner_host {
time_t now = 0;
int utc_offset_min = 0;
bool verbose = false;
}  // namespace learner_host

// ---------------------------------------------------------------------------
// In-memory storage
// ---------------------------------------------------------------------------

class MemStorage : public LearnerStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> blobs;
  int saves = 0;
  bool fail_saves = false;

  bool load(const char *name, void *buf, size_t len) override {
    auto it = blobs.find(name);
    if (it == blobs.end() || it->second.size() != len) return false;
    memcpy(buf, it->second.data(), len);
    return true;
  }
  bool save(const char *name, const void *buf, size_t len) override {
    if (fail_saves) return false;
    saves++;
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    blobs[name].assign(p, p + len);
    return true;
  }
};

static MemStorage g_storage;
LearnerStorage &learnerStorage() { return g_storage; }

// ---------------------------------------------------------------------------
// Scaffolding
// ---------------------------------------------------------------------------

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    g_checks++;                                                         \
    if (!(cond)) {                                                      \
      g_failures++;                                                     \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                   \
  } while (0)

static time_t utc(int y, int mo, int d, int h, int mi, int s = 0) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = s;
  return proper_timegm(&t);
}

static void reset() {
  g_storage.blobs.clear();
  g_storage.saves = 0;
  g_storage.fail_saves = false;
  learner_host::utc_offset_min = 0;
  learner_host::now = utc(2026, 10, 12, 12, 0);  // Monday
}

// Feed `seconds` of tap-open samples at 10 s cadence, then one closed sample.
static void run_tap(NavienLearner &l, uint32_t seconds, bool recirc) {
  for (uint32_t t = 0; t < seconds; t += 10) {
    l.onWaterSample(true, recirc);
    learner_host::now += 10;
  }
  l.onWaterSample(false, recirc);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_bucket_store_persists_through_storage() {
  reset();
  BucketStore a;
  CHECK(a.begin());
  CHECK(g_storage.blobs.count("buckets") == 1);
  CHECK(a.data().magic == BUCKET_MAGIC);
  CHECK(a.data().current_year == 2026);
  CHECK(a.updateBucket(1, 84, 2, 6.0f));

  BucketStore b;
  CHECK(b.begin());
  CHECK(b.data().buckets[1][84].raw_count == 2);
  CHECK(b.data().buckets[1][84].weighted_score == 6.0f);
  CHECK(b.nonZeroCount() == 1);

  // A corrupt record is rejected and recreated empty.
  g_storage.blobs["buckets"][0] ^= 0xFF;
  BucketStore c;
  CHECK(c.begin());
  CHECK(c.nonZeroCount() == 0);
}

static void test_demand_event_lands_in_bucket() {
  reset();
  learner_host::now = utc(2026, 10, 12, 7, 2);  // Monday 07:02 UTC -> bucket 84
  NavienLearner l;
  CHECK(l.begin());
  CHECK(!l.isDisabled());

  run_tap(l, 120, /*recirc=*/false);  // 2 minutes, cold pipes: weight 1.0
  // Not billed until the cold gap passes with no resumption.
  l.taskStep();
  CHECK(l.bucketStore().data().buckets[1][84].raw_count == 0);
  learner_host::now += NavienLearner::COLD_GAP_SEC + 1;
  l.onWaterSample(false, false);
  l.taskStep();
  CHECK(l.bucketStore().data().buckets[1][84].raw_count == 1);
  CHECK(l.bucketStore().data().buckets[1][84].weighted_score == 3.0f);  // 1.0 * recency 3
  CHECK(l.measuredWindow()[l.measuredHead()].total[1] == 1);
  CHECK(l.measuredWindow()[l.measuredHead()].covered[1] == 0);
}

static void test_short_tap_with_recirc_is_discarded() {
  reset();
  learner_host::now = utc(2026, 10, 12, 7, 2);
  NavienLearner l;
  CHECK(l.begin());
  run_tap(l, 20, /*recirc=*/true);  // under MIN_DURATION_RECIRC_SEC
  learner_host::now += NavienLearner::COLD_GAP_SEC + 1;
  l.onWaterSample(false, true);
  l.taskStep();
  CHECK(l.bucketStore().nonZeroCount() == 0);
}

static void test_recompute_hands_off_local_slots() {
  reset();
  learner_host::utc_offset_min = 480;              // PST: UTC = local + 480
  learner_host::now = utc(2026, 10, 12, 20, 0);    // 12:00 local
  NavienLearner l;
  CHECK(l.begin());

  // Seed a strong weekday-morning habit: local 07:00 = UTC 15:00 = bucket 180.
  BucketFile &bf = l.bucketStore().data();
  for (int dow = 1; dow <= 5; dow++) {
    bf.buckets[dow][180].raw_count = 20;
    bf.buckets[dow][180].weighted_score = 60.0f;
    bf.buckets[dow][181].raw_count = 8;
    bf.buckets[dow][181].weighted_score = 24.0f;
  }
  bf.accumulation_start_epoch = (uint32_t) (learner_host::now - 8 * 604800);
  CHECK(l.bucketStore().save());

  l.requestRecompute();
  LearnedSlot slots[21];
  int count = 0, offset = 0;
  bool got = false;
  for (int i = 0; i < 50 && !got; i++) {
    l.taskStep();
    got = l.checkNewSchedule(slots, 21, count, offset);
  }
  CHECK(got);
  CHECK(offset == 480);
  CHECK(count == 5);
  int weekdays = 0;
  for (int i = 0; i < count; i++) {
    if (slots[i].local_dow >= 1 && slots[i].local_dow <= 5) weekdays++;
    // Window brackets 07:00 local (420 min) with the preheat lead.
    CHECK(slots[i].start_min <= 420 && slots[i].end_min >= 420);
    CHECK(slots[i].score > 0.0f);
  }
  CHECK(weekdays == 5);
  CHECK(l.lastRecomputeTime() == learner_host::now);
  // The handoff is one-shot, and a false return leaves count/offset defined.
  count = 99;
  offset = 99;
  CHECK(!l.checkNewSchedule(slots, 21, count, offset));
  CHECK(count == 0 && offset == 0);
  // A caller buffer smaller than the schedule gets the first `cap` slots and
  // a log line, not an overrun.
  l.requestRecompute();
  LearnedSlot two[2];
  got = false;
  for (int i = 0; i < 50 && !got; i++) {
    l.taskStep();
    got = l.checkNewSchedule(two, 2, count, offset);
  }
  CHECK(got && count == 2 && offset == 480);
  // Predicted efficiency is populated for the seeded days.
  CHECK(l.predictedEfficiency()[1] > 99.0f);
}

static void test_measured_window_persists() {
  reset();
  learner_host::now = utc(2026, 10, 12, 7, 2);
  {
    NavienLearner l;
    CHECK(l.begin());
    run_tap(l, 120, false);
    learner_host::now += NavienLearner::COLD_GAP_SEC + 1;
    l.onWaterSample(false, false);
    l.taskStep();
    CHECK(l.saveMeasured());
  }
  NavienLearner m;
  CHECK(m.begin());
  CHECK(m.measuredWindow()[m.measuredHead()].total[1] == 1);
}

static void test_storage_failure_disables_learner_cleanly() {
  reset();
  g_storage.fail_saves = true;
  NavienLearner l;
  CHECK(!l.begin());
  CHECK(l.isDisabled());
  l.onWaterSample(true, false);  // must not crash or enqueue
  l.taskStep();
}

// Scheduler driven through the seam: no NVS, no SNTP, fire slots installed by
// the derived class the way an embedding firmware would.
class TestScheduler : public SchedulerBase {
 public:
  std::vector<State> changes;
  int loads = 0;

  void install(uint8_t dow, int start_min, int end_min) {
    appendUtcFireSlot(dow, start_min / 60, start_min % 60, end_min / 60, end_min % 60, 1.0f);
  }
  bool try_install(uint8_t dow, int start_min, int end_min) {
    return appendUtcFireSlot(dow, start_min / 60, start_min % 60, end_min / 60, end_min % 60, 1.0f);
  }
  // A local window through the scheduler's own conversion and split.
  bool install_local(int local_dow, int start_min, int end_min, int offset_min) {
    int dow, start, end;
    localWindowToUtc(local_dow, start_min, end_min, offset_min, dow, start, end);
    return appendUtcFireWindow(dow, start, end, 1.0f);
  }
  bool active_at(int dow, int hour, int minute) const { return isActiveOnFireSlots(dow, hour, minute); }
  void reevaluate() { initializeCurrentState(); }

 protected:
  void stateChange(State s) override { changes.push_back(s); }
  bool loadScheduleFromStorage() override {
    loads++;
    memset(weekSchedule, 0xFF, sizeof(weekSchedule));
    return true;  // nothing from initDefault()
  }
};

static void test_scheduler_runs_without_nvs_or_sntp() {
  reset();
  learner_host::now = utc(2026, 10, 12, 6, 0);  // Monday 06:00 UTC
  TestScheduler s;
  CHECK(s.begin() != 0);
  CHECK(s.loads == 1);
  s.install(1, 7 * 60, 9 * 60);  // Monday 07:00-09:00 UTC

  s.loop();  // first synced pass initialises
  CHECK(s.isReady());
  CHECK(s.getCurrentState() == SchedulerBase::InActive);
  time_t next = 0;
  CHECK(s.getNextState(&next) == SchedulerBase::Active);
  CHECK(next == utc(2026, 10, 12, 7, 0));

  learner_host::now = utc(2026, 10, 12, 7, 0);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::Active);
  CHECK(!s.changes.empty() && s.changes.back() == SchedulerBase::Active);

  learner_host::now = utc(2026, 10, 12, 9, 0);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::InActive);

  // Override forces Active for its duration, then reverts to the schedule.
  s.activateOverride(5);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::Override);
  learner_host::now += 5 * 60 + 1;
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::InActive);

  // Vacation suppresses the next window. With a once-a-week slot already past
  // for today, getNextState() finds no transition inside its 7-day scan and
  // leaves nextStateChangeTime at 0, so after a jump the first loop() only
  // recomputes it and the second applies it; on the device loop() runs
  // continuously and this is one tick.
  s.setVacationState(true);
  learner_host::now = utc(2026, 10, 19, 7, 30);  // next Monday, inside the window
  s.loop();
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::Vacation);

  // Clearing vacation does not re-evaluate by itself: SchedulerBase leaves
  // that to the caller (FakeGatoScheduler re-initialises after Eve writes;
  // an embedding scheduler must do the same).
  s.setVacationState(false);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::Vacation);
  s.reevaluate();
  CHECK(s.getCurrentState() == SchedulerBase::Active);  // inside Monday's window
  learner_host::now = utc(2026, 10, 19, 9, 0);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::InActive);
}

static void test_scheduler_waits_for_clock() {
  reset();
  learner_host::now = 1234;  // unsynced
  TestScheduler s;
  s.begin();
  s.install(1, 7 * 60, 9 * 60);
  s.loop();
  CHECK(!s.isReady());
  learner_host::now = utc(2026, 10, 12, 8, 0);
  s.loop();
  CHECK(s.isReady());
  CHECK(s.getCurrentState() == SchedulerBase::Active);
}

static void test_get_time_slot_bounds() {
  reset();
  learner_host::now = utc(2026, 10, 12, 8, 0);
  TestScheduler s;
  s.begin();
  uint8_t a, b, c, d;
  CHECK(!s.getTimeSlot(0, 3, a, b, c, d));  // index 3 is past the 3-slot day
  CHECK(!s.getTimeSlot(7, 0, a, b, c, d));
}

// The shared local -> UTC conversion (dacarson/NavienManager#11).
static void test_local_window_to_utc() {
  int dow, start, end;
  // Pacific (UTC = local + 480): Saturday 15:30-17:30 local crosses UTC midnight
  // into Sunday; the end is past 1440 and the caller splits it.
  SchedulerBase::localWindowToUtc(6, 15 * 60 + 30, 17 * 60 + 30, 480, dow, start, end);
  CHECK(dow == 6 && start == 23 * 60 + 30 && end == 1440 + 90);
  // Saturday 20:00-21:00 local is Sunday 04:00-05:00 UTC.
  SchedulerBase::localWindowToUtc(6, 20 * 60, 21 * 60, 480, dow, start, end);
  CHECK(dow == 0 && start == 4 * 60 && end == 5 * 60);
  // East of UTC (Tokyo, -540): Monday 06:00-08:00 local is Sunday 21:00-23:00 UTC.
  SchedulerBase::localWindowToUtc(1, 6 * 60, 8 * 60, -540, dow, start, end);
  CHECK(dow == 0 && start == 21 * 60 && end == 23 * 60);
  // A local window written as ending at 00:00 means "until midnight".
  SchedulerBase::localWindowToUtc(1, 22 * 60, 0, 420, dow, start, end);
  CHECK(dow == 2 && start == 5 * 60 && end == 7 * 60);
  SchedulerBase::localWindowToUtc(1, 22 * 60, 0, 0, dow, start, end);
  CHECK(dow == 1 && start == 22 * 60 && end == 1440);
}

// A window split at UTC midnight fires continuously across the day boundary
// even when its tail is stored after the next day's own slots: the scan takes
// the earliest edge of each UTC day, not the first slot in table order. The
// Saturday -> Sunday wrap is the case from the review of #13.
static void test_fire_slots_cross_utc_midnight_out_of_order() {
  reset();
  learner_host::now = utc(2026, 10, 17, 22, 0);  // Saturday 22:00 UTC
  TestScheduler s;
  s.begin();
  CHECK(s.install_local(0, 6 * 60, 8 * 60, 480));           // Sunday 06:00-08:00 local = Sun 14:00-16:00 UTC, appended first
  CHECK(s.install_local(6, 15 * 60 + 30, 17 * 60 + 30, 480));  // Saturday 15:30-17:30 local = Sat 23:30 -> Sun 01:30 UTC
  CHECK(s.getUtcFireSlotCount() == 3);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::InActive);
  time_t next = 0;
  CHECK(s.getNextState(&next) == SchedulerBase::Active);
  CHECK(next == utc(2026, 10, 17, 23, 30));
  learner_host::now = utc(2026, 10, 17, 23, 30);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::Active);
  learner_host::now = utc(2026, 10, 18, 0, 0);  // Sunday 00:00 UTC: the tail, stored last
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::Active);
  CHECK(s.getNextState(&next) == SchedulerBase::InActive);
  CHECK(next == utc(2026, 10, 18, 1, 30));  // not Sunday's own 14:00 slot
  learner_host::now = utc(2026, 10, 18, 1, 29);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::Active);
  learner_host::now = utc(2026, 10, 18, 1, 30);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::InActive);
  // The earlier-UTC window of a later local day (Saturday 20:00 local = Sunday
  // 04:00 UTC, appended after Sunday's 14:00 slot) is found too.
  CHECK(s.install_local(6, 20 * 60, 21 * 60, 480));
  s.reevaluate();
  CHECK(s.getNextState(&next) == SchedulerBase::Active);
  CHECK(next == utc(2026, 10, 18, 4, 0));
  learner_host::now = utc(2026, 10, 18, 4, 0);
  s.loop();
  CHECK(s.getCurrentState() == SchedulerBase::Active);
}

// Capacity: 21 local windows, all split, need 42 slots; a window that needs
// two slots is appended whole or not at all.
static void test_fire_slot_capacity_and_atomic_append() {
  reset();
  learner_host::now = utc(2026, 10, 12, 10, 0);
  CHECK(SchedulerBase::MAX_UTC_FIRE_SLOTS == 42);
  TestScheduler t;
  t.begin();
  for (int i = 0; i < 41; i++) CHECK(t.try_install(i % 7, (i / 7) * 60, (i / 7) * 60 + 30));
  CHECK(t.getUtcFireSlotCount() == 41);
  CHECK(!t.install_local(6, 15 * 60 + 30, 17 * 60 + 30, 480));  // needs 2, 1 free: nothing appended
  CHECK(t.getUtcFireSlotCount() == 41);
  CHECK(t.install_local(6, 20 * 60, 21 * 60, 480));  // needs 1
  CHECK(t.getUtcFireSlotCount() == 42);
  CHECK(!t.try_install(0, 23 * 60, 23 * 60 + 30));
}

// Simulation: random tables, a week stepped minute by minute, the scheduler's
// state after every loop() compared with a brute-force "is any slot active
// now". A wrong next-change time shows up as a state that lags the oracle,
// whatever the table's shape (the class of bug behind #13's review).
static void test_week_simulation_matches_oracle() {
  uint32_t seed = 20261021u;
  auto rnd = [&](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % n;
  };
  for (int trial = 0; trial < 8; trial++) {
    reset();
    learner_host::now = utc(2026, 10, 11, 0, 0);  // Sunday 00:00 UTC
    TestScheduler s;
    s.begin();
    int windows = 1 + rnd(21);
    for (int i = 0; i < windows; i++) {
      int local_dow = rnd(7), start = rnd(23 * 60), len = 10 + rnd(5 * 60);
      int end = start + len > 1440 ? 1440 : start + len;
      int offset = (trial % 3 == 0) ? 480 : (trial % 3 == 1) ? -540 : 0;
      s.install_local(local_dow, start, end, offset);  // may overlap; may refuse when full
    }
    s.loop();
    int mismatches = 0;
    for (int minute = 0; minute < 8 * 1440 && mismatches < 5; minute++) {
      learner_host::now += 60;
      s.loop();
      struct tm t = {};
      gmtime_r(&learner_host::now, &t);
      bool oracle = s.active_at(t.tm_wday, t.tm_hour, t.tm_min);
      bool got = s.getCurrentState() == SchedulerBase::Active;
      if (got != oracle) {
        mismatches++;
        fprintf(stderr, "  trial %d: minute %d (%s %02d:%02d UTC) scheduler %s, oracle %s\n", trial, minute,
                (const char *[]){"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}[t.tm_wday], t.tm_hour, t.tm_min,
                got ? "Active" : "Inactive", oracle ? "Active" : "Inactive");
      }
    }
    CHECK(mismatches == 0);
  }
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "-v") == 0) learner_host::verbose = true;

  struct {
    const char *name;
    void (*fn)();
  } tests[] = {
      {"bucket_store_persists_through_storage", test_bucket_store_persists_through_storage},
      {"demand_event_lands_in_bucket", test_demand_event_lands_in_bucket},
      {"short_tap_with_recirc_is_discarded", test_short_tap_with_recirc_is_discarded},
      {"recompute_hands_off_local_slots", test_recompute_hands_off_local_slots},
      {"measured_window_persists", test_measured_window_persists},
      {"storage_failure_disables_learner_cleanly", test_storage_failure_disables_learner_cleanly},
      {"scheduler_runs_without_nvs_or_sntp", test_scheduler_runs_without_nvs_or_sntp},
      {"scheduler_waits_for_clock", test_scheduler_waits_for_clock},
      {"get_time_slot_bounds", test_get_time_slot_bounds},
      {"local_window_to_utc", test_local_window_to_utc},
      {"fire_slots_cross_utc_midnight_out_of_order", test_fire_slots_cross_utc_midnight_out_of_order},
      {"fire_slot_capacity_and_atomic_append", test_fire_slot_capacity_and_atomic_append},
      {"week_simulation_matches_oracle", test_week_simulation_matches_oracle},
  };
  for (auto &t : tests) {
    int before = g_failures;
    t.fn();
    printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
  }
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
