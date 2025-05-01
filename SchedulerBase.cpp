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

#include "SchedulerBase.h"
#include "esp_netif_sntp.h"
#include <WiFi.h>

#define NTP_SERVER "pool.ntp.org"

SchedulerBase::SchedulerBase()
: sntpSyncDone(false), currentState(Unknown),
startVacationTime(0), endVacationTime(0),
isInitialized(false) {
}

void SchedulerBase::initDefault() {
  memset(weekSchedule, 0xFF, sizeof(weekSchedule));
  for (int i = 0; i < 7; ++i) {
    weekSchedule[i].slots[0].startHour = 7;
    weekSchedule[i].slots[0].startMinute = 0;
    weekSchedule[i].slots[0].endHour = 9;
    weekSchedule[i].slots[0].endMinute = 0;
    
    weekSchedule[i].slots[1].startHour = 18;
    weekSchedule[i].slots[1].startMinute = 0;
    weekSchedule[i].slots[1].endHour = 21;
    weekSchedule[i].slots[1].endMinute = 0;
  }
}

bool SchedulerBase::saveScheduleToStorage() {
  
    // Don't write unchanged data
  DaySchedule weekScheduleEEPROM[7];
  size_t len = sizeof(weekScheduleEEPROM);
  esp_err_t status = nvs_get_blob(nvsStorageHandle, "weekSchedule", &weekScheduleEEPROM, &len);
  if (status) {
    Serial.print(F("Not critical, failed to load existing schedule for save comparison: "));
    Serial.println(esp_err_to_name(status));
  }
  
  if (!status && memcmp(weekSchedule, weekScheduleEEPROM, sizeof(weekSchedule)) == 0) {
    Serial.println(F("Schedule unchanged, not saving to NVS"));
    return true;
  }
  
  status = nvs_set_blob(nvsStorageHandle, "weekSchedule", &weekSchedule, sizeof(weekSchedule));
  if (status) {
    Serial.print(F("Failed to save schedule to NVS "));
    Serial.println(esp_err_to_name(status));
    return false;
  }
  Serial.println(F("Schedule updated to NVS."));
  nvs_commit(nvsStorageHandle);
  return true;
}

bool SchedulerBase::loadScheduleFromStorage() {
  size_t len = sizeof(weekSchedule);
  esp_err_t status = nvs_get_blob(nvsStorageHandle, "weekSchedule", &weekSchedule, &len);
  if (status) {
    Serial.print(F("Opening NVS "));
    Serial.println(esp_err_to_name(status));
    return false;
  }
  
    // Validate what we loaded.
  for (int i = 0; i < 7; i++) {
    for (int x = 0; x < 4; x++) {
      if (weekSchedule[i].slots[x].startHour != 0xFF && (weekSchedule[i].slots[x].startHour < 0 || weekSchedule[i].slots[x].startHour > 23)) {
        Serial.println(F("Schedule corrupt in NVS."));
        return false;
      }
      if (weekSchedule[i].slots[x].startMinute != 0xFF && (weekSchedule[i].slots[x].startMinute < 0 || weekSchedule[i].slots[x].startMinute > 59)) {
        Serial.println(F("Schedule corrupt in NVS."));
        return false;
      }
      if (weekSchedule[i].slots[x].endHour != 0xFF && (weekSchedule[i].slots[x].endHour < 0 || weekSchedule[i].slots[x].endHour > 23)) {
        Serial.println(F("Schedule corrupt in NVS."));
        return false;
      }
      if (weekSchedule[i].slots[x].endMinute != 0xFF && (weekSchedule[i].slots[x].endMinute < 0 || weekSchedule[i].slots[x].endMinute > 59)) {
        Serial.println(F("Schedule corrupt in NVS."));
        return false;
      }
    }
  }
  
  return true;
}

