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
#include <map>  // For command map
#include <vector>
#include <ESPTelnet.h>
#include "Navien.h"
#include "nvs.h"
#include "FakeGatoHistoryService.h"
#include "FakeGatoScheduler.h"
#include "NavienLearner.h"
#include "TimeUtils.h"

ESPTelnet telnet;
extern Navien navienSerial;
extern FakeGatoHistoryService *historyService;
extern FakeGatoScheduler *scheduler;
extern NavienLearner *learner;
String trace;

// Functions in NavienBroadcaster.ino
extern String waterToJSON(const Navien::NAVIEN_STATE_WATER *water, String rawhexstring = "");
extern String gasToJSON(const Navien::NAVIEN_STATE_GAS *gas, String rawhexstring = "");


// Define the type for the command callback as a function pointer
typedef void (*CommandCallback)(const String&);

struct Command {
  String description;
  CommandCallback callback;
};

// Command map to store command, description, and callback function
std::map<String, Command> commandMap;

// Helper function to register commands
void registerCommand(const String& commandName, const String& description, CommandCallback callback) {
  commandMap[commandName] = { description, callback };
}

// (optional) callback functions for telnet events
void onTelnetConnect(String ip) {
  Serial.print(F("- Telnet: "));
  Serial.print(ip);
  Serial.println(F(" connected"));

  telnet.print(F("\nWelcome "));
  telnet.println(telnet.getIP());
  telnet.println(F("(Use bye to disconnect.)"));
  telnet.print(F("> "));
}

void onTelnetDisconnect(String ip) {
  Serial.print(F("- Telnet: "));
  Serial.print(ip);
  Serial.println(F(" disconnected"));
}

// Handle Telnet input and dispatch to the appropriate command
void onTelnetInput(String input) {
  input.trim();  // Remove leading/trailing whitespace

  // Split command and arguments
  int firstSpaceIndex = input.indexOf(' ');
  String commandName = (firstSpaceIndex == -1) ? input : input.substring(0, firstSpaceIndex);
  String params = (firstSpaceIndex == -1) ? "" : input.substring(firstSpaceIndex + 1);

  // Check if the command exists in the command map
  if (commandMap.count(commandName)) {
    commandMap[commandName].callback(params);  // Call the callback function with parameters
  } else if (commandName == "help") {
    // Print the list of commands and their descriptions
    telnet.println(F("Available commands:"));
    for (auto& cmd : commandMap) {
      telnet.printf("  %s - %s\n", cmd.first.c_str(), cmd.second.description.c_str());
    }
  } else {
    telnet.println(F("Unknown command. Type 'help' to see available commands."));
  }
  telnet.print(F("> "));
}

// Example command callbacks
void commandPing(const String& params) {
  telnet.println(F("Pong! Telnet is working."));
}

void commandWiFi(const String& params) {
  telnet.println(F("Wi-Fi Details:"));
  telnet.print(F("  SSID: "));
  telnet.println(WiFi.SSID());
  telnet.print(F("  IP Address: "));
  telnet.println(WiFi.localIP().toString());
  telnet.print(F("  Signal Strength: "));
  telnet.print(WiFi.RSSI());
  telnet.println(F(" dBm"));
}

void commandMemory(const String& params) {
  telnet.println(F("Memory Details"));
  telnet.print(F("  Free heap: "));
  telnet.print(ESP.getFreeHeap());
  telnet.println(F(" bytes"));
  telnet.print(F("  Max alloc block: "));
  telnet.print(ESP.getMaxAllocHeap());
  telnet.println(F(" bytes"));
}

void commandTrace(const String& params) {
  if (params == F("gas") || params == F("water") || params == F("command") || params == F("announce")) {
    trace = params;
    telnet.print(F("Tracing only "));
    telnet.print(params);
    telnet.println(F(" interactions."));
  } else {
    trace = F("all");
    telnet.println(F("Tracing all interactions."));
  }
}

void commandStop(const String& params) {
  trace = F("");
  telnet.println(F("Tracing stopped."));
}

void commandGas(const String& params) {
  telnet.println(gasToJSON(&navienSerial.currentState()->gas));
}

void commandWater(const String& params) {
  bool createArray = navienSerial.currentState()->max_water_devices_seen > 0 ? true:false;
  if (createArray) {
    telnet.println(F("["));
  }
  for (int i = 0; i <= navienSerial.currentState()->max_water_devices_seen; i++) {
    telnet.print(waterToJSON(&navienSerial.currentState()->water[i]));
    if (i != navienSerial.currentState()->max_water_devices_seen) {
      telnet.println(F(","));
    } else {
      telnet.println();
    }
  }
  if (createArray) {
    telnet.println(F("]"));
  }
}

void commandControl(const String& params) {
  if (navienSerial.controlAvailable()) {
    telnet.println(F("Commands can be sent."));
  } else {
    telnet.println(F("Commands cannot be sent."));  // Also fixed typo in message
  }
}

void commandSetTemp(const String& params) {
  float temp;
  if (params.isEmpty()) {
    temp = navienSerial.currentState()->gas.set_temp;
    telnet.printf("Current set temperature: %0.1f°C\n", temp);
  } else {
    temp = params.toFloat();
    int success = -1;
    if (temp > 20.0 || temp < 60.0) {
      success = navienSerial.setTemp(temp);
    }
    if (success < 0) {
      telnet.printf("Failed setting temperature to: %0.1f°C Return code: %d\n", temp, success);
    } else {
      telnet.printf("Set temperature to: %0.1f°C Return code: %d\n", temp, success);
    }
  }
}

void commandPower(const String& params) {
  if (params.isEmpty()) {
    telnet.print(F("Current Power is: "));
    for (int i = 0; i <= navienSerial.currentState()->max_water_devices_seen; i++) {
      if (navienSerial.currentState()->water[i].system_power) {
        telnet.print(F("ON"));
      } else {
        telnet.print(F("OFF"));
      }
      if (i > 0) {
        telnet.print(", ");
      }
    }
    telnet.println();
  } else if (params.equalsIgnoreCase("on")) {
    if (navienSerial.power(true) != -1) {
      telnet.println(F("Powering on."));
    } else {
      telnet.println(F("Failed to power on."));
    }
  } else if (params.equalsIgnoreCase("off")) {
    if (navienSerial.power(false) != -1) {
      telnet.println(F("Powering off."));
    } else {
      telnet.println(F("Failed to power off."));
    }
  } else {
    telnet.printf("Unknown power parameter: %s", params.c_str());
  }
}

void commandRecirc(const String& params) {
  if (params.isEmpty()) {
    telnet.print(F("Recirculation is: "));
    for (int i = 0; i <= navienSerial.currentState()->max_water_devices_seen; i++) {
      if (navienSerial.currentState()->water[i].recirculation_running) {
        telnet.print(F("ON"));
      } else {
        telnet.print(F("OFF"));
      }
            if (i > 0) {
        telnet.print(", ");
      }
    }
    telnet.println();
  } else if (params.equalsIgnoreCase("on")) {
    if (navienSerial.recirculation(true) != -1) {
      telnet.println(F("Turning recirculation on."));
    } else {
      telnet.println(F("Failed to turning recirculation on."));
    }
  } else if (params.equalsIgnoreCase("off")) {
    if (navienSerial.recirculation(false) != -1) {
      telnet.println(F("Turning recirculation off."));
    } else {
      telnet.println(F("Failed to turning recirculation off."));
    }
  } else {
    telnet.printf("Unknown recirculation parameter: %s", params.c_str());
  }
}

void commandHotButton(const String& params) {
  if (navienSerial.hotButton() != -1)
    telnet.println(F("Hot button command sent."));
  else
    telnet.println(F("Hot button command failed."));
}

void commandEraseEve(const String& params) {
    telnet.println("Erasing Eve Program Data...");
    nvs_handle_t savedData;
    esp_err_t err = nvs_open("SAVED_DATA", NVS_READWRITE, &savedData);

    if (err != ESP_OK) {
        telnet.printf("❌ Failed to open NVS: %s\n", esp_err_to_name(err));
        return;
    }

    // Erase the "PROG_SEND_DATA" key
    err = nvs_erase_key(savedData, "PROG_SEND_DATA");
    if (err == ESP_OK) {
        telnet.println("✅ Successfully erased PROG_SEND_DATA.");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        telnet.println("⚠️ Key PROG_SEND_DATA does not exist.");
    } else {
        telnet.printf("❌ Failed to erase key: %s\n", esp_err_to_name(err));
    }

    // Commit the changes
    err = nvs_commit(savedData);
    if (err != ESP_OK) {
        telnet.printf("❌ Commit failed: %s\n", esp_err_to_name(err));
    }

    // Close the handle
    nvs_close(savedData);
    telnet.println("Reboot to pick up changes");
}


void commandTimezone(const String& params) {
    nvs_handle_t nvsStorageHandle;  
    if (params.equalsIgnoreCase("clear")) {
        scheduler->eraseTz();
        telnet.println(F("Time Zone erased"));
    } else if (params.length() == 0) {
        // Print current time zone
        String tz = scheduler->getTz();
        if (tz && !tz.isEmpty()) {
            telnet.printf("Current Time Zone: %s\n", tz.c_str());
        } else {
            telnet.println("No Time Zone is set!");
        }
    } else {
        scheduler->setTz(params);
        telnet.printf("Time Zone set to: %s\n", params.c_str());
    }
}

