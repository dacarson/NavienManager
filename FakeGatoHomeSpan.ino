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

 Based on Eve History Protocol documented here:
 https://github.com/An00bIS47/Homekit/blob/3908e0799cc9fae23791c562420145f8f8491269/docs/eve/Elgato-Eve-History-Protocol.md
 and Eve History implementation here:
 https://github.com/simont77/fakegato-history
 */

#include "HomeSpan.h"  // Corrected include syntax
#include <LittleFS.h>

#define EPOCH_OFFSET 978307200  // Seconds from 1970-01-01 to 2001-01-01
#define MEMORY_SIZE 3024  // Log entry buffer size, 3 weeks at a record per 10 mins

CUSTOM_SERV(EveHistoryData, E863F007-079E-48FF-8F27-9C2605A29F52);

CUSTOM_CHAR_DATA(EveHistoryStatus, E863F116-079E-48FF-8F27-9C2605A29F52, PR+EV+HD); // EveS2R1
CUSTOM_CHAR_DATA(EveHistoryEntries, E863F117-079E-48FF-8F27-9C2605A29F52, PR+EV+HD); // EveS2R2
CUSTOM_CHAR_DATA(EveHistoryRequest, E863F11C-079E-48FF-8F27-9C2605A29F52, PW+EV+HD); // EveS2W1
CUSTOM_CHAR_DATA(EveSetTime, E863F121-079E-48FF-8F27-9C2605A29F52, PW+EV+HD); // EveS2W2

struct EveHistoryService : Service::EveHistoryData {
  
  Characteristic::EveHistoryStatus historyStatus;
  Characteristic::EveHistoryEntries historyEntries;
  Characteristic::EveHistoryRequest historyRequest;
  Characteristic::EveSetTime setTime;
  
  typedef struct {
    uint32_t time; // Time of the entry
    uint16_t currentTemp;  // 0102: Temperature (x100)
    uint16_t targetTemp;   // 1102: Target Temperature (x100)
    uint8_t valvePercent;  // 1001: Valve position (%)
    uint8_t thermoTarget;  // 1201: Target mode
    uint8_t openWindow;    // 1D01: Window status (0 = closed, 1 = open)
  } LogEntry;
  
    // e1f50500 41dd0500 09c5ef1f 05 0102 1102 1001 1201 1d01 9202 de0f 00000000 00000000 01ff
  typedef union {
    struct {
      uint32_t timeSinceLastUpdate;
      uint32_t negativeOffsetrefTime;  // always zero
      uint32_t refTime;
      uint8_t paramCount; // 05
      uint8_t signature[10]; // "0102 1102 1001 1201 1d01"
      /*
       Because of byte alignment, can't use variables.
       uint16_t store.usedMemory;  // this will end up equal to maxEntries
       uint16_t memorySize;
       uint32_t store.firstEntry; // Is zero until entries are rolled over
       uint32_t unknown; // Always zero
       uint8_t end[2];
       */
    } status;
    
    uint8_t raw_data[38];
  } HistoryStatusData;
  
  HistoryStatusData historyStatusData;
    // Data to persist
  typedef struct {
    uint16_t historySize = MEMORY_SIZE;
    LogEntry history[MEMORY_SIZE];  // Circular log buffer
    uint16_t firstEntry = 0;
    uint16_t lastEntry = 0;           // Last entry position
    uint16_t usedMemory = 0;
    uint32_t refTime = 0;
  } PersistHistoryData;
  PersistHistoryData store;

  
  bool restarted = true;
  bool sendTime = true;

  #define LOG_ENTRY_FREQ_TEN_MIN 600000  // 10-minute interval (600000 milliseconds)
  #define LOG_ENTRY_FREQ_ONE_MIN  60000  // 1-minute interval (10000 millseconds)
  int logInterval = LOG_ENTRY_FREQ_TEN_MIN;
  
  
  typedef struct {
    uint32_t count;  // Number of entries in this interval

    float totalTemp;
    float totalTargetTemp;
    uint16_t totalValvePos;

    uint8_t lastThermoTarget;
    uint8_t lastOpenWindow;
  } AveragedEntry;
  