void SchedulerBase::setVacationState(bool active) {
  endVacationTime = 0;
  
  if (active) {
    startVacationTime = time(nullptr);
  } else {
    startVacationTime = 0;
  }
  
  // Recalculate next state change since vacation state changed
  if (isInitialized) {
    getNextState(&nextStateChangeTime);
  }
}

void SchedulerBase::activateOverride(int durationMinutes) {
  time_t now = time(nullptr);
  overrideStartTime = now;
  overrideEndTime = now + (durationMinutes * 60); // Convert minutes to seconds
  overrideActive = true;
  
  // Calculate next state change (which will be when override expires)
  nextStateChangeTime = overrideEndTime;
  
  Serial.printf("Override activated! Scheduler forced ON for %d minutes.\n", durationMinutes);
}


SchedulerBase::State SchedulerBase::getNextState(time_t *nextStateTime) const {
  time_t nextTime;
  
    // Handle vacation state
  if (currentState == State::Vacation) {
      // If no end time set, stay in vacation indefinitely
    if (endVacationTime == 0) {
      if (nextStateTime) {
        *nextStateTime = 0;  // No next state change
      }
      return State::Vacation;
    }
    
    if (nextStateTime) {
      *nextStateTime = endVacationTime;
    }
    
      // Check if we'll be in an active slot when vacation ends
    struct tm *end_tm = localtime(&endVacationTime);
    int endHour = end_tm->tm_hour;
    int endMinute = end_tm->tm_min;
    int endDay = end_tm->tm_wday;
    
    for (int slot = 0; slot < 4 && weekSchedule[endDay].slots[slot].startHour != 0xFF; slot++) {
      if (isTimeWithinSlot(endHour, endMinute, weekSchedule[endDay].slots[slot])) {
        return State::Active;  // We'll transition from Vacation -> Active
      }
    }
    
    return State::InActive;  // We'll transition from Vacation -> InActive
  }
  
    // Find the next event
  time_t now = time(nullptr);
  struct tm *tm_struct = localtime(&now);
  
  int currentHour = tm_struct->tm_hour;
  int currentMinute = tm_struct->tm_min;
  int currentDay = tm_struct->tm_wday;
  
  int currentTimeInMinutes = currentHour * 60 + currentMinute;
  
  for (int dayoffset = 0; dayoffset < 7; dayoffset++) {
    int day = (currentDay + dayoffset) % 7;
    for (int slot = 0; slot < 4 && weekSchedule[day].slots[slot].startHour != 0xFF; slot++) {
      int startMin = dayoffset * 24 * 60 + weekSchedule[day].slots[slot].startHour * 60 + weekSchedule[day].slots[slot].startMinute;
      if (startMin > currentTimeInMinutes) {
          // Calculate start time of next item
        tm nextState_tm;
        nextState_tm.tm_year = tm_struct->tm_year;
        nextState_tm.tm_mon = tm_struct->tm_mon;
        nextState_tm.tm_mday = tm_struct->tm_mday + dayoffset;
        nextState_tm.tm_hour = weekSchedule[day].slots[slot].startHour;
        nextState_tm.tm_min = weekSchedule[day].slots[slot].startMinute;
        nextState_tm.tm_sec = 0;
        nextState_tm.tm_isdst = -1; // Use TZ to determine DST offsets.
        nextTime = mktime(&nextState_tm);
        if (startVacationTime && startVacationTime < nextTime) {
          if (nextStateTime) {
            *nextStateTime = startVacationTime;
          }
          return State::Vacation;
        }
        if (nextStateTime) {
          *nextStateTime = nextTime;
        }
        return State::Active;
      }
      int endMin = dayoffset * 24 * 60 + weekSchedule[day].slots[slot].endHour * 60 + weekSchedule[day].slots[slot].endMinute;
      if (endMin > currentTimeInMinutes) {
          // Calculate start time of next item
        tm nextState_tm;
        nextState_tm.tm_year = tm_struct->tm_year;
        nextState_tm.tm_mon = tm_struct->tm_mon;
        nextState_tm.tm_mday = tm_struct->tm_mday + dayoffset;
        nextState_tm.tm_hour = weekSchedule[day].slots[slot].endHour;
        nextState_tm.tm_min = weekSchedule[day].slots[slot].endMinute;
        nextState_tm.tm_sec = 0;
        nextState_tm.tm_isdst = -1; // Use TZ to determine DST offsets.
        nextTime = mktime(&nextState_tm);
        if (startVacationTime && startVacationTime < nextTime) {
          if (nextStateTime) {
            *nextStateTime = startVacationTime;
          }
          return State::Vacation;
        }
        if (nextStateTime) {
          *nextStateTime = nextTime;
        }
        return State::InActive;
      }
    }
  }
  
    // Found no timeslots or vacation slot, so we are in active indefinitely
  if (nextStateTime) {
    *nextStateTime = 0;
  }
  return State::InActive;
}