String getFormattedTimeForValue(time_t value) {    
  time_t now = value;      
    struct tm *timeinfo = localtime(&now); 

    char timeString[64];
    strftime(timeString, sizeof(timeString), "%A, %Y-%m-%d %H:%M", timeinfo); // 24hr format, no seconds, includes weekday

    return String(timeString);
}

void commandScheduler(const String& params) {
  static const char *dayNames[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

  if (params.equalsIgnoreCase("on")) {
    scheduler->setEnabled(true);
    telnet.println(F("Scheduler enabled."));
    return;
  } else if (params.equalsIgnoreCase("off")) {
    scheduler->setEnabled(false);
    telnet.println(F("Scheduler disabled."));
    return;
  } else if (!params.isEmpty()) {
    telnet.printf("Unknown scheduler parameter: %s\n", params.c_str());
    return;
  }

  time_t now = time(nullptr);
  struct tm *localTime = localtime(&now);
  telnet.printf("Current time:     %s", asctime(localTime));
  telnet.printf("Schedule enabled: %s\n", scheduler->enabled() ? "Yes" : "No");
  telnet.printf("Current state:    %s\n", FakeGatoScheduler::getSchedulerState(scheduler->getCurrentState()).c_str());

  if (scheduler->isOverrideActive()) {
    telnet.printf("Override expires: %s\n", getFormattedTimeForValue(scheduler->getOverrideEndTime()).c_str());
  }

  time_t nextTime;
  SchedulerBase::State nextState = scheduler->getNextState(&nextTime);
  if (nextTime > 0) {
    telnet.printf("Next transition:  %s -> %s\n",
      getFormattedTimeForValue(nextTime).c_str(),
      FakeGatoScheduler::getSchedulerState(nextState).c_str());
  } else {
    telnet.println("Next transition:  None scheduled");
  }

  bool tzKnown = (getenv("TZ") != nullptr);
  telnet.println("Weekly schedule:");
  for (int day = 0; day < 7; day++) {
    telnet.printf("  %s:", dayNames[day]);

    struct SlotDisplay {
      int lsh, lsm, leh, lem;
      int sh, sm, eh, em;
      int dayShift;  // -1 = fires previous local day, 0 = same day, +1 = next local day
    } slots[3];
    int slotCount = 0;

    for (int slot = 0; slot < 3; slot++) {
      uint8_t sh, sm, eh, em;
      if (!scheduler->getTimeSlot(day, slot, sh, sm, eh, em)) continue;
      SlotDisplay &s = slots[slotCount++];
      s.sh = sh; s.sm = sm; s.eh = eh; s.em = em;
      s.dayShift = 0;
      if (tzKnown) {
        // Slots are stored in UTC; convert to local using a fixed reference
        // date (Jan 2 1970) so only the hour:min offset matters.
        struct tm ref = {};
        ref.tm_year = 70; ref.tm_mon = 0; ref.tm_mday = 2; ref.tm_sec = 0;
        ref.tm_hour = sh; ref.tm_min = sm;
        time_t ts = proper_timegm(&ref);
        struct tm *ls = localtime(&ts);
        s.lsh = ls->tm_hour; s.lsm = ls->tm_min;
        ref.tm_hour = eh; ref.tm_min = em;
        time_t te = proper_timegm(&ref);
        struct tm *le = localtime(&te);
        s.leh = le->tm_hour; s.lem = le->tm_min;
        // Detect midnight rollover: compare local vs UTC start in minutes.
        int diff = (s.lsh * 60 + s.lsm) - (sh * 60 + sm);
        if (diff >  720) s.dayShift = -1; // local is previous day (e.g. UTC 04:00 → local 21:00)
        if (diff < -720) s.dayShift = +1; // local is next day
      }
    }

    // Sort by local start time (insertion sort; max 3 elements).
    if (tzKnown) {
      for (int i = 1; i < slotCount; i++) {
        SlotDisplay key = slots[i];
        int j = i - 1;
        while (j >= 0 && (slots[j].lsh * 60 + slots[j].lsm) > (key.lsh * 60 + key.lsm)) {
          slots[j + 1] = slots[j];
          j--;
        }
        slots[j + 1] = key;
      }
    }

    for (int i = 0; i < slotCount; i++) {
      SlotDisplay &s = slots[i];
      if (tzKnown) {
        telnet.printf(" %02d:%02d-%02d:%02d (UTC %02d:%02d-%02d:%02d%s)",
                      s.lsh, s.lsm, s.leh, s.lem, s.sh, s.sm, s.eh, s.em,
                      s.dayShift == -1 ? ", prev day" : s.dayShift == +1 ? ", next day" : "");
      } else {
        telnet.printf(" %02d:%02d-%02d:%02d (UTC)", s.sh, s.sm, s.eh, s.em);
      }
    }
    if (slotCount == 0) telnet.print(" (none)");
    telnet.println();
  }
}

void commandHistory(const String& params) {
  if (!historyService) {
    telnet.println(F("Error: History service not available"));
    return;
  }
  // Parse length parameter (default to all entries if not specified)
  int length = historyService->store.usedMemory;
  if (params.length() > 0) {
    length = min((int)params.toInt(), (int)historyService->store.usedMemory);
  }
  telnet.println(F("Time,CurrentTemp,TargetTemp,ValvePercent,ThermoTarget,OpenWindow"));
  
  // Calculate starting entry based on length
  int firstEntry = historyService->store.firstEntry;
  int lastEntry = historyService->store.lastEntry;
  int startEntry = max(lastEntry - length, firstEntry);
  
  // Output entries in CSV format
  for (int i = startEntry; i <= lastEntry; i++) {
    auto entry = historyService->store.history[i % historyService->store.historySize];
    telnet.printf("%s, %.2f,%.2f,%d,%d,%d\n",
      getFormattedTimeForValue(entry.time).c_str(),
      entry.currentTemp / 100.0,
      entry.targetTemp / 100.0,
      entry.valvePercent,
      entry.thermoTarget,
      entry.openWindow
    );
  }
}

void commandEraseHistory(const String& params) {
    if (!historyService) {
    telnet.println(F("Error: History service not available"));
    return;
  }

  historyService->eraseHistory();
  telnet.println(F("History erased"));
}

void commandTime(const String& params) {
  time_t now = time(nullptr);

    struct tm *localTime = localtime(&now);
    telnet.printf("Local Time: %s", asctime(localTime));

    struct tm *gmtTime = gmtime(&now);
    telnet.printf("GMT/UTC Time: %s", asctime(gmtTime));
}

void commandfsStat(const String& params) {
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();

  size_t freeBytes = totalBytes - usedBytes;

  telnet.printf("LittleFS Partition Info:\n");
  telnet.printf("Total Size: %u bytes\n", totalBytes);
  telnet.printf("Used Size: %u bytes\n", usedBytes);
  telnet.printf("Free Space: %u bytes\n", freeBytes);
}

void commandLearnerStatus(const String& params) {
  static const char *dayNames[] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
  };

  if (!learner || learner->isDisabled()) {
    telnet.println(F("Learner is disabled or not initialized."));
    return;
  }

  telnet.println(F("Learner Status"));

  // Last recompute time and age.
  time_t lastRecompute = learner->lastRecomputeTime();
  if (lastRecompute > 0) {
    char tbuf[32];
    struct tm  tm_buf;
    struct tm *t = localtime_r(&lastRecompute, &tm_buf);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", t);
    time_t elapsed = time(nullptr) - lastRecompute;
    if (elapsed >= 3600) {
      telnet.printf("  Last recompute:  %s  (%dh ago)\n", tbuf, (int)(elapsed / 3600));
    } else {
      telnet.printf("  Last recompute:  %s  (%dmin ago)\n", tbuf, (int)(elapsed / 60));
    }
  } else {
    telnet.println(F("  Last recompute:  never"));
  }

  // Bucket fill.
  int nonZero = learner->bucketStore().nonZeroCount();
  int total   = BUCKET_DAYS * BUCKET_PER_DAY;
  telnet.printf("  Bucket fill:     %d / %d non-zero (%.1f%%)\n\n",
                nonZero, total, nonZero * 100.0f / total);

  // Per-day table header.
  telnet.println(F("  Day         Predicted  Measured   Gap      Cold-starts (4wk)"));
  telnet.println(F("  -----------------------------------------------------------------"));

  const float       *pred = learner->predictedEfficiency();
  const WeekMeasured *mw  = learner->measuredWindow();

  float sumPred = 0.0f, sumMeas = 0.0f;
  int   cntPred = 0,    cntMeas = 0;

  for (int dow = 0; dow < 7; dow++) {
    uint32_t tot = 0, cov = 0;
    for (int w = 0; w < 4; w++) {
      tot += mw[w].total[dow];
      cov += mw[w].covered[dow];
    }
    float measPct = (tot > 0) ? (cov * 100.0f / tot) : NAN;
    float predPct = pred[dow];

    char predStr[12], measStr[12], gapStr[12];
    if (!isnan(predPct)) {
      snprintf(predStr, sizeof(predStr), "%6.1f%%", predPct);
      sumPred += predPct;
      cntPred++;
    } else {
      snprintf(predStr, sizeof(predStr), "    N/A");
    }
    if (!isnan(measPct)) {
      snprintf(measStr, sizeof(measStr), "%6.1f%%", measPct);
      sumMeas += measPct;
      cntMeas++;
    } else {
      snprintf(measStr, sizeof(measStr), "    N/A");
    }
    if (!isnan(predPct) && !isnan(measPct)) {
      snprintf(gapStr, sizeof(gapStr), "%+7.1f%%", predPct - measPct);
    } else {
      snprintf(gapStr, sizeof(gapStr), "     N/A");
    }

    telnet.printf("  %-11s  %s   %s   %s   %u\n",
                  dayNames[dow], predStr, measStr, gapStr, (unsigned)tot);
  }

  telnet.println(F("  -----------------------------------------------------------------"));
  char avgPred[12] = "    N/A", avgMeas[12] = "    N/A";
  if (cntPred > 0) snprintf(avgPred, sizeof(avgPred), "%6.1f%%", sumPred / cntPred);
  if (cntMeas > 0) snprintf(avgMeas, sizeof(avgMeas), "%6.1f%%", sumMeas / cntMeas);
  telnet.printf("  %-11s  %s   %s\n", "Weekly avg", avgPred, avgMeas);
}

