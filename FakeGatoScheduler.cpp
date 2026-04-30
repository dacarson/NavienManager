/*
 Copyright (c) 2025 David Carson (dacarson)
 
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


#define CUSTOM_CHAR_HEADER
#include "FakeGatoScheduler.h"
#include "NavienLearner.h"
#include "Navien.h"
#include "TimeUtils.h"
#include <ArduinoJson.h>
#include <climits>
#include <cmath>

extern Navien navienSerial;
extern NavienLearner *learner;

FakeGatoScheduler::FakeGatoScheduler()
: SchedulerBase() {

  programData = new Characteristic::ProgramData();
  
  size_t len;
  nvs_open("SAVED_DATA",NVS_READWRITE,&savedData);       // open a new namespace called SAVED_DATA in the NVS
  if(!nvs_get_blob(savedData,"PROG_SEND_DATA",NULL,&len)) {        // if PROG_SEND_DATA data found
    nvs_get_blob(savedData,"PROG_SEND_DATA",&prog_send_data,&len);       // retrieve data
    loadSlotScoresFromStorage();
    
    WEBLOG("SCHEDULER Loaded Program State");
      // Restore scheduleActive from its own key.
      // SCHED_ACTIVE is the authoritative persisted source; fall back to
      // prog_send_data.schedule_state.schedule_on for backwards compatibility.
    uint8_t savedScheduleActive = 0;
    if (nvs_get_u8(savedData, "SCHED_ACTIVE", &savedScheduleActive) == ESP_OK) {
      scheduleActive = (savedScheduleActive != 0);
    } else if (prog_send_data.schedule_state.schedule_on) {
      scheduleActive = true;  // backwards-compat fallback for old firmware
    }
    setVacationState(false);
    if (prog_send_data.vacation.enabled)
      setVacationState(true);
  } else {
    WEBLOG("SCHEDULER Initializing Program State");
    prog_send_data.temp_offset.offset = 0;
    prog_send_data.install_status.status = 0xC0;
    prog_send_data.vacation.enabled = 0x00;
    setVacationState(false);
    prog_send_data.schedule_state.schedule_on = 0x00;
    scheduleActive = false;
      // clear every day of the weekly schedule
    memset(&prog_send_data.weekSchedule.day, 0xFF, sizeof(prog_send_data.weekSchedule.day));
      // clear today's schedule
    memset(&prog_send_data.currentSchedule.current, 0xFF, sizeof(prog_send_data.currentSchedule.current));
    resetSlotScores();
  }
  
  updateSchedulerWeekSchedule();
  refreshProgramData = true;
}

void FakeGatoScheduler::resetSlotScores() {
  for (int d = 0; d < 7; d++) {
    for (int s = 0; s < SLOT_SCORE_STORAGE_SLOTS; s++) {
      _slotScoreUtc[d][s] = SLOT_SCORE_UNKNOWN;
    }
  }
}

void FakeGatoScheduler::loadSlotScoresFromStorage() {
  size_t len = sizeof(_slotScoreUtc);
  if (nvs_get_blob(savedData, "SLOT_SCORE_V1", &_slotScoreUtc, &len) != ESP_OK || len != sizeof(_slotScoreUtc)) {
    resetSlotScores();
  }
}

void FakeGatoScheduler::saveSlotScoresToStorage() {
  nvs_set_blob(savedData, "SLOT_SCORE_V1", &_slotScoreUtc, sizeof(_slotScoreUtc));
}

void FakeGatoScheduler::setEnabled(bool enable) {
  scheduleActive = enable;
  nvs_set_u8(savedData, "SCHED_ACTIVE", enable ? 1 : 0);
  nvs_set_blob(savedData, "PROG_SEND_DATA", &prog_send_data, sizeof(prog_send_data));
  saveSlotScoresToStorage();
  nvs_commit(savedData);
  refreshProgramData = true;
  if (enable && isInitialized) initializeCurrentState();
}

bool FakeGatoScheduler::getSlotScoreUtc(int day, int slotIndex, float &scoreOut) const {
  if (day < 0 || day > 6 || slotIndex < 0 || slotIndex >= ACTIVE_SLOT_LIMIT) return false;
  uint8_t sh, sm, eh, em;
  if (!getTimeSlot(day, slotIndex, sh, sm, eh, em)) return false;
  int eveDay = (day + 6) % 7;  // SchedulerBase Sunday-first -> Eve Monday-first
  float score = _slotScoreUtc[eveDay][slotIndex];
  if (isnan(score)) return false;
  scoreOut = score;
  return true;
}

String FakeGatoScheduler::getSchedulerState(int state) {
    switch (state) {
        case 0: return "Unknown";
        case 1: return "Active";
        case 2: return "Inactive";
        case 3: return "Vacation";
        case 4: return "Override";
        default: return "Invalid";
    }
}

void FakeGatoScheduler::stateChange(State newState){
  time_t nextStateTime;
  this->getNextState(&nextStateTime);
  struct tm *tm_struct = localtime(&nextStateTime);
        Serial.printf("Next event scheduled for: %02d:%02d %02d/%02d/%04d\n",
              tm_struct->tm_hour, tm_struct->tm_min,
              tm_struct->tm_mon + 1, tm_struct->tm_mday,  // tm_mon is 0-based
              tm_struct->tm_year + 1900);               // tm_year is years since 1900

  
  // Track Override states even if the scheduler is not active
  if (!scheduleActive && 
    (newState != State::Override && currentState != State::Override)) {
    WEBLOG("Ignoring state change, scheduler not active.");
    return;
  }
  // If the schedule is not running and we are leaving Override state
  // then we need to go InActive
  if (!scheduleActive && currentState == State::Override) {
    newState = State::InActive;
  }
  
    // Should not need to update the targetState of the Thermostat
    // as that will update when the state changes in the Navien
  switch (newState) {
    case State::Override:
      // Fall through
    case State::Active:
      WEBLOG("SCHEDULER going Active %s", newState == State::Override ? "- Override" : "" );
      if (!navienSerial.currentState()->water[0].system_power)
        navienSerial.power(true);
      if (!navienSerial.currentState()->water[0].recirculation_active)
        if (navienSerial.recirculation(true) == -1)
          WEBLOG("Failed to enable Recirculation.");
        // ignore setpoints as they only go to 30 degC
        //navienSerial.setTemp(0.5 * prog_send_data.temperatures.comfortScheduleTemp);
      break;
      
    case State::InActive:
      WEBLOG("SCHEDULER going Inactive");
      if (!navienSerial.currentState()->water[0].system_power)
        navienSerial.power(true);
      if (navienSerial.recirculation(false) == -1)
        WEBLOG("Failed to disable Recirculation.");
        // ignore setpoints as they only go to 30 degC
        //navienSerial.setTemp(0.5 * prog_send_data.temperatures.comfortScheduleTemp);
      break;
      
    case State::Vacation:
      WEBLOG("SCHEDULER going Vacation");
      navienSerial.recirculation(false);
      if (navienSerial.power(false) == -1)
        WEBLOG("Failed to turn power off");
      break;
  }
  
}

void FakeGatoScheduler::addMilliseconds(PROG_CMD_CURRENT_TIME *timeStruct, uint32_t milliseconds) {
    // Eve wire time — intentionally local-time (sent back to Eve for display only).
    // Convert milliseconds to seconds and remaining milliseconds
  uint32_t secondsToAdd = milliseconds / 1000;
  milliseconds %= 1000;  // Remaining milliseconds (not stored in struct)
  
    // Convert structure to time_t for easier manipulation
  struct tm t = { 0 };
  t.tm_year = timeStruct->year + 100;  // tm_year is years since 1900
  t.tm_mon = timeStruct->month - 1;    // tm_mon is 0-based
  t.tm_mday = timeStruct->day;
  t.tm_hour = timeStruct->hours;
  t.tm_min = timeStruct->minutes;
  t.tm_sec = 0;  // Assuming no seconds in the structure
  
    // Add seconds
  time_t rawTime = mktime(&t);  // Convert struct to time_t
  rawTime += secondsToAdd;      // Add the seconds
  
    // Convert back to structure
  struct tm *updatedTime = localtime(&rawTime);
  timeStruct->year = updatedTime->tm_year - 100;
  timeStruct->month = updatedTime->tm_mon + 1;
  timeStruct->day = updatedTime->tm_mday;
  timeStruct->hours = updatedTime->tm_hour;
  timeStruct->minutes = updatedTime->tm_min;
}

void FakeGatoScheduler::guessTimeZone(PROG_CMD_CURRENT_TIME *eveLocalTime) {
  // TZ is now display-only; wrong TZ corrupts Eve display but not schedule firing.

    struct tm eveTimeInfo = {0};  // Initialize to zero

    eveTimeInfo.tm_year = eveLocalTime->year + 100;  // tm_year is years since 1900
    eveTimeInfo.tm_mon  = eveLocalTime->month - 1;   // tm_mon is 0-based (Jan = 0)
    eveTimeInfo.tm_mday = eveLocalTime->day;
    eveTimeInfo.tm_hour = eveLocalTime->hours;
    eveTimeInfo.tm_min  = eveLocalTime->minutes;
    eveTimeInfo.tm_sec  = 0;  // Assuming seconds are zero

    time_t localTime = proper_timegm(&eveTimeInfo);  // treat Eve local as UTC to get pseudo-epoch
    time_t currentTime = time(nullptr);  // Get the current system time
    struct tm *deviceLocalTime = localtime(&currentTime); // Convert to local time struct

    // Make sure the timezone is set AND the hours are the same
    // If hours are different, then we need to update the TZ
    if (getenv("TZ") && eveTimeInfo.tm_hour == deviceLocalTime->tm_hour) {
      Serial.print("Timezone is correct. ");
      Serial.println(getenv("TZ"));
      return; // TZ already set.
    }

    double timeDiffSeconds = difftime(currentTime, localTime);
    int timeDiffHours = std::round(timeDiffSeconds / 3600.0);

    char tzString[10];
    snprintf(tzString, sizeof(tzString), "UTC%+d", timeDiffHours);

    if (setTz(String(tzString))) {
      WEBLOG("Set estimated TZ %s\n", tzString);
    } else {
      WEBLOG("Failed to set new TZ %s\n", tzString);
    }
}

void FakeGatoScheduler::updateSchedulerWeekSchedule() {
    // Note Eve schedules are Monday - Sunday
    // SchedulerBase are Sunday - Saturday

    // Clear all slots to 0xFF (unused) before filling valid ones, so
    // uninitialized or stale data from previous loads cannot bleed through.
  memset(weekSchedule, 0xFF, sizeof(weekSchedule));

  for (int day = 0; day < 7; day++) { // Monday - Sunday
    CMD_DAY_SCHEDULE *daySchedule = &(prog_send_data.weekSchedule.day[day]);
    for (int i = 0; i < 3 && daySchedule->slot[i].offset_start != 0xFF; i++) {
      weekSchedule[(day + 1) % 7].slots[i].startHour = (uint8_t)(daySchedule->slot[i].offset_start / 6);
      weekSchedule[(day + 1) % 7].slots[i].startMinute = (uint8_t)(daySchedule->slot[i].offset_start % 6) * 10;
      weekSchedule[(day + 1) % 7].slots[i].endHour = (uint8_t)(daySchedule->slot[i].offset_end / 6);
      weekSchedule[(day + 1) % 7].slots[i].endMinute = (uint8_t)(daySchedule->slot[i].offset_end % 6) * 10;
    }
  }
  
  // Recalculate current state after schedule update
  if (isInitialized) {
    initializeCurrentState();
  }
}

void FakeGatoScheduler::parseProgramData(uint8_t *data, int len) {
  int byte_offset = 0;
  bool storeData = false;
  
  while (byte_offset < len) {
    switch (data[byte_offset]) {
      case BEGIN_BLOCK:
        byte_offset++;
        break;
        
      case END_BLOCK:
        byte_offset++;
          // This is the end of the data so drop everything after this
        byte_offset = len;
        break;
        
      case VALVE_PROTECTION:
      {
        PROG_CMD_VALVE_PROTECT *valve_prot = (PROG_CMD_VALVE_PROTECT *)(&data[byte_offset]);
        Serial.printf("Valve Protection ");
        printData(&data[byte_offset], sizeof(PROG_CMD_VALVE_PROTECT));
        byte_offset += sizeof(PROG_CMD_VALVE_PROTECT);
        break;
      }
        
      case TEMPERATURE_OFFSET:
      {
        PROG_CMD_TEMPERATURE_OFFSET *temp_offset = (PROG_CMD_TEMPERATURE_OFFSET *)(&data[byte_offset]);
        temperature_offset = temp_offset->offset;
        Serial.printf("Temperature Offset: %0.1f C\n", (float)((int8_t)(temp_offset->offset)) / 10.0);
        memcpy(&prog_send_data.temp_offset, temp_offset, sizeof(PROG_CMD_TEMPERATURE_OFFSET));
        byte_offset += sizeof(PROG_CMD_TEMPERATURE_OFFSET);
        storeData = true;
        break;
      }
        
      case SCHEDULE_STATE:
      {
        PROG_CMD_SCHEDULE_STATE *schedule_state = (PROG_CMD_SCHEDULE_STATE *)(&data[byte_offset]);
        // Update the current state to what the user wants.
        // We report schedule_on=1 only during Active/Override, and 0 during Inactive.
        // Eve writes back whatever it last read, so a schedule_on=0 arriving while we are
        // Inactive is just Eve echoing our own 0 — not a genuine user disable request.
        // A genuine disable can only arrive when Eve was showing 1, i.e. Active/Override.
        if (schedule_state->schedule_on) {
          scheduleActive = true;
          Serial.println("Schedule: On");
          nvs_set_u8(savedData, "SCHED_ACTIVE", 1);
          if (isInitialized) initializeCurrentState();
        } else if (currentState == SchedulerBase::Active || currentState == SchedulerBase::Override) {
          scheduleActive = false;
          Serial.println("Schedule: Off");
          nvs_set_u8(savedData, "SCHED_ACTIVE", 0);
        } else {
          Serial.println("Schedule: Off ignored (Inactive write-back from Eve)");
        }
        // Report 1 only when a slot is currently active, so Eve doesn't try to change
        // the set point during Inactive periods between slots.
        schedule_state->schedule_on = (scheduleActive && (currentState == SchedulerBase::Active || currentState == SchedulerBase::Override)) ? 1 : 0;
        memcpy(&prog_send_data.schedule_state, schedule_state, sizeof(PROG_CMD_SCHEDULE_STATE));
        byte_offset += sizeof(PROG_CMD_SCHEDULE_STATE);
        storeData = true;
        break;
      }
      
      case INSTALLED_STATUS: {
        PROG_DATA_INSTALLED_STATUS *install_status = (PROG_DATA_INSTALLED_STATUS *)(&data[byte_offset]);
        Serial.print("Install status: ");
        Serial.println(install_status->status);
        // Don't memcpy, keep the default of 0xC0 - installed
        byte_offset += sizeof(PROG_DATA_INSTALLED_STATUS);
        break;
      }
      
      case UNKNOWN_BLOCK: {
        PROG_DATA_UNKNOWN_BLOCK *unknown_block = (PROG_DATA_UNKNOWN_BLOCK *)(&data[byte_offset]);
        Serial.print("Unknown block: ");
        Serial.print(unknown_block->unknown_01);
        Serial.print(" ");
        Serial.println(unknown_block->unknown_02);
        // Don't memcpy, keep the default of 0xC0 - installed
        byte_offset += sizeof(PROG_DATA_UNKNOWN_BLOCK);
        break;
      }
        
      case VACATION_MODE:
      {
        PROG_CMD_VACATION_MODE *vacationMode = (PROG_CMD_VACATION_MODE *)(&data[byte_offset]);
        Serial.printf("Vacation Mode: ");
        if (vacationMode->enabled) {
          Serial.printf(" On, Set Point %0.1f C\n", (0.5 * vacationMode->away_temp));
          setVacationState(true);
        } else {
          Serial.println(" Off");
          setVacationState(false);
        }
        memcpy(&prog_send_data.vacation, vacationMode, sizeof(PROG_CMD_VACATION_MODE));
        byte_offset += sizeof(PROG_CMD_VACATION_MODE);
        storeData = true;
        break;
      }
        
      case CURRENT_SCHEDULE:
      {
        PROG_CMD_CURRENT_SCHEDULE *currentSchedule = (PROG_CMD_CURRENT_SCHEDULE *)(&data[byte_offset]);
        Serial.printf("Current Schedule today %d ", currentScheduleDay);
        printDaySchedule(&currentSchedule->current);
          //only update if we don't know what day it is
        if (currentScheduleDay < 0)
          memcpy(&prog_send_data.currentSchedule, currentSchedule, sizeof(PROG_CMD_CURRENT_SCHEDULE));
        byte_offset += sizeof(PROG_CMD_CURRENT_SCHEDULE);
        storeData = true;
        break;
      }
        
      case TEMPERATURES:
      {
        PROG_CMD_TEMPERATURES *temperatures = (PROG_CMD_TEMPERATURES *)(&data[byte_offset]);
        Serial.printf("Default Temp: %0.1f C\n", (0.5 * temperatures->defaultTemp));
        Serial.printf("Economy Temp: %0.1f C\n", (0.5 * temperatures->economyScheduleTemp));
        Serial.printf("Comfort Temp: %0.1f C\n", (0.5 * temperatures->comfortScheduleTemp));
        
        // Override the default and comfort temps with the device set_point because the
        // Eve app only allows up to 30degC/86degF
        prog_send_data.temperatures.header = TEMPERATURES;
        prog_send_data.temperatures.unknown = 0x00;
        prog_send_data.temperatures.defaultTemp = navienSerial.currentState()->gas.set_temp * 2;
        prog_send_data.temperatures.comfortScheduleTemp = navienSerial.currentState()->gas.set_temp * 2;
        prog_send_data.temperatures.economyScheduleTemp = Navien::TEMPERATURE_MIN;
        byte_offset += sizeof(PROG_CMD_TEMPERATURES);
        storeData = true;
        break;
      }
      
      case OPEN_WINDOW: {
        PROG_DATA_OPEN_WINDOW *open_window = (PROG_DATA_OPEN_WINDOW *)(&data[byte_offset]);
        Serial.print("Open window: ");
        Serial.print(open_window->unknown_01);
        Serial.print(" ");
        Serial.print(open_window->unknown_02);
        Serial.print(" ");
        Serial.println(open_window->unknown_03);
        // Should not be sent to the device. But if it does, ignore it.
        byte_offset += sizeof(PROG_DATA_OPEN_WINDOW);
        break;
      }      
        
      case WEEK_SCHEDULE:
      {
        PROG_CMD_WEEK_SCHEDULE *weekSchedule = (PROG_CMD_WEEK_SCHEDULE *)(&data[byte_offset]);
        Serial.println("Week Schedule: ");
        for (int day = 0; day < 7; day++) {
          Serial.printf("Schedule Day %d ", day);
          printDaySchedule(&weekSchedule->day[day]);
        }
        Serial.println("");
        // Compute best available UTC offset for conversion.
        // Use _lastKnownUtcOffsetMin if already set this session; otherwise
        // recompute inline from the saved currentTime so a lone WEEK_SCHEDULE
        // packet (or one arriving before CURRENT_TIME) still converts correctly.
        {
          int offsetToUse = _lastKnownUtcOffsetMin;
          if (prog_send_data.currentTime.year != 0 && time(nullptr) > 1700000000L) {
            struct tm eveUTC = {0};
            eveUTC.tm_year = prog_send_data.currentTime.year + 100;
            eveUTC.tm_mon  = prog_send_data.currentTime.month - 1;
            eveUTC.tm_mday = prog_send_data.currentTime.day;
            eveUTC.tm_hour = prog_send_data.currentTime.hours;
            eveUTC.tm_min  = prog_send_data.currentTime.minutes;
            time_t evePseudo = proper_timegm(&eveUTC);
            time_t sysUTC = time(nullptr);
            int computed = (int)(difftime(sysUTC, evePseudo) / 60.0 + 0.5);
            if (computed >= -720 && computed <= 720) {
              offsetToUse = computed;
              _utcOffsetKnown = true;
            }
          }
          if (!_utcOffsetKnown) {
            // Offset unknown: discard the received schedule rather than storing
            // unconverted local slots into a UTC-based scheduler.  Eve will
            // re-send after CURRENT_TIME establishes the offset.
            WEBLOG("SCHEDULER UTC offset unknown; discarding WEEK_SCHEDULE (will re-apply once CURRENT_TIME received)");
            byte_offset += sizeof(PROG_CMD_WEEK_SCHEDULE);
            break;
          }
          memcpy(&prog_send_data.weekSchedule, weekSchedule, sizeof(PROG_CMD_WEEK_SCHEDULE));
          resetSlotScores();  // Eve wire schedule carries no score metadata.
          convertEveSlotsToUTC(offsetToUse);
        }
        updateSchedulerWeekSchedule();
        updateCurrentScheduleIfNeeded(true);
        initializeCurrentState(); // Recalculate current state after schedule change
        byte_offset += sizeof(PROG_CMD_WEEK_SCHEDULE);
        storeData = true;
        break;
      }
        
      case CURRENT_TIME:
      {
        PROG_CMD_CURRENT_TIME *currentTime = (PROG_CMD_CURRENT_TIME *)(&data[byte_offset]);
        Serial.printf("Current Time: ");
        Serial.printf("%d:%d ", currentTime->hours, currentTime->minutes);
        Serial.printf("Day %d ", currentTime->day);
        Serial.printf("Month %d Year 20%d\n", currentTime->month, currentTime->year);
        memcpy(&prog_send_data.currentTime, currentTime, sizeof(PROG_CMD_CURRENT_TIME));
        clockOffset = millis();
        guessTimeZone(currentTime);
        // Compute UTC offset: treat Eve local time as UTC pseudo-epoch, diff against real UTC.
        // Sign: UTC = Eve_local + _lastKnownUtcOffsetMin (PST/UTC-8 → offset = +480).
        {
          struct tm eveUTC = {0};
          eveUTC.tm_year = currentTime->year + 100;
          eveUTC.tm_mon  = currentTime->month - 1;
          eveUTC.tm_mday = currentTime->day;
          eveUTC.tm_hour = currentTime->hours;
          eveUTC.tm_min  = currentTime->minutes;
          time_t evePseudo = proper_timegm(&eveUTC);
          time_t sysUTC    = time(nullptr);
          int newOffset = (int)(difftime(sysUTC, evePseudo) / 60.0 + 0.5);
          if (newOffset < -720 || newOffset > 720) {
            WEBLOG("SCHEDULER UTC offset %d min out of range, ignoring", newOffset);
          } else {
            _lastKnownUtcOffsetMin = newOffset;
            _utcOffsetKnown = true;
          }
        }
        byte_offset += sizeof(PROG_CMD_CURRENT_TIME);
        break;
      }
        
      case UNKNOWN_FF:
      {
        PROG_CMD_UNKNOWN_FF *unknown_ff = (PROG_CMD_UNKNOWN_FF *)(&data[byte_offset]);
        Serial.printf("Unknown_FF ");
        Serial.printf("Value 1: %x, ", unknown_ff->unknown_01);
        Serial.printf("Value 2: %x", unknown_ff->unknown_02);
        Serial.println("");
        byte_offset += sizeof(PROG_CMD_UNKNOWN_FF);
        break;
      }
        
      default:
        Serial.printf("Found unknown header packet %d\n", data[byte_offset]);
        printData(data, len);
          // Stop parsing as I don't know how long this header is
        byte_offset = len;
        break;
    }
  }
  
  addMilliseconds(&prog_send_data.currentTime, millis() - clockOffset);
  clockOffset = millis();
  if (storeData) {
    nvs_set_blob(savedData,"PROG_SEND_DATA",&prog_send_data,sizeof(prog_send_data));      // update data in the NVS
    saveSlotScoresToStorage();
    nvs_commit(savedData);
    refreshProgramData = true;
  }
}

void FakeGatoScheduler::updateCurrentScheduleIfNeeded(bool force) {
  // Intentionally localtime — selects which local day to show Eve.
  time_t now = time(nullptr);
  struct tm *tm_struct = localtime(&now);
  int eveDayOfWeek = (tm_struct->tm_wday + 6) % 7; // Convert to Monday - Sunday
  if (force || currentScheduleDay != eveDayOfWeek) {
    currentScheduleDay = eveDayOfWeek;
    memcpy(&prog_send_data.currentSchedule.current,
           &prog_send_data.weekSchedule.day[currentScheduleDay], sizeof(PROG_CMD_CURRENT_SCHEDULE));
  }
}

bool FakeGatoScheduler::setWeekScheduleFromJSON(const String &json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("setWeekScheduleFromJSON: JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray schedule = doc["schedule"];
  if (!schedule || schedule.size() != 7) {
    Serial.println("setWeekScheduleFromJSON: 'schedule' must be an array of 7 days");
    return false;
  }

  // Clear all Eve week schedule slots to 0xFF (unused)
  memset(&prog_send_data.weekSchedule.day, 0xFF, sizeof(prog_send_data.weekSchedule.day));
  resetSlotScores();

  // JSON index:  0=Sunday .. 6=Saturday  (SchedulerBase order)
  // Eve storage: 0=Monday .. 6=Sunday
  // Mapping:  Eve day = (SchedulerBase day + 6) % 7
  for (int dow = 0; dow < 7; dow++) {
    JsonObject dayObj = schedule[dow];
    if (dayObj.isNull()) {
      Serial.printf("setWeekScheduleFromJSON: day %d is not an object\n", dow);
      return false;
    }
    JsonArray slots = dayObj["slots"];

    int eveDay = (dow + 6) % 7;
    CMD_DAY_SCHEDULE *eveDaySchedule = &prog_send_data.weekSchedule.day[eveDay];

    int slotIdx = 0;
    if (slots) {
      for (JsonObject slot : slots) {
        if (slotIdx >= 3) break;
        uint8_t sh = slot["startHour"]   | 0xFF;
        uint8_t sm = slot["startMinute"] | 0xFF;
        uint8_t eh = slot["endHour"]     | 0xFF;
        uint8_t em = slot["endMinute"]   | 0xFF;

        if (sh > 23 || sm > 59 || eh > 23 || em > 59) {
          Serial.printf("setWeekScheduleFromJSON: invalid time in day %d slot %d\n", dow, slotIdx);
          return false;
        }
        eveDaySchedule->slot[slotIdx].offset_start = sh * 6 + sm / 10;
        eveDaySchedule->slot[slotIdx].offset_end   = eh * 6 + em / 10;
        _slotScoreUtc[eveDay][slotIdx] = slot["score"] | SLOT_SCORE_UNKNOWN;
        slotIdx++;
      }
    }
  }

  // Sync weekSchedule[] (SchedulerBase format) from the updated Eve data
  updateSchedulerWeekSchedule();
  updateCurrentScheduleIfNeeded(true);

  // Persist to NVS
  nvs_set_blob(savedData, "PROG_SEND_DATA", &prog_send_data, sizeof(prog_send_data));
  saveSlotScoresToStorage();
  nvs_commit(savedData);
  refreshProgramData = true;

  return true;
}

int FakeGatoScheduler::begin() {
  if (SchedulerBase::begin()) {
    // Check schedVersion in SAVED_DATA.  Any firmware prior to Phase 3 stored
    // local-time slots; version 1 means slots are already UTC.  If missing or
    // wrong, clear the schedule so the user re-pushes from Eve (which will
    // then convert via convertEveSlotsToUTC on the next write).
    uint8_t schedVersion = 0;
    nvs_get_u8(savedData, "schedVersion", &schedVersion);
    if (schedVersion != 1) {
      WEBLOG("SCHEDULER schedVersion %d: clearing local-time slots; re-push schedule from Eve", (int)schedVersion);
      memset(&prog_send_data.weekSchedule.day, 0xFF, sizeof(prog_send_data.weekSchedule.day));
      memset(&prog_send_data.currentSchedule.current, 0xFF, sizeof(prog_send_data.currentSchedule.current));
      resetSlotScores();
      nvs_set_u8(savedData, "schedVersion", 1);
      nvs_set_blob(savedData, "PROG_SEND_DATA", &prog_send_data, sizeof(prog_send_data));
      saveSlotScoresToStorage();
      nvs_commit(savedData);
    }

    // Re-apply Eve schedule data on top of whatever SchedulerBase::begin()
    // loaded. Eve data in prog_send_data is the authoritative source.
    updateSchedulerWeekSchedule();
    updateCurrentScheduleIfNeeded(true);

    // Slots are now UTC (either confirmed by schedVersion==1 or cleared for re-push).
    setScheduleUtcMode(true);

    return true;
  }

  return false;
}

void FakeGatoScheduler::loop() {
  SchedulerBase::loop();

  // Apply any new schedule produced by NavienLearner on Core 0.
  // checkNewSchedule() is non-blocking; it returns false immediately if no
  // new schedule is ready.  setWeekScheduleFromJSON() must only run on Core 1.
  // Only apply learner-generated (UTC) schedules once the scheduler firing
  // path is also UTC-based (Phase 3).  Before that, applying UTC slots to a
  // localtime-based scheduler would cause firings at the wrong wall-clock time.
  if (_scheduleIsUtc && learner && !learner->isDisabled()) {
    String newScheduleJson;
    if (learner->checkNewSchedule(newScheduleJson)) {
      setWeekScheduleFromJSON(newScheduleJson);
    }
  }

  if (isInitialized && (refreshProgramData || programData->timeVal() > 60000) ){
    updateCurrentScheduleIfNeeded(false);
    addMilliseconds(&prog_send_data.currentTime, millis() - clockOffset);
    clockOffset = millis();

    // Build a local-time copy for Eve readback; prog_send_data stays UTC internally.
    PROG_DATA_FULL_DATA sendData = prog_send_data;
    if (_scheduleIsUtc) {
      // Determine UTC→local offset for readback.  Prefer the Eve-confirmed offset;
      // fall back to the system TZ (loaded from NVS in begin()) so the first Eve
      // connection after a reboot doesn't send UTC times that Eve misinterprets as
      // local (which would cause a double-conversion on the next write).
      int effectiveOffsetMin = 0;
      if (_utcOffsetKnown) {
        effectiveOffsetMin = _lastKnownUtcOffsetMin;
      } else if (getenv("TZ") && time(nullptr) > 1700000000L) {
        time_t now2 = time(nullptr);
        struct tm local_tm, utc_tm;
        localtime_r(&now2, &local_tm);
        gmtime_r(&now2, &utc_tm);
        effectiveOffsetMin = (utc_tm.tm_hour * 60 + utc_tm.tm_min)
                           - (local_tm.tm_hour * 60 + local_tm.tm_min);
        if (effectiveOffsetMin >  720) effectiveOffsetMin -= 1440;
        if (effectiveOffsetMin < -720) effectiveOffsetMin += 1440;
      }

      // Convert stored UTC schedule → local time for Eve display.
      convertSlotsOffset(prog_send_data.weekSchedule, sendData.weekSchedule,
                         -effectiveOffsetMin);
      // currentSchedule must also reflect local-time today for Eve.
      time_t now2 = time(nullptr);
      struct tm *ts = localtime(&now2);  // intentionally local-time — Eve display
      int eveDow = (ts->tm_wday + 6) % 7;
      memcpy(&sendData.currentSchedule.current,
             &sendData.weekSchedule.day[eveDow], sizeof(CMD_DAY_SCHEDULE));
    }

    // Don't announce when there is new program data, Eve app will fetch it when it wants it.
    programData->setData((const uint8_t *)&sendData, sizeof(PROG_DATA_FULL_DATA), refreshProgramData);
    refreshProgramData = false;
  }
}


void FakeGatoScheduler::convertSlotsOffset(const PROG_CMD_WEEK_SCHEDULE &in,
                                            PROG_CMD_WEEK_SCHEDULE &out,
                                            int offsetMin,
                                            const float inScores[7][SLOT_SCORE_STORAGE_SLOTS],
                                            float outScores[7][SLOT_SCORE_STORAGE_SLOTS]) {
  // General slot offset converter. offsetMin > 0 shifts forward (local→UTC for PST),
  // offsetMin < 0 shifts backward (UTC→local for readback).
  // Eve day 0=Monday..6=Sunday; offset units are value*10 minutes past midnight.
  memset(&out.day, 0xFF, sizeof(out.day));
  if (outScores) {
    for (int d = 0; d < 7; d++) {
      for (int s = 0; s < SLOT_SCORE_STORAGE_SLOTS; s++) {
        outScores[d][s] = SLOT_SCORE_UNKNOWN;
      }
    }
  }

  for (int d = 0; d < 7; d++) {
    const CMD_DAY_SCHEDULE *daySched = &in.day[d];
    for (int i = 0; i < 4 && daySched->slot[i].offset_start != 0xFF; i++) {
      int t_start = daySched->slot[i].offset_start * 10; // minutes since midnight
      int t_end   = daySched->slot[i].offset_end   * 10;
      int shifted_start = t_start + offsetMin;
      int shifted_end   = t_end   + offsetMin;

      // Day rollover: adjust target day and normalise both times together.
      int target_day = d;
      if (shifted_start < 0) {
        target_day = (d + 6) % 7; // previous day
        shifted_start += 1440;
        shifted_end   += 1440;
      } else if (shifted_start >= 1440) {
        target_day = (d + 1) % 7; // next day
        shifted_start -= 1440;
        shifted_end   -= 1440;
      }

      // Clamp end to 23:50 (offset 143); warn if slot straddles midnight.
      if (shifted_end > 1430) {
        WEBLOG("SCHEDULER convertSlotsOffset: Eve day %d slot %d straddles midnight (offset=%d), truncating end to 23:50", d, i, offsetMin);
        shifted_end = 1430;
      }
      if (shifted_end <= shifted_start) {
        WEBLOG("SCHEDULER convertSlotsOffset: Eve day %d slot %d zero/negative duration after conversion, skipping", d, i);
        continue;
      }

      // Round to nearest 10-min boundary (Eve wire format resolution).
      // Without rounding, a 1-min jitter in _lastKnownUtcOffsetMin truncates
      // to the wrong 10-min slot (e.g. 599/10=59 → 9:50 instead of 10:00).
      uint8_t off_start = (uint8_t)((shifted_start + 5) / 10);
      uint8_t off_end   = (uint8_t)((shifted_end   + 5) / 10);
      float slotScore = inScores ? inScores[d][i] : SLOT_SCORE_UNKNOWN;
      // Clamp to valid Eve range (max 23:50 = offset 143).
      if (off_start > 143) off_start = 143;
      if (off_end   > 143) off_end   = 143;

      // Write into first free slot of the target day (max 3 — Eve UI limit).
      CMD_DAY_SCHEDULE *targetDay = &out.day[target_day];
      bool written = false;
      for (int s = 0; s < ACTIVE_SLOT_LIMIT; s++) {
        if (targetDay->slot[s].offset_start == 0xFF) {
          targetDay->slot[s].offset_start = off_start;
          targetDay->slot[s].offset_end   = off_end;
          if (outScores) outScores[target_day][s] = slotScore;
          written = true;
          break;
        }
      }
      if (!written) {
        int worst = 0;
        float worstScore = outScores ? outScores[target_day][0] : SLOT_SCORE_UNKNOWN;
        uint8_t worstStart = targetDay->slot[0].offset_start;
        for (int s = 1; s < ACTIVE_SLOT_LIMIT; s++) {
          float sScore = outScores ? outScores[target_day][s] : SLOT_SCORE_UNKNOWN;
          uint8_t sStart = targetDay->slot[s].offset_start;
          if (sScore < worstScore || (fabsf(sScore - worstScore) < 0.0001f && sStart > worstStart)) {
            worst = s;
            worstScore = sScore;
            worstStart = sStart;
          }
        }
        bool replace = false;
        if (slotScore > worstScore) {
          replace = true;
        } else if (fabsf(slotScore - worstScore) < 0.0001f && off_start < worstStart) {
          // Deterministic tie-break for equal scores: prefer earlier local start.
          // This keeps earlier demand windows visible/retained when constrained
          // by Eve's 3-slot-per-day cap.
          replace = true;
        }
        if (replace) {
          targetDay->slot[worst].offset_start = off_start;
          targetDay->slot[worst].offset_end   = off_end;
          if (outScores) outScores[target_day][worst] = slotScore;
        }
      }
    }
  }
}

int FakeGatoScheduler::getEffectiveOffsetMin() const {
  if (_utcOffsetKnown) return _lastKnownUtcOffsetMin;
  if (getenv("TZ") && time(nullptr) > 1700000000L) {
    time_t now = time(nullptr);
    struct tm local_tm, utc_tm;
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
    int off = (utc_tm.tm_hour * 60 + utc_tm.tm_min)
            - (local_tm.tm_hour * 60 + local_tm.tm_min);
    if (off >  720) off -= 1440;
    if (off < -720) off += 1440;
    return off;
  }
  return INT_MIN;
}

void FakeGatoScheduler::sanitizeScheduleToLocalLimit(int offsetMin) {
  // Round-trip UTC→local→UTC. convertSlotsOffset enforces 3 slots per output
  // day in both directions, so any slot that would be invisible in Eve (because
  // its local day is already full) is dropped from storage here, preventing it
  // from firing silently.
  PROG_CMD_WEEK_SCHEDULE localView, sanitized;
  float localScores[7][SLOT_SCORE_STORAGE_SLOTS], sanitizedScores[7][SLOT_SCORE_STORAGE_SLOTS];
  convertSlotsOffset(prog_send_data.weekSchedule, localView,  -offsetMin,
                     _slotScoreUtc, localScores);
  convertSlotsOffset(localView,                   sanitized,  +offsetMin,
                     localScores, sanitizedScores);
  if (memcmp(&prog_send_data.weekSchedule, &sanitized, sizeof(sanitized)) != 0) {
    WEBLOG("SCHEDULER: slots trimmed from UTC schedule — would exceed 3 per local day and be invisible in Eve");
    memcpy(&prog_send_data.weekSchedule, &sanitized, sizeof(sanitized));
    memcpy(&_slotScoreUtc, &sanitizedScores, sizeof(_slotScoreUtc));
  }
}

void FakeGatoScheduler::convertEveSlotsToUTC(int utcOffsetMin) {
  PROG_CMD_WEEK_SCHEDULE orig = prog_send_data.weekSchedule;
  float convertedScores[7][SLOT_SCORE_STORAGE_SLOTS];
  convertSlotsOffset(orig, prog_send_data.weekSchedule, utcOffsetMin,
                     _slotScoreUtc, convertedScores);
  memcpy(&_slotScoreUtc, &convertedScores, sizeof(_slotScoreUtc));
  sanitizeScheduleToLocalLimit(utcOffsetMin);
}

void FakeGatoScheduler::printData(uint8_t *data, int len) {
  Serial.printf("Data %d ", len);
  for (int i = 0; i < len; i++) {
    if (data[i] < 0x10)
      Serial.printf("0%x ", data[i]);
    else
      Serial.printf("%x ", data[i]);
  }
  Serial.println("");
}

void FakeGatoScheduler::printDaySchedule(CMD_DAY_SCHEDULE *daySchedule) {
  for (int i = 0; i < 4; i++) {
    if (daySchedule->slot[i].offset_start != UNSET) {
      Serial.printf("%d:%d - %d:%d ", (int)(daySchedule->slot[i].offset_start / 6),
                    (int)((daySchedule->slot[i].offset_start % 6) * 10),
                    (int)(daySchedule->slot[i].offset_end / 6),
                    (int)((daySchedule->slot[i].offset_end % 6) * 10));
    }
  }
  Serial.println("");
}