bool SchedulerBase::isTimeWithinSlot(int currentHour, int currentMinute, TimeSlot slot) const {
    // Convert times to minutes since midnight for comparison
  int currentTimeInMinutes = currentHour * 60 + currentMinute;
  int slotStartInMinutes = slot.startHour * 60 + slot.startMinute;
  int slotEndInMinutes = slot.endHour * 60 + slot.endMinute;
  
  return currentTimeInMinutes >= slotStartInMinutes && currentTimeInMinutes <= slotEndInMinutes;
}

bool SchedulerBase::setTz(String timezone) {
  // Reload the current timezone as it may have been updated elsewhere.
  size_t len = 64;
  char tzStr[64];
  esp_err_t status = nvs_get_str(nvsStorageHandle, "TZ", tzStr, &len);
  if (status) {
    tz = String();
  } else {
    tz = tzStr;
  }

  if (tz == timezone) {
    Serial.print(timezone);
    Serial.println(" Timezones are the same, not updating.");
    return true;
  } else {
    Serial.print("Updating timezone to ");
    Serial.println(timezone);
    tz = timezone;
    
    setenv("TZ", timezone.c_str(), 1);
    tzset();
    
    status = nvs_set_str(nvsStorageHandle, "TZ", tz.c_str());
    if (status) {
      Serial.printf("Failed to write TZ from NVS: %s.\n", esp_err_to_name(status));
      return false;
    }
    nvs_commit(nvsStorageHandle);
    return true;
  }
}

void SchedulerBase::eraseTz() {
    esp_err_t status = nvs_erase_key(nvsStorageHandle, "TZ");
    if (status == ESP_OK) {
        Serial.println("Time zone erased from NVS.");
    } else if (status == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("No stored time zone found.");
    } else {
        Serial.printf("Failed to erase key: %s\n", esp_err_to_name(status));
    }
    nvs_commit(nvsStorageHandle);  

    unsetenv("TZ");
    tzset();
    tz = String();
}

bool SchedulerBase::isActive() {
  return currentState == State::Active;
}

bool SchedulerBase::vacationActive() {
  return currentState == State::Vacation;
}

int SchedulerBase::begin() {
  
  esp_err_t status;
  status = nvs_open("SCHEDULER", NVS_READWRITE, &nvsStorageHandle);
  if (status) {
    Serial.printf("Failed to open NVS Storage: %s\n", esp_err_to_name(status));
    return false;
  }
  
  // If we failed to load the schedule from EEPROM, then
  // init to a default schedule.
  if (!loadScheduleFromStorage()) {
    Serial.println("No saved Schedule, loading default.");
    initDefault();
  }
  
  uint32_t storedTime = 0;
  status = nvs_get_u32(nvsStorageHandle, "startVacation", &storedTime);
  startVacationTime = (time_t)storedTime;
  if (status) {
    Serial.println("Failed to load vacation start time, defaulting to unset.");
    startVacationTime = 0;
  }
  
  status = nvs_get_u32(nvsStorageHandle, "endVacation", &storedTime);
  startVacationTime = (time_t)storedTime;
  if (status) {
    Serial.println("Failed to load vacation end time, defaulting to unset.");
    endVacationTime = 0;
  }
  
  size_t len = 64;
  char tzStr[64];
  status = nvs_get_str(nvsStorageHandle, "TZ", tzStr, &len);
  if (status) {
    Serial.printf("Failed to load TZ from NVS: %s.\n", esp_err_to_name(status));
    Serial.println("Schedules won't run until TZ set.");
  } else {
    tz = tzStr;
    Serial.print("Restoring saved TZ: ");
    Serial.println(tz);
    setenv("TZ", tz.c_str(), 1);
    tzset();
  }
  
  // Enable SNTP
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
  esp_netif_sntp_init(&config);  // DEFAULT_CONFIG is to start the server on init()
  sntpSyncDone = false;
  
  // Don't calculate next state change time here - it will be done in loop()
  // once SNTP sync is complete and TZ is set
  
  return true;
}