void commandSaveLearner(const String& params) {
  if (!learner || learner->isDisabled()) {
    telnet.println(F("Learner is disabled or not initialized."));
    return;
  }
  if (learner->saveMeasured()) {
    telnet.println(F("Measured efficiency window saved."));
  } else {
    telnet.println(F("Failed to save measured efficiency window."));
  }
}

void commandReboot(const String& params) {
  telnet.println(F("Rebooting system..."));
  telnet.disconnectClient();
  ESP.restart();
}

void commandBye(const String& params) {
  telnet.println(F("Goodbye"));
  telnet.disconnectClient();
}

// Register all commands in setup
void setupTelnetCommands() {
  telnet.stop(); // Stop it if it is already running

  registerCommand(F("ping"), F("Test if telnet commands are working"), commandPing);
  registerCommand(F("wifi"), F("Print WiFi status"), commandWiFi);
  registerCommand(F("memory"), F("Print available memory"), commandMemory);

  registerCommand(F("trace"), F("Dump interactions (options: gas/water/command/announce)"), commandTrace);
  registerCommand(F("stop"), F("Stop tracing"), commandStop);

  registerCommand(F("gas"), F("Print current gas state as JSON"), commandGas);
  registerCommand(F("water"), F("Print current water state as JSON"), commandWater);
  registerCommand(F("control"), F("Check if control commands are available"), commandControl);

  registerCommand(F("setTemp"), F("Set or get set point temperature"), commandSetTemp);
  registerCommand(F("power"), F("Set or get power state (on/off)"), commandPower);
  registerCommand(F("recirc"), F("Set or get recirculation state (on/off)"), commandRecirc);
  registerCommand(F("hotButton"), F("Send hot button command"), commandHotButton);

  registerCommand(F("scheduler"), F("Show scheduler status or enable/disable it (on/off)"), commandScheduler);
  registerCommand(F("timezone"), F("Set or get current timezone"), commandTimezone);
  registerCommand(F("time"), F("Print local and gmt time"), commandTime);
  registerCommand(F("erasePgm"), F("Erase all Program State"), commandEraseEve);

  registerCommand(F("learnerStatus"), F("Print schedule learner status and efficiency table"), commandLearnerStatus);
  registerCommand(F("saveLearner"), F("Save measured efficiency window to flash"), commandSaveLearner);
  registerCommand(F("history"), F("Print history entries in CSV format (optional: number of entries)"), commandHistory);
  registerCommand(F("eraseHistory"), F("Erase all history entries"), commandEraseHistory);
  registerCommand(F("fsStat"), F("File system status"), commandfsStat);
  
  registerCommand(F("reboot"), F("Reboot ESP32"), commandReboot);
  registerCommand(F("bye"), F("Disconnect"), commandBye);

  // passing on functions for various telnet events
  telnet.onConnect(onTelnetConnect);
  telnet.onDisconnect(onTelnetDisconnect);
  telnet.onInputReceived(onTelnetInput);  // Register Telnet input callback

  telnet.begin(23);
  Serial.println(F("Telnet server started"));

}