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

/*
 FakeGatoScheduler
 
 */

#define CUSTOM_CHAR_HEADER
#include "FakeGatoScheduler.h"
#include "Navien.h"

extern Navien navienSerial;

FakeGatoScheduler::FakeGatoScheduler(Characteristic::ProgramCommand *prgCommand,
                                     Characteristic::ProgramData *prgData)
: SchedulerBase(), programCommand(prgCommand), programData(prgData) {
  
  size_t len;
  nvs_open("SAVED_DATA",NVS_READWRITE,&savedData);       // open a new namespace called SAVED_DATA in the NVS
  if(!nvs_get_blob(savedData,"PROG_SEND_DATA",NULL,&len)) {        // if PROG_SEND_DATA data found
    nvs_get_blob(savedData,"PROG_SEND_DATA",&prog_send_data,&len);       // retrieve data
    
    WEBLOG("SCHEDULER Loaded Program State");
      // Setup the scheduler state.
    if (prog_send_data.schedule_state.schedule_on)
      scheduleActive = true;
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
  }
  
  updateSchedulerWeekSchedule();
  refreshProgramData = true;
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
        WEBLOG("Next event scheduled for: %02d:%02d %02d/%02d/%04d\n",
              tm_struct->tm_hour, tm_struct->tm_min,
              tm_struct->tm_mon + 1, tm_struct->tm_mday,  // tm_mon is 0-based
              tm_struct->tm_year + 1900);               // tm_year is years since 1900

  
  if (!scheduleActive) {
    WEBLOG("Ignoring state change, scheduler not active.");
    return;
  }
  
    // Should not need to update the targetState of the Thermostat
    // as that will update when the state changes in the Navien
  switch (newState) {
    case State::Override:
      WEBLOG("SCHEDULER Override");
      // Fall through
    case State::Active:
      WEBLOG("SCHEDULER going active");
      if (currentState == State::Vacation)
        navienSerial.power(true);
      if (currentState != State::Override)
        if (navienSerial.recirculation(true) == -1)
          WEBLOG("Failed to enable Recirculation.");
        // ignore setpoints as they only go to 30 degC
        //navienSerial.setTemp(0.5 * prog_send_data.temperatures.comfortScheduleTemp);
      break;
      
    case State::InActive:
      WEBLOG("SCHEDULER going inactive");
      if (currentState == State::Vacation)
        navienSerial.power(true);
      if (navienSerial.recirculation(false) == -1)
        WEBLOG("Failed to disable Recirculation.");
        // ignore setpoints as they only go to 30 degC
        //navienSerial.setTemp(0.5 * prog_send_data.temperatures.comfortScheduleTemp);
      break;
      
    case State::Vacation:
      WEBLOG("SCHEDULER going vacation");
      navienSerial.recirculation(false);
      if (navienSerial.power(false) == -1)
        WEBLOG("Failed to turn power off");
      break;
  }
  
}

void FakeGatoScheduler::addMilliseconds(PROG_CMD_CURRENT_TIME *timeStruct, uint32_t milliseconds) {
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

  if (getenv("TZ")) {
    return; // TZ already set.
  }

    struct tm eveTimeInfo = {0};  // Initialize to zero

    eveTimeInfo.tm_year = eveLocalTime->year + 100;  // tm_year is years since 1900
    eveTimeInfo.tm_mon  = eveLocalTime->month - 1;   // tm_mon is 0-based (Jan = 0)
    eveTimeInfo.tm_mday = eveLocalTime->day;
    eveTimeInfo.tm_hour = eveLocalTime->hours;
    eveTimeInfo.tm_min  = eveLocalTime->minutes;
    eveTimeInfo.tm_sec  = 0;  // Assuming seconds are zero

    time_t localTime =  mktime(&eveTimeInfo);  // Convert to time_t (Unix timestamp)
    time_t currentTime = time(nullptr);  // Get the current system time

    double timeDiffSeconds = difftime(currentTime, localTime);
    int timeDiffHours = std::round(timeDiffSeconds / 3600.0);
    
    char tzString[10];
    snprintf(tzString, sizeof(tzString), "UTC%+d", timeDiffHours);
    WEBLOG("Estimating TZ to be: %s\n", tzString);

    setTz(String(tzString));
}