  AveragedEntry avgLog = {0};  // Initialize the averaging structure
  
  EveHistoryService() : Service::EveHistoryData{} {
    
    Serial.println(F("Configuring Eve History Service"));

        // Initialize HistroyStatusStructure
    memset(&historyStatusData, 0, sizeof (HistoryStatusData));
    historyStatusData.status.paramCount = 0x5;
    uint8_t tempSignature[10] = {0x01, 0x02, 0x11, 0x02, 0x10, 0x01, 0x12, 0x01, 0x1D, 0x01};
    memcpy(historyStatusData.status.signature, tempSignature, sizeof(tempSignature));
    uint16_t memSize = store.historySize;
    memcpy(&historyStatusData.raw_data[25], &memSize, sizeof(uint16_t));
    historyStatusData.raw_data[35] = 0x01;
    historyStatusData.raw_data[36] = 0x01;

    // load the history back into memory
    if (loadHistory()) {
      WEBLOG("Restored History from file");
    } else {
      WEBLOG("Failed to restore History from file, using empty history.");
    }

    updateAndSetHistoryStatus();
    historyEntries.setData(0,0);
    
  }
  
  void accumulateLogEntry(float currentTemp, float targetTemp, uint8_t valvePercent, uint8_t thermoTarget, uint8_t openWindow) {

    // Ignore zero entries.
    if (!currentTemp || !targetTemp) {
      WEBLOG("Ignoring zero value Log entries");
      return;
    }

    if (store.lastEntry && store.history[store.lastEntry % store.historySize].time != 0) { // make sure we have a valid previous entry
      // Check if a key value changed (triggers high-frequency logging)
      bool keyValueChanged = ((uint16_t)(targetTemp * 100) != store.history[store.lastEntry % store.historySize].targetTemp ||
                              thermoTarget != store.history[store.lastEntry % store.historySize].thermoTarget ||
                              valvePercent > 0);

      // **Log accumulated data before switching to high-frequency mode**
      if (keyValueChanged && logInterval == LOG_ENTRY_FREQ_TEN_MIN) {
          if (avgLog.count > 0) {  // Ensure we have accumulated data
            //WEBLOG("Writing low freq averaged data entry");
            generateTimedHistoryEntry();
          }
          logInterval = LOG_ENTRY_FREQ_ONE_MIN;
          /*WEBLOG("Switching to high freq logging. TargetTemp new %d old %d ThermoTarget new %d old %d Valve %d",
                (uint16_t)(targetTemp * 100), store.history[store.lastEntry % store.historySize].targetTemp,
                thermoTarget, store.history[store.lastEntry % store.historySize].thermoTarget, valvePercent); */
      } else if (valvePercent == 0 && logInterval == LOG_ENTRY_FREQ_ONE_MIN) {
          if (avgLog.count > 0) {  // Ensure we have accumulated data
            //WEBLOG("Writing high freq averaged data entry");
            generateTimedHistoryEntry();
          }
        logInterval = LOG_ENTRY_FREQ_TEN_MIN;
        //WEBLOG("Switching to low freq logging");
      }
    } else {
      // We have no entries, so create the first one
      addHistoryEntry(currentTemp, targetTemp, valvePercent, thermoTarget, openWindow);
      return;
    }

      // Accumulate data for the current interval
    avgLog.totalTemp += currentTemp;
    avgLog.totalTargetTemp += targetTemp;
    avgLog.totalValvePos += valvePercent;
    // Store the last value, don't average these values
    avgLog.lastThermoTarget = thermoTarget;
    avgLog.lastOpenWindow = openWindow;

    avgLog.count++;
    
  }
  
