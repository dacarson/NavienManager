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

#pragma once

#include "HomeSpan.h"
#include <LittleFS.h>

#define EPOCH_OFFSET 978307200
#define MEMORY_SIZE 3024

CUSTOM_SERV(FakeGatoHistoryData, E863F007-079E-48FF-8F27-9C2605A29F52);
CUSTOM_CHAR_DATA(EveHistoryStatus, E863F116-079E-48FF-8F27-9C2605A29F52, PR+EV+HD);
CUSTOM_CHAR_DATA(EveHistoryEntries, E863F117-079E-48FF-8F27-9C2605A29F52, PR+EV+HD);
CUSTOM_CHAR_DATA(EveHistoryRequest, E863F11C-079E-48FF-8F27-9C2605A29F52, PW+HD);
CUSTOM_CHAR_DATA(EveSetTime, E863F121-079E-48FF-8F27-9C2605A29F52, PW+HD);

struct LogEntry {
    uint32_t time;
    uint16_t currentTemp;
    uint16_t targetTemp;
    uint8_t valvePercent;
    uint8_t thermoTarget;
    uint8_t openWindow;
};

// e1f50500 41dd0500 09c5ef1f 05 0102 1102 1001 1201 1d01 9202 de0f 00000000 00000000 0101
union HistoryStatusData {
    struct {
        uint32_t timeSinceLastUpdate;
        uint32_t negativeOffsetrefTime;
        uint32_t refTime;
        uint8_t paramCount;
        uint8_t signature[10];
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
};

struct PersistHistoryData {
    uint16_t historySize = MEMORY_SIZE;
    LogEntry history[MEMORY_SIZE];
    uint16_t firstEntry = 0;
    uint16_t lastEntry = 0;
    uint16_t usedMemory = 0;
    uint32_t refTime = 0;
};

struct AveragedEntry {
    uint32_t count;
    float totalTemp;
    float totalTargetTemp;
    uint16_t totalValvePos;
    uint8_t lastThermoTarget;
    uint8_t lastOpenWindow;
};

class FakeGatoHistoryService : public Service::FakeGatoHistoryData {
public:
    FakeGatoHistoryService();
    void accumulateLogEntry(float currentTemp, float targetTemp, uint8_t valvePercent, uint8_t thermoTarget, uint8_t openWindow);
    void generateTimedHistoryEntry();
    void addHistoryEntry(float currentTemp, float targetTemp, uint8_t valvePercent, uint8_t thermoTarget, uint8_t openWindow);
    bool saveHistory();
    bool loadHistory();
    void eraseHistory();
    void updateAndSetHistoryStatus();
    void sendHistory(uint32_t currentEntry);
    boolean update() override;
    void loop() override;

    PersistHistoryData store;

private:
    Characteristic::EveHistoryStatus historyStatus;
    Characteristic::EveHistoryEntries historyEntries;
    Characteristic::EveHistoryRequest historyRequest;
    Characteristic::EveSetTime setTime;
    HistoryStatusData historyStatusData;
    AveragedEntry avgLog = {0};
    LogEntry previousEntry = {0};
    bool restarted = true;
    bool sendTime = true;
    int logInterval;
    void printData(uint8_t *data, int len);
};