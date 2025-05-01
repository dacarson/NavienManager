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

#include <Arduino.h>
#include <time.h>
#include "nvs.h"

class SchedulerBase {
  public:
    SchedulerBase();
    virtual ~SchedulerBase() {}

    virtual int begin();
    virtual void loop();
    bool isReady() { return isInitialized; }

    // Timezone management
    bool setTz(String timezone);
    String getTz() { return tz; }
    void eraseTz();

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
      TimeSlot slots[4];
    };

  // Derived class can override to get
  // state changes.
  virtual void stateChange(State newState) {}

  virtual bool saveScheduleToStorage();
  virtual bool loadScheduleFromStorage();

  virtual void initDefault();

  bool isTimeWithinSlot(int currentHour, int currentMinute, TimeSlot slot) const;

  DaySchedule weekSchedule[7]; // 0 = Sunday, 6 = Saturday
  time_t startVacationTime;
  time_t endVacationTime;

  String tz;
  bool sntpSyncDone;
  State currentState;
  bool isInitialized;

private:
  time_t overrideStartTime = 0;  // When override began
  time_t overrideEndTime = 0;    // When override should expire
  bool overrideActive = false;   // Is override running?
  time_t nextStateChangeTime = 0; // When the next state change should occur

  nvs_handle_t nvsStorageHandle;

protected:
  // Initialize the current state based on current time and schedule
  void initializeCurrentState();
};
