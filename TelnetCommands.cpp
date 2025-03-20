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

ESPTelnet telnet;
extern Navien navienSerial;
extern FakeGatoHistoryService *historyService;
extern FakeGatoScheduler *scheduler;
String trace;

// Functions in NavienBroadcaster.ino
extern String waterToJSON(const Navien::NAVIEN_STATE *state, String rawhexstring = "");
extern String gasToJSON(const Navien::NAVIEN_STATE *state, String rawhexstring = "");


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
  telnet.println(gasToJSON(navienSerial.currentState()));
}

void commandWater(const String& params) {
  telnet.println(waterToJSON(navienSerial.currentState()));
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
    if (navienSerial.currentState()->water.system_power) {
      telnet.println(F("ON"));
    } else {
      telnet.println(F("OFF"));
    }
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
    if (navienSerial.currentState()->water.recirculation_running) {
      telnet.println(F("ON"));
    } else {
      telnet.println(F("OFF"));
    }
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

void eraseStoredTimezone() {
  nvs_handle_t nvsStorageHandle;  
    esp_err_t status = nvs_open("SCHEDULER", NVS_READWRITE, &nvsStorageHandle);
    if (status != ESP_OK) {
        telnet.printf("❌ Failed to open NVS: %s\n", esp_err_to_name(status));
        return;
    }

    status = nvs_erase_key(nvsStorageHandle, "TZ");  // ✅ Erase the stored time zone
    if (status == ESP_OK) {
        telnet.println("✅ Time zone erased from NVS.");
    } else if (status == ESP_ERR_NVS_NOT_FOUND) {
        telnet.println("⚠️ No stored time zone found.");
    } else {
        telnet.printf("❌ Failed to erase key: %s\n", esp_err_to_name(status));
    }

    nvs_commit(nvsStorageHandle);  // ✅ Commit changes
    nvs_close(nvsStorageHandle);   // ✅ Close handle

    unsetenv("TZ");  // ✅ Unset TZ from system
    tzset();
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

  registerCommand(F("trace"), F("Dump interactions (options: gas/water/command/announce)"), commandTrace);
  registerCommand(F("stop"), F("Stop tracing"), commandStop);

  registerCommand(F("gas"), F("Print current gas state as JSON"), commandGas);
  registerCommand(F("water"), F("Print current water state as JSON"), commandWater);
  registerCommand(F("control"), F("Check if control commands are available"), commandControl);

  registerCommand(F("setTemp"), F("Set or get set point temperature"), commandSetTemp);
  registerCommand(F("power"), F("Set or get power state (on/off)"), commandPower);
  registerCommand(F("recirc"), F("Set or get recirculation state (on/off)"), commandRecirc);
  registerCommand(F("hotButton"), F("Send hot button command"), commandHotButton);

  registerCommand(F("timezone"), F("Set or get current timezone"), commandTimezone);
  registerCommand(F("time"), F("Print local and gmt time"), commandTime);
  registerCommand(F("erasePgm"), F("Erase all Program State"), commandEraseEve);

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