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

bool wifiConnected = false;

Navien navienSerial(2);
#define RXD2 16
#define TXD2 17

extern ESPTelnet telnet;
extern void setupTelnetCommands();   // TelnetCommands.ino
extern void setupNavienBroadcaster();  // NavienBroadcaster.ino
extern void setupHomeSpanWeb(); // HomeSpanWeb.ino
extern void setupHomeSpanAccessories();

void myWiFiBegin(const char *s, const char *p) {
  WiFi.begin(s, p);
  Serial.println(F(""));
  Serial.println(F("Connecting to WiFi "));
}

void onWifiConnected(int connection) {
  // Setup callbacks for UDP broadcast
  setupNavienBroadcaster();

  // Start Telnet server on port 23
  setupTelnetCommands();

  wifiConnected = true;
}

void setup() {
  Serial.begin(115200);

  // Start Serial 2 for Navien
  navienSerial.begin(RXD2, TXD2);
  Serial.println(F("Navien Serial Started"));

  // setup HomeSpan. It needs to own WiFi setup so
  // that it can do the pairing.
  //homeSpan.setWifiCredentials(ssid, password);
  homeSpan.setWifiBegin(myWiFiBegin);
  homeSpan.setConnectionCallback(onWifiConnected);
  homeSpan.begin(Category::Thermostats,"Navien Manager");
  homeSpan.enableOTA(false, false);

  setupHomeSpanWeb(); // Setup the homespan webpage

  setupHomeSpanAccessories();
  Serial.println(F("HomeSpan Started"));
}

void loop() {
  if (wifiConnected) {
  telnet.loop();
  }

  navienSerial.loop();
  homeSpan.poll();
}
