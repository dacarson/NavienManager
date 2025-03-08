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
#include <ESPTelnet.h>
#include <ArduinoOTA.h>
#include <nvs_flash.h>
#include "HomeSpan.h"


#include "Navien.h"
#include "FakeGatoScheduler.h"

bool wifiConnected = false;

Navien navienSerial(2);
#define RXD2 16
#define TXD2 17

ESPTelnet telnet;
extern void setupTelnetCommands();   // TelnetCommands.ino

extern void setupNavienCallbacks();  // NavienBroadcaster.ino

extern void setupOTA();              // OTASupport.ino

extern FakeGatoScheduler* setupHomeSpanAccessories();
extern void setupHomeSpanHistory();

FakeGatoScheduler* scheduler;

String getNextTransitionTime() {
  time_t nextStateTime;
  scheduler->getNextState(&nextStateTime);

    if (nextStateTime == 0) {
        return "No upcoming transition";
    }

    struct tm *tm_struct = localtime(&nextStateTime);
    String formattedTime = String(tm_struct->tm_hour) + ":" +
                           (tm_struct->tm_min < 10 ? "0" : "") + String(tm_struct->tm_min) +
                           " " + String(tm_struct->tm_mon + 1) + "/" + String(tm_struct->tm_mday) +
                           "/" + String(tm_struct->tm_year + 1900);
    
    return formattedTime;
}

String getNextTransitionState() {
  time_t nextStateTime;
  int state = scheduler->getNextState(&nextStateTime);
  
  if (nextStateTime == 0) {
      return "N/A";
  }

    return FakeGatoScheduler::getSchedulerState(state);
}

String statusCard(const char* title, String value, const char* cssClass) {
    return "<div class='status-card'><div class='status-header'>" + 
           String(title) + 
           "</div><div class='status-value " + 
           String(cssClass) + 
           "'>" + value + "</div></div>";
}

String getFormattedTime() {
    time_t now = time(nullptr);            
    struct tm *timeinfo = localtime(&now); 

    char timeString[64];
    strftime(timeString, sizeof(timeString), "%A, %Y-%m-%d %H:%M", timeinfo); // 24hr format, no seconds, includes weekday

    return String(timeString);
}

void navienStatus(String &html) {
  float setTemp = navienSerial.currentState()->gas.set_temp;
  float outletTemp = navienSerial.currentState()->gas.outlet_temp;
  float inletTemp = navienSerial.currentState()->gas.inlet_temp;

  uint16_t currentGasUsage = navienSerial.currentState()->gas.current_gas_usage; 
  float accumulatedGasUsage = navienSerial.currentState()->gas.accumulated_gas_usage;

  float domesticFlowRate = navienSerial.currentState()->water.flow_lpm;

  html = "<h3>Controller: "
    + String(navienSerial.currentState()->gas.controller_version)
    + " Panel: "
    + String(navienSerial.currentState()->gas.panel_version)
    + "</h3>";


  html += "<div class='status-container'>"
    + statusCard("Hotwater Power", (String(navienSerial.currentState()->water.system_power ? "On":"Off" )), (navienSerial.currentState()->water.system_power ? "status-ok" : "status-error"))
    + statusCard("Domestic Consumption", String(navienSerial.currentState()->water.consumption_active ? "Yes":"No"), (navienSerial.currentState()->water.consumption_active ? "status-warning" :"status-ok"))

    + statusCard("Current Gas Usage", (String(currentGasUsage) + " kcal / " + String(3.96567 * currentGasUsage) + " BTU"), (currentGasUsage > 0 ? "status-warning" :"status-ok") )
    + statusCard("Accumulated Gas", (String(accumulatedGasUsage) + " m<sup>3</sup> / " + String(accumulatedGasUsage * 35315.0 / 100000) + " Therms"), "status-ok")

    + statusCard("Domestic Outlet Set Temp", (String(setTemp) + " &deg;C / " + String((setTemp * 9.0 / 5.0) + 32) + " &deg;F"), "status-ok")
    + statusCard("Domestic Outlet Temp", (String(outletTemp) + " &deg;C / " + String((outletTemp * 9.0 / 5.0) + 32) + " &deg;F"), "status-ok")

    + statusCard("Inlet Temp", (String(inletTemp) + " &deg;C / " + String((inletTemp * 9.0 / 5.0) + 32) + " &deg;F"), "status-ok")
    + statusCard("Domestic Flow Rate", (String(domesticFlowRate) + " lpm / " + String(domesticFlowRate * 0.264172) + " GPM"), domesticFlowRate > 0 ? "status-warning": "status-ok")

    + statusCard("Total Operating Time", (String(navienSerial.currentState()->gas.total_operating_time / 60.0) + " hr"), "status-ok")
    + statusCard("Accumulated Usage Cnt", String(navienSerial.currentState()->gas.accumulated_domestic_usage_cnt), "status-ok")

    + statusCard("Navien Scheduler", String(navienSerial.currentState()->water.schedule_active ? "Active":"Inactive" ), (navienSerial.currentState()->water.schedule_active ? "status-warning" :"status-ok"))
    + statusCard("Operating Capacity", (String(navienSerial.currentState()->water.operating_capacity) + " %"), (navienSerial.currentState()->water.operating_capacity > 0 ? "status-warning" :"status-ok"))

    + statusCard("Recirculation", String(navienSerial.currentState()->water.recirculation_active ? "Active":"Inactive" ), (navienSerial.currentState()->water.recirculation_active ? "status-warning" :"status-ok"))
    + statusCard("Recirculation Pump", String(navienSerial.currentState()->water.recirculation_running ? "Running":"Stopped"), (navienSerial.currentState()->water.recirculation_running ? "status-warning" :"status-ok"))
    + "</div>";

    html += "<h2>Scheduler Status</h2>"
    "<h3>Device Time: <span id='deviceTime'>" + getFormattedTime() + "</span></h3>"
    + "<div class='status-container'>"
    + statusCard("Scheduler Enabled", String(scheduler->enabled() ? "Yes" : "No"), scheduler->enabled() ? "status-ok" : "status-warning")
    + statusCard("Current State", getSchedulerState(scheduler->getCurrentState()), "status-ok")
    + statusCard("Next Transition", getNextTransitionTime(), scheduler->enabled() ? "status-ok" : "status-warning")
    + statusCard("Next State", getNextTransitionState(), scheduler->enabled() ? "status-ok" : "status-warning")
    + "</div>";

    html += "<h2>System Log</h2>"
    "<style>.tab2{margin: 0 auto;width:80%;border-collapse:collapse;background:#2a2a3a;color:white;}</style>";

String footerScript = 
    "<script>"
    "document.addEventListener('DOMContentLoaded',()=>{"
    "let table=document.querySelector('.tab2');"
    "if(table){"
    "let footer=document.createElement('div');"
    "footer.className='footer';"
    "footer.innerHTML='Updated: <span id=\"timestamp\">Loading...</span>';"
    "table.parentNode.insertBefore(footer,table.nextSibling);"
    "function updateTimestamp(){"
    "document.getElementById('timestamp').innerText=new Date().toLocaleString();}"
    "updateTimestamp();setInterval(updateTimestamp,6e4);"
    "setInterval(()=>location.reload(),6e4);}" // <-- Auto-reload every 60s
    "});"
    "</script>";

    html += footerScript;

}

void myWiFiBegin(const char *s, const char *p) {
  WiFi.begin(s, p);
  Serial.println(F(""));
  Serial.println(F("Connecting to WiFi "));
}

void onWifiConnected(int connection) {
  // Setup callbacks for UDP broadcast
  setupNavienCallbacks();
  Serial.println(F("UDP Broadcast started"));

  // Start Telnet server on port 23
  setupTelnetCommands();
  telnet.begin(23);
  Serial.println(F("Telnet server started"));

  // Setup OTA
  setupOTA();
  ArduinoOTA.begin();
  Serial.println(F("OTA Started"));

  wifiConnected = true;
} 

void setup() {
  Serial.begin(115200);

  // Start Serial 2 for Navien
  navienSerial.begin(RXD2, TXD2);
  Serial.println(F("Navien Serial Started"));

  // setup HomeSpan. It needs to own WiFi setup so
  // that it can do the pairing.
  homeSpan.setWifiCredentials(ssid, password);
  homeSpan.setWifiBegin(myWiFiBegin);
  homeSpan.setConnectionCallback(onWifiConnected);
  homeSpan.begin(Category::Thermostats,"Navien");
  homeSpan.enableWebLog(50);
  homeSpan.setWebLogCallback(navienStatus);
  String cssStyle = 
  "body{font-family:Arial,sans-serif;background:#1e1e2f;color:#fff;margin:0;padding:20px;text-align:center}"
  "th{font-size:18px;font-weight:bold;text-align:left;}tr{font-size:16px;}"
  ".tab1 {display:none} .bod1 h2:first-of-type::after {content:\" Status\"}" // Hide the built-in table (tab1) and add "Status" to the first h2 heading
  "h2{font-size:28px;color:#f8c537;margin-bottom:10px}"
  "h3{font-size:14px;color:white;margin-bottom:10px}"
  ".status-container{display:grid;grid-template-columns:repeat(2, 1fr);gap:15px;max-width: 800px;margin:auto;}"
  ".status-card{background:#2a2a3a;padding:15px 20px;border-radius:10px;width:80%;max-width:400px;box-shadow:0 4px 10px rgba(0,0,0,.3);display:flex;justify-content:space-between;align-items:center;"
    ".status-header{font-size:18px;font-weight:bold;flex:1;text-align:left;}"
    ".status-value{font-size:16px;font-weight:bold;flex-shrink:0;text-align:right;}"
    ".status-ok{color:#28a745}.status-warning{color:#ffc107}.status-error{color:#dc3545}"
  "}"
  ".footer{margin-top:20px;font-size:14px;opacity:.7}";
  homeSpan.setWebLogCSS(cssStyle.c_str());
  scheduler = setupHomeSpanAccessories();
  Serial.println(F("HomeSpan Started"));
}

void loop() {
  if (wifiConnected) {
  ArduinoOTA.handle();
  telnet.loop();
  }

  navienSerial.loop();
  homeSpan.poll();
}