    // A new entry is needed every 10 mins. Call this function at minimum 10 minute intervals.
  void generateTimedHistoryEntry() {

    if (avgLog.count > 0) {
      float avgTemp = avgLog.totalTemp / avgLog.count;
      float avgTargetTemp = avgLog.totalTargetTemp / avgLog.count;
      uint8_t avgValvePos = avgLog.totalValvePos / avgLog.count;
      addHistoryEntry(avgTemp, avgTargetTemp, avgValvePos, avgLog.lastThermoTarget, avgLog.lastOpenWindow);  // Send the averaged entry
    } else {
      if (store.lastEntry && store.history[store.lastEntry % store.historySize].time != 0) { // make sure we have a valid previous entry, if so copy it to a new entry
      // Values in history are stored multiplied by 100, need to divide when reading back
        float avgTemp = store.history[store.lastEntry % store.historySize].currentTemp / 100;
        float avgTargetTemp = store.history[store.lastEntry % store.historySize].targetTemp / 100;
        uint8_t avgValvePos = store.history[store.lastEntry % store.historySize].valvePercent;
        uint8_t thermoTarget = store.history[store.lastEntry % store.historySize].thermoTarget;
        uint8_t openWindow = store.history[store.lastEntry % store.historySize].openWindow;
        addHistoryEntry(avgTemp, avgTargetTemp, avgValvePos, thermoTarget, openWindow);  // Send the averaged entry
      }
    }
    
      // Reset for the next interval
    avgLog.totalTemp = 0;
    avgLog.totalTargetTemp = 0;
    avgLog.totalValvePos = 0;
    avgLog.lastThermoTarget = 0;
    avgLog.lastOpenWindow = 0;
    avgLog.count = 0;

    updateAndSetHistoryStatus();
  }
  
  void addHistoryEntry(float currentTemp, float targetTemp, uint8_t valvePercent, uint8_t thermoTarget, uint8_t openWindow) {
    
    if (store.usedMemory < store.historySize) {
      store.usedMemory++;
      store.firstEntry = 0;
      store.lastEntry = store.usedMemory;
    } else {
      store.firstEntry++;
      store.lastEntry = store.firstEntry + store.usedMemory;
      if (restarted) {
        store.history[store.lastEntry % store.historySize].time = 0; // send a store.refTime 0x81 history entry
        store.firstEntry++;
        store.lastEntry = store.firstEntry + store.usedMemory;
        restarted = false;
      }
    }
    
    if (store.refTime == 0) {
      store.refTime =  time(nullptr) - EPOCH_OFFSET;
      store.history[store.lastEntry % store.historySize].time = 0; // send a store.refTime 0x81 history entry
      store.lastEntry++;
      store.usedMemory++;
    }
    
    store.history[store.lastEntry % store.historySize].time = time(nullptr);
    store.history[store.lastEntry % store.historySize].currentTemp = (uint16_t)(currentTemp * 100);
    store.history[store.lastEntry % store.historySize].targetTemp = (uint16_t)(targetTemp * 100);
    store.history[store.lastEntry % store.historySize].valvePercent = valvePercent;
    store.history[store.lastEntry % store.historySize].thermoTarget = thermoTarget;
    store.history[store.lastEntry % store.historySize].openWindow = openWindow;

    saveHistory();
    
  }
  
  #define HISTORY_FILE "/history.bin"

  bool saveHistory() {
    File file = LittleFS.open(HISTORY_FILE, "w");
    if (!file) {
        Serial.println("Failed to save history to file");
        return false;
    }
    
    size_t written = file.write((uint8_t*)&store, sizeof(PersistHistoryData));
    file.close();
    
    return written == sizeof(PersistHistoryData);
  }

  bool loadHistory() {
    File file = LittleFS.open(HISTORY_FILE, "r");
    if (!file) {
        Serial.println("Failed to open history file");
        return false;
    }

    size_t readBytes = file.read((uint8_t*)&store, sizeof(PersistHistoryData));
    file.close();

    return readBytes == sizeof(PersistHistoryData);
  }

  void updateAndSetHistoryStatus() {
    historyStatusData.status.timeSinceLastUpdate = (time(nullptr) - EPOCH_OFFSET) - store.refTime;
    historyStatusData.status.refTime = time(nullptr) < EPOCH_OFFSET ? 0 : store.refTime;
    
    uint16_t uMem = store.usedMemory < store.historySize ? store.usedMemory + 1 : store.usedMemory;
    memcpy(&historyStatusData.raw_data[23], &uMem, sizeof(uint16_t));
    uint32_t first = store.usedMemory < store.historySize ? store.firstEntry : store.firstEntry + 1;
    memcpy(&historyStatusData.raw_data[27], &first, sizeof(uint32_t));
    
    Serial.printf(F("Updating History Status store.usedMemory %d store.lastEntry %d\n"), store.usedMemory, store.lastEntry);
    //printData(historyStatusData.raw_data, sizeof(historyStatusData.raw_data));
    
    // Don't notify everytime history is updated, causes un-neccesary noise. Eve app will fetch it when it wants it.
    historyStatus.setData(historyStatusData.raw_data, sizeof(historyStatusData.raw_data), false);
  }
  
  void printData(uint8_t *data, int len) {
    Serial.printf(F("Data %d "), len);
    for (int i = 0; i < len; i++) {
      if (data[i] < 0x10)
          Serial.printf(F("0%x "), data[i]);
      else
        Serial.printf(F("%x "), data[i]);
    }
    Serial.println("");
  }
  
  void sendHistory(uint32_t currentEntry) {
    if (currentEntry == 0)
        currentEntry = 1;
    
    uint8_t historySendBuffer[960];  // Max HomeKit response size
    int sendBufferLen = 0;
    
    //WEBLOG("Send History currentEntry %d store.lastEntry %d\n", currentEntry, store.lastEntry);
    if (currentEntry <= store.lastEntry) {
      uint32_t memoryAddress = currentEntry % store.historySize;
      
      // Build up the history send buffer
      for (int i = 0; i < 11; i++) {
        Serial.printf(F("MemoryAddress %d\n"), memoryAddress);
        if ((store.history[memoryAddress].time == 0) || (sendTime) || currentEntry == store.firstEntry + 1) { // Its a store.refTime entry or we need to set the time
          Serial.println(F("Sending special Ref Time history entry"));
          uint8_t refEntry[21];
          memset(refEntry, 0, 21);
          refEntry[0] = 21; // entry length
          memcpy(&refEntry[1], &currentEntry, sizeof(uint32_t)); // Current Entry
          refEntry[5] = 0x1; // Secs since reference time set
          refEntry[9] = 0x81;
          memcpy(&refEntry[10], &store.refTime, sizeof(uint32_t));
          
          sendTime = false;
          memcpy(&historySendBuffer[sendBufferLen], &refEntry, 21);
          sendBufferLen += 21;
        } else {
          uint8_t dataEntry[17];
          dataEntry[0] = 17; // entry length
          memcpy(&dataEntry[1], &currentEntry, sizeof(uint32_t));
          // Don't allow negative offsets as the variable is unsigned.
          uint32_t refOffset = 0;
          if ((store.history[memoryAddress].time - EPOCH_OFFSET) >=  store.refTime) {
            refOffset = (store.history[memoryAddress].time - EPOCH_OFFSET) - store.refTime;
          }
          memcpy(&dataEntry[5], &refOffset, sizeof(uint32_t));
          dataEntry[9] = 0x1f;
          memcpy(&dataEntry[10], &store.history[memoryAddress].currentTemp, sizeof(uint16_t));
          memcpy(&dataEntry[12], &store.history[memoryAddress].targetTemp, sizeof(uint16_t));
          dataEntry[14] = store.history[memoryAddress].valvePercent;
          dataEntry[15] = store.history[memoryAddress].thermoTarget;
          dataEntry[16] = store.history[memoryAddress].openWindow;
          memcpy(&historySendBuffer[sendBufferLen], &dataEntry, 17);
          sendBufferLen += 17;
        }
        currentEntry++;
        memoryAddress = currentEntry % store.historySize;
        if (currentEntry > store.lastEntry)
            break;
      }
      Serial.println(F("Sending History"));
      printData(historySendBuffer, sendBufferLen);
      historyEntries.setData(historySendBuffer, sendBufferLen);
    } else {
      Serial.println(F("No History To Send"));
      historyEntries.setData(0, 0);
    }
  }
  
