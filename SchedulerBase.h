/*
Copyright (c) 2024-2025 David Carson (dacarson)

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

#include <math.h>
#include <stdint.h>
#include <time.h>
#include "LearnerPlatform.h"

#if LEARNER_WITH_NVS_SCHEDULE
#include <Arduino.h>
#include "nvs.h"
#endif

class SchedulerBase {
  public:
    SchedulerBase();
    virtual ~SchedulerBase() {}

    virtual int begin();
    virtual void loop();
    bool isReady() { return isInitialized; }

#if LEARNER_WITH_NVS_SCHEDULE
    // Timezone management (display only; firing is UTC)
    bool setTz(String timezone);
    String getTz() { return tz; }
    void eraseTz();
#endif

    bool isActive();
    bool vacationActive();

    void setVacationState(bool active);

    enum State {
        Unknown,
        Active,
        InActive,
        Vacation,
        Override
    };

    // Getter for the current state
    State getCurrentState() const { return currentState; }
    State getNextState(time_t *nextStateTime = 0) const;

    void activateOverride(int durationMinutes = 5);
    bool isOverrideActive() const { return overrideActive; }
    time_t getOverrideEndTime() const { return overrideEndTime; }
    time_t getNextStateChangeTime() const { return nextStateChangeTime; }
    bool getTimeSlot(int day, int slotIndex, uint8_t &startHour, uint8_t &startMinute, uint8_t &endHour, uint8_t &endMinute) const;

    // Up to 21 UTC fire windows (3 per local day). Used for firing when the learner
    // (or POST /schedule) supplies more windows than fit in weekSchedule[7][3].
    static constexpr int MAX_UTC_FIRE_SLOTS = 21;
    int  getUtcFireSlotCount() const { return _utcFireSlotCount; }
    bool getUtcFireSlot(int index, uint8_t &utcDow,
                        uint8_t &startHour, uint8_t &startMinute,
                        uint8_t &endHour, uint8_t &endMinute,
                        float *scoreOut = nullptr) const;
    void clearUtcFireSlots() { _utcFireSlotCount = 0; }

protected:
    // Structure to store schedule for a day
    struct TimeSlot {
      uint8_t startHour;
      uint8_t startMinute;
      uint8_t endHour;
      uint8_t endMinute;
    };

    struct DaySchedule {
      // 0xFF for times indicates unused slots
      TimeSlot slots[3];
    };

    struct UtcFireSlot {
      uint8_t utcDow;       // 0=Sun .. 6=Sat (tm_wday)
      uint8_t startHour;
      uint8_t startMinute;
      uint8_t endHour;
      uint8_t endMinute;
      float   score;        // NAN if unknown
    };

  // Derived class can override to get
  // state changes.
  virtual void stateChange(State newState) {}

  // Persistence and platform hooks. The defaults (LEARNER_WITH_NVS_SCHEDULE)
  // are the sketch's: the week schedule and vacation times in the SCHEDULER
  // NVS namespace, TZ restore, SNTP start, and a WiFi + SNTP wait before the
  // scheduler runs. Under a custom platform the defaults are inert (nothing
  // saved, nothing loaded, time synced once LEARNER_NOW() is plausible) and
  // the derived class supplies its own.
  virtual bool saveScheduleToStorage();
  virtual bool loadScheduleFromStorage();
  virtual void loadVacationFromStorage();
  virtual bool platformBegin();
  virtual bool timeSynced();

  virtual void initDefault();

  bool isTimeWithinSlot(int currentHour, int currentMinute, TimeSlot slot) const;
  bool appendUtcFireSlot(uint8_t utcDow,
                         uint8_t startHour, uint8_t startMinute,
                         uint8_t endHour, uint8_t endMinute,
                         float score = NAN);
  State getNextStateFromFireSlots(time_t *nextStateTime) const;
  bool  isActiveOnFireSlots(int utcDow, int hour, int minute) const;

  DaySchedule weekSchedule[7]; // 0 = Sunday, 6 = Saturday (≤3 slots/UTC day, Eve wire)
  UtcFireSlot _utcFireSlots[MAX_UTC_FIRE_SLOTS];
  int         _utcFireSlotCount = 0;
  time_t startVacationTime;
  time_t endVacationTime;

#if LEARNER_WITH_NVS_SCHEDULE
  String tz;
#endif
  bool sntpSyncDone;
  State currentState;
  bool isInitialized;

private:
  time_t overrideStartTime = 0;  // When override began
  time_t overrideEndTime = 0;    // When override should expire
  bool overrideActive = false;   // Is override running?
  time_t nextStateChangeTime = 0; // When the next state change should occur

#if LEARNER_WITH_NVS_SCHEDULE
  nvs_handle_t nvsStorageHandle;
#endif

protected:
  // Initialize the current state based on current time and schedule
  void initializeCurrentState();
};