void SchedulerBase::initializeCurrentState() {
  time_t now = time(nullptr);
  
  State newState = State::InActive; // Default to inactive
  
  // First check if we're in override mode
  if (overrideActive && now < overrideEndTime) {
    newState = State::Override;
  }
  // Then check if we're in vacation mode
  else if (startVacationTime && now >= startVacationTime && (!endVacationTime || now < endVacationTime)) {
    newState = State::Vacation;
  }
  // Finally check if we're in an active time slot
  else {
    struct tm *tm_struct = localtime(&now);
    int currentHour = tm_struct->tm_hour;
    int currentMinute = tm_struct->tm_min;
    int currentDay = tm_struct->tm_wday;
    
    for (int i = 0; i < 4 && weekSchedule[currentDay].slots[i].startHour != 0xFF; i++) {
      if (isTimeWithinSlot(currentHour, currentMinute, weekSchedule[currentDay].slots[i])) {
        newState = State::Active;
        break;
      }
    }
  }
  
  // Set the new state and notify
  if (newState != currentState) {
    stateChange(newState);
    currentState = newState;
  }
  
  // Calculate the next state change time
  getNextState(&nextStateChangeTime);
}

void SchedulerBase::loop() {
  
  if (!sntpSyncDone && WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  esp_err_t value;
  if (!sntpSyncDone && (value = esp_netif_sntp_sync_wait(2000) == ESP_ERR_TIMEOUT)) {
    if (tz.length() > 0) {
      Serial.print("+");
    }
    return;
  }
  if (!sntpSyncDone) {
    Serial.printf("Sync %s\n", esp_err_to_name(value));
    Serial.println("SNTP Sync completed");
    sntpSyncDone = true;
    return;
  }
  
  // No timezone set, so we can't schedule.
  if (getenv("TZ") == 0) {
    isInitialized = false;
    return;
  }

  // If we just became initialized, determine our current state
  if (!isInitialized) {
    isInitialized = true;
    initializeCurrentState();
    return;
  }
  
  time_t now = time(nullptr);
  State newState = currentState; // Default to current state
  
  // Handle override expiration
  if (overrideActive && now >= overrideEndTime) {
    overrideActive = false;
    Serial.println("Override expired. Reverting to normal scheduling.");
    // Recalculate next state change since override expired
    getNextState(&nextStateChangeTime);
  }
  
  // If override is active, force the state and return
  if (overrideActive) {
    newState = State::Override;
    if (newState != currentState) {
      stateChange(newState);
      currentState = newState;
    }
    return;
  }
  
  // Check if it's time for a state change
  if (nextStateChangeTime > 0 && now >= nextStateChangeTime) {
    // Get the next state and its time
    newState = getNextState(&nextStateChangeTime);
    
    if (newState != currentState) {
      stateChange(newState);
      currentState = newState;
    }
  }
  
  // If we don't have a next state change time, calculate it
  if (nextStateChangeTime == 0) {
    getNextState(&nextStateChangeTime);
  }
}