  boolean update() override {
    
    if (historyRequest.updated()) {
      int len = historyRequest.getNewData(0, 0);
      uint8_t *data = (uint8_t *)malloc(sizeof(uint8_t) * len);
      historyRequest.getNewData(data, len);
      uint32_t address = *(uint32_t*)&data[2];
      
      Serial.printf(F("History Request %d\n"), address);
      sendHistory(address);
      delete data;
    }
    
    if (setTime.updated()) {
      int len = setTime.getNewData(0, 0);
      uint8_t *data = (uint8_t *)malloc(sizeof(uint8_t) * len);
      setTime.getNewData(data, len);
      Serial.print(F("Set Time "));
      uint32_t eveTimestamp = *(uint32_t*)data;
      
      time_t currentTime = eveTimestamp + EPOCH_OFFSET;  // Convert to Unix timestamp (since 1970)
      
      struct tm *timeInfo = localtime(&currentTime);          // Convert to time structure
      Serial.printf(F("%02d:%02d %02d/%02d/%04d\n"),
              timeInfo->tm_hour, timeInfo->tm_min,
              timeInfo->tm_mon + 1, timeInfo->tm_mday,  // tm_mon is 0-based
              timeInfo->tm_year + 1900);               // tm_year is years since 1900
      
      uint32_t before = time(nullptr);

      Serial.print(F("Checking the clock. Before: "));
      Serial.print(before);
      Serial.print(F(", After: "));
      Serial.print(currentTime);
      Serial.print(F(", Elapsed: "));
      Serial.println(currentTime - before);
      
      if (currentTime - before > 5) {
        Serial.println(F("Updating local clock"));
        
        // set the clock
        struct timeval tv;
        tv.tv_sec = currentTime;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);

        // Check to see if the reference time was set, but the clock had not been set yet.
        // If so, then we need to correct it
        if (store.refTime != 0 && store.refTime > currentTime ) {
          Serial.printf(F("Fixing refTime %u to %u\n"), store.refTime, eveTimestamp);
          store.refTime = eveTimestamp - (store.refTime + EPOCH_OFFSET);
          Serial.printf(F("Fixed refTime %u\n"), store.refTime);

          // Fix up the History Status Data and republish.
          historyStatusData.status.timeSinceLastUpdate = (currentTime - EPOCH_OFFSET) - store.refTime;
          Serial.printf(F("Correcting timeSincceLastUpdate %u\n"), historyStatusData.status.timeSinceLastUpdate);
          historyStatusData.status.refTime = store.refTime;
          historyStatus.setData(historyStatusData.raw_data, sizeof(historyStatusData.raw_data));

          // Fix up history entries
          for (int i = 1; i <= store.lastEntry; i++) {
            if (store.history[i].time != 0) { // Skip store.refTime 0x81 history entries
              //Serial.printf("Fixing history entry %d from %u to %u\n", i, store.history[i].time, store.history[i].time + currentTime - before);
              store.history[i].time += (currentTime - before);
            }
          }
        }
      }
      
      delete data;
    }
    
    return true;
  }
  
  void loop() override {

      // Create history entries
    if (historyStatus.timeVal() >= logInterval) {
      Serial.printf("Triggering HistoryEntry creation timeVal:%u\n", historyStatus.timeVal());
      generateTimedHistoryEntry();
    }
    
  } // end loop
  
};