void FakeGatoScheduler::updateSchedulerWeekSchedule() {
    // Note Eve schedules are Monday - Sunday
    // SchedulerBase are Sunday - Saturday
  
  for (int day = 0; day < 7; day++) { // Monday - Sunday
    CMD_DAY_SCHEDULE *daySchedule = &(prog_send_data.weekSchedule.day[day]);
    for (int i = 0; i < 4 && daySchedule->slot[i].offset_start != 0xFF; i++) {
      weekSchedule[(day + 1) % 7].slots[i].startHour = (uint8_t)(daySchedule->slot[i].offset_start / 6);
      weekSchedule[(day + 1) % 7].slots[i].startMinute = (uint8_t)(daySchedule->slot[i].offset_start % 6) * 10;
      weekSchedule[(day + 1) % 7].slots[i].endHour = (uint8_t)(daySchedule->slot[i].offset_end / 6);
      weekSchedule[(day + 1) % 7].slots[i].endMinute = (uint8_t)(daySchedule->slot[i].offset_end % 6) * 10;
    }
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
        if (schedule_state->schedule_on) {
          scheduleActive = true;
          Serial.println("Schedule: On");
        } else {
          scheduleActive = false;
          Serial.println("Schedule: Off");
        }
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
        
        prog_send_data.temperatures.header = TEMPERATURES;
        prog_send_data.temperatures.unknown = 0x00;
        prog_send_data.temperatures.defaultTemp = temperatures->defaultTemp;
        prog_send_data.temperatures.economyScheduleTemp = temperatures->economyScheduleTemp;
        prog_send_data.temperatures.comfortScheduleTemp = temperatures->comfortScheduleTemp;
        byte_offset += sizeof(PROG_CMD_TEMPERATURES);
        storeData = true;
        break;
      }
      
      case OPEN_WINDOW: {
        PROG_DATA_OPEN_WINDOW *open_window = (PROG_DATA_OPEN_WINDOW *)(&data[byte_offset]);
        Serial.print("Open window: ");
        Serial.print(open_window->unknown_01);
        Serial.print(" ");
        Serial.print(open_window->unknown_01);
        Serial.print(" ");
        Serial.println(open_window->unknown_02);
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
        memcpy(&prog_send_data.weekSchedule, weekSchedule, sizeof(PROG_CMD_WEEK_SCHEDULE));
        updateSchedulerWeekSchedule();
        updateCurrentScheduleIfNeeded(true);
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
    nvs_commit(savedData);
    refreshProgramData = true;
  }
}

void FakeGatoScheduler::updateCurrentScheduleIfNeeded(bool force) {
  time_t now = time(nullptr);
  struct tm *tm_struct = localtime(&now);
  int eveDayOfWeek = (tm_struct->tm_wday + 6) % 7; // Convert to Monday - Sunday
  if (force || currentScheduleDay != eveDayOfWeek) {
    currentScheduleDay = eveDayOfWeek;
    memcpy(&prog_send_data.currentSchedule.current,
           &prog_send_data.weekSchedule.day[currentScheduleDay], sizeof(PROG_CMD_CURRENT_SCHEDULE));
  }
}

int FakeGatoScheduler::begin() {
  if (SchedulerBase::begin()) {
    updateCurrentScheduleIfNeeded(true);
    return true;
  }
  
  return false;
}

void FakeGatoScheduler::loop() {
  SchedulerBase::loop();
  
  if (refreshProgramData || programData->timeVal() > 60000) {
    updateCurrentScheduleIfNeeded(false);
    addMilliseconds(&prog_send_data.currentTime, millis() - clockOffset);
    clockOffset = millis();
    programData->setData((const uint8_t *)&prog_send_data, sizeof(PROG_DATA_FULL_DATA), false);
    refreshProgramData = false;
  }
}

void FakeGatoScheduler::update() {
  if (programCommand->updated()) {
    int len = programCommand->getNewData(0, 0);
    uint8_t *data = (uint8_t *)malloc(sizeof(uint8_t) * len);
    programCommand->getNewData(data, len);
    parseProgramData(data, len);
    delete data;
  }
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
