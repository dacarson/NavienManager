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

#include <WiFi.h>
#include <LittleFS.h>
#include <ESPTelnet.h>
#include <nvs_flash.h>
#include "HomeSpan.h"

#include "Navien.h"
#include "NavienLearner.h"

bool wifiConnected = false;
bool timeInit = false;

Navien navienSerial(2);
NavienLearner *learner = nullptr;
#define RXD2 16
#define TXD2 17

extern ESPTelnet telnet;
extern void setupTelnetCommands();   // TelnetCommands.ino
extern void setupNavienBroadcaster();  // NavienBroadcaster.ino
extern void setupHomeSpanWeb(); // HomeSpanWeb.ino
extern void setupHomeSpanAccessories();
extern void setupScheduleEndpoint(); // ScheduleEndpoint.ino
extern void loopScheduleEndpoint();

void myWiFiBegin(const char *s, const char *p) {
  WiFi.begin(s, p);
  Serial.println(F(""));
  Serial.println(F("Connecting to WiFi "));
}

void onWifiConnected(int connection) {
  // HomeSpan fires this callback on L2 association, before DHCP completes.
  // Bail out if we don't have a valid IP yet.
  if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    return;
  }

  // Only run setup once; on reconnects the servers are already running.
  if (!wifiConnected) {
    // Setup callbacks for UDP broadcast
    setupNavienBroadcaster();

    // Start schedule HTTP endpoint on port 8080
    setupScheduleEndpoint();

    // Start Telnet server on port 23
    setupTelnetCommands();

    wifiConnected = true;
  }
}

void setup() {
  Serial.begin(115200);

  // Start Serial 2 for Navien
  navienSerial.setRxBufferSize(1024);  // Expand the receive buffer size to 1024 bytes
  navienSerial.begin(RXD2, TXD2);
  Serial.println(F("Navien Serial Started"));

  // Learner is independent of HomeSpan/HomeKit and starts unconditionally.
  learner = new NavienLearner();
  learner->begin();

  // setup HomeSpan. It needs to own WiFi setup so
  // that it can do the pairing.
  //homeSpan.setWifiCredentials(ssid, password);
  homeSpan.setWifiBegin(myWiFiBegin);
  homeSpan.setConnectionCallback(onWifiConnected);
  homeSpan.setHostNameSuffix("Controller");
  homeSpan.begin(Category::Thermostats,"Navien Manager");
  homeSpan.enableOTA(false, false);
  homeSpan.setStatusCallback([](HS_STATUS status) {
    if (status == HS_OTA_STARTED && learner && !learner->isDisabled()) {
      learner->saveMeasured();
    }
  });

  setupHomeSpanWeb(); // Setup the homespan webpage

  setupHomeSpanAccessories();
  Serial.println(F("HomeSpan Started"));
}

void loop() {
  if (wifiConnected) {
    telnet.loop();
    loopScheduleEndpoint();
  }

  // Check for system clock setup, which the SchedulerBase class does
  if (!timeInit && getenv("TZ")){
    struct tm localTime;
    if (getLocalTime(&localTime)) { // getLocalTime() returns non-zero if initialized
      timeInit = true;
      homeSpan.assumeTimeAcquired();
    }
  }

  navienSerial.loop();
  homeSpan.poll();
}
