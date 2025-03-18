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
 
#pragma once

#include "SchedulerBase.h"
#include "HomeSpan.h"

  // Custom Characteristics.
CUSTOM_CHAR_DATA(ProgramData, E863F12F-079E-48FF-8F27-9C2605A29F52, PR + EV);

class FakeGatoScheduler : public SchedulerBase {
public:
  FakeGatoScheduler();
  virtual ~FakeGatoScheduler() {}
  
  virtual int begin();
  virtual void loop();

  bool enabled() { return scheduleActive; }

  static String getSchedulerState(int state); // Return the state as a string

  // When the Service recieves Eve Program Data,
  // this function is called to process it.
  void parseProgramData(uint8_t *data, int len);
  
  
protected:
    // Implement the base schedule storage functions to do nothing
    // as the schedule is stored inside prog_send_data
  virtual bool saveScheduleToStorage() { return true; };
  virtual bool loadScheduleFromStorage() {return true; };
  
  virtual void stateChange(State newState);
  
  
    // Section headers
  enum {
    BEGIN_BLOCK = 0x00,
    END_BLOCK = 0x06,
    VALVE_PROTECTION = 0x11,
    TEMPERATURE_OFFSET = 0x12,
    SCHEDULE_STATE = 0x13,
    INSTALLED_STATUS = 0x14,
    UNKNOWN_BLOCK = 0x17,
    VACATION_MODE = 0x19,
    CURRENT_SCHEDULE = 0x1a,
    TEMPERATURES = 0xf4,
    OPEN_WINDOW = 0xf6,
    WEEK_SCHEDULE = 0xfa,
    CURRENT_TIME = 0xfc,
    UNKNOWN_FF = 0xff
  };
  
  struct PROG_CMD_CURRENT_TIME {
    uint8_t header = CURRENT_TIME;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t month;
    uint8_t year;  // year since 2000
  };
  
  typedef struct {
      // Offsets are value * 10 minutes past midnight
    uint8_t offset_start;
    uint8_t offset_end;
  } CMD_TIME_SLOT;
  
  typedef struct {
    CMD_TIME_SLOT slot[4];
  } CMD_DAY_SCHEDULE;
  
  void addMilliseconds(PROG_CMD_CURRENT_TIME *timeStruct, uint32_t milliseconds);
  void updateSchedulerWeekSchedule();
  void updateCurrentScheduleIfNeeded(bool force = false);

  void guessTimeZone(PROG_CMD_CURRENT_TIME *currentTime);
  
    // Debug functions
  static void printData(uint8_t *data, int len);
  static void printDaySchedule(CMD_DAY_SCHEDULE *daySchedule);
  
  Characteristic::ProgramData *programData;
  
  nvs_handle savedData;
  bool refreshProgramData; // Keep track to see if we need to refresh the data sent for Program Data
  unsigned long clockOffset;
  uint8_t temperature_offset = 0;
  int currentScheduleDay = -1;
  
  bool scheduleActive;
  
  struct PROG_CMD_SCHEDULE_STATE {
    uint8_t header = SCHEDULE_STATE;
    uint8_t schedule_on = 0;  // 00 Schedule off, 01 schedule on
  };
  
  struct PROG_CMD_TEMPERATURES {
      // Temp in 0.5 increments
    uint8_t header = TEMPERATURES;
    uint8_t defaultTemp;
    uint8_t economyScheduleTemp;
    uint8_t comfortScheduleTemp;
  };
  
  struct PROG_DATA_TEMPERATURES {
      // Temp in 0.5 increments
    uint8_t header = TEMPERATURES;
    uint8_t unknown;  // 0x24?
    uint8_t defaultTemp;
    uint8_t economyScheduleTemp;
    uint8_t comfortScheduleTemp;
  };
  
  struct PROG_CMD_WEEK_SCHEDULE {
    uint8_t header = WEEK_SCHEDULE;
    CMD_DAY_SCHEDULE day[7];
  };
  
  struct PROG_CMD_CURRENT_SCHEDULE {
    uint8_t header = CURRENT_SCHEDULE;
    CMD_DAY_SCHEDULE current;
  };
  
  struct PROG_CMD_VACATION_MODE {
    uint8_t header = VACATION_MODE;
    uint8_t enabled;
    uint8_t away_temp;
  };
  
  struct PROG_DATA_INSTALLED_STATUS {
    uint8_t header = INSTALLED_STATUS;
    uint8_t status = 0xc0;  // 0xc0 == OK, 0xc7 == NotAttached
  };
  
  struct PROG_DATA_UNKNOWN_BLOCK {
    uint8_t header = UNKNOWN_BLOCK; // 0x17
    uint8_t unknown_01;  // 0x04
    uint8_t unknown_02;  // 0x0a
  };
  struct PROG_DATA_OPEN_WINDOW {
    uint8_t header = OPEN_WINDOW; 
    uint8_t unknown_01;  // 0x01  00 == Ok? 10 == open window
    uint8_t unknown_02;  // 0x07
    uint8_t unknown_03;  // 0x01
  };
  
  struct PROG_CMD_VALVE_PROTECT {
    uint8_t header = VALVE_PROTECTION;
    uint8_t unknown_01;  // 0xFF
    uint8_t unknown_02;  // 0x00
    uint8_t unknown_03;  // 0xF2
    uint8_t unknown_04;  // 0x20
    uint8_t unknown_05;  // 0x76
  } ;
  
  struct PROG_CMD_TEMPERATURE_OFFSET {
    uint8_t header = TEMPERATURE_OFFSET;
    uint8_t offset;  // Temperature offset if degC
  };
  
  struct PROG_CMD_UNKNOWN_FF {
    uint8_t header = UNKNOWN_FF;
    uint8_t unknown_01;  // 0x04 0xFF
    uint8_t unknown_02;  // 0xF6 0xFF
  };
  
    // Outgoing
  typedef struct {
    PROG_CMD_TEMPERATURE_OFFSET temp_offset;
    PROG_CMD_SCHEDULE_STATE schedule_state;
    PROG_DATA_INSTALLED_STATUS install_status;
    PROG_CMD_CURRENT_TIME currentTime;
    PROG_CMD_WEEK_SCHEDULE weekSchedule;
    PROG_DATA_TEMPERATURES temperatures;
    PROG_CMD_CURRENT_SCHEDULE currentSchedule;
    PROG_CMD_VACATION_MODE vacation;
  } PROG_DATA_FULL_DATA;
  
  PROG_DATA_FULL_DATA prog_send_data;
  
    // Packet Headers
  enum {
    UNSET = 0xff
  };
  
};
