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

#include "HomeSpan.h"

#include "FakeGatoScheduler.h"
extern FakeGatoScheduler* scheduler;

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

String statusCard(const char* title, const char* cssClass, String value1, String value2 = "") {
    String html = "<div class='status-card'><div class='status-header'>";
    html += title;
    html += "</div><div class='status-value ";
    html += cssClass;
    html += "'>";

    if (value2.length() > 0) {  // If a second value is provided (metric/imperial toggle)
        html += "<span class='metric'>" + value1 + "</span>";
        html += "<span class='imperial'>" + value2 + "</span>";
    } else {  // If only one value is provided, show it normally
        html += value1;
    }

    html += "</div></div>";
    return html;
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

  char buffer[25];
  float domesticFlowRate = navienSerial.currentState()->water[0].flow_lpm;

  html = "<h3>Controller: "
    + String(navienSerial.currentState()->gas.controller_version)
    + " Panel: "
    + String(navienSerial.currentState()->gas.panel_version)
    + "</h3>";

  html += "<div class='toggle-container'>"
    "<label class='switch'>"
    "<input type='checkbox' id='unitToggle'>"
    "<span class='slider'></span>"
    "</label><span class='toggle-label'>Units</span></div>";


  html += "<div class='status-container'>";
  for (int i = 0; i <= navienSerial.currentState()->max_water_devices_seen; i++) {
    if (i == 0) {
      html += statusCard("Hotwater Power", (navienSerial.currentState()->water[i].system_power ? "status-ok" : "status-error"), String(navienSerial.currentState()->water[i].system_power ? "On":"Off" ));
      html += statusCard("Domestic Consumption", (navienSerial.currentState()->water[i].consumption_active ? "status-warning" :"status-ok"), String(navienSerial.currentState()->water[i].consumption_active ? "Yes":"No"));
    } else {
      sprintf(buffer, "Hotwater Power [%d]", i);
      html += statusCard(buffer, (navienSerial.currentState()->water[i].system_power ? "status-ok" : "status-error"), String(navienSerial.currentState()->water[i].system_power ? "On":"Off" ));
      sprintf(buffer, "Domestic Consumption [%d]", i);
      html += statusCard(buffer, (navienSerial.currentState()->water[i].consumption_active ? "status-warning" :"status-ok"), String(navienSerial.currentState()->water[i].consumption_active ? "Yes":"No"));
    }
  }

    html += statusCard("Current Gas Usage", currentGasUsage > 0 ? "status-warning" :"status-ok", String(currentGasUsage) + " kcal", String(3.96567 * currentGasUsage) + " BTU" );
    html += statusCard("Accumulated Gas", "status-ok", String(accumulatedGasUsage) + " m<sup>3</sup>", String(accumulatedGasUsage * 35315.0 / 100000) + " Therms" );

    html += statusCard("Domestic Outlet Set Temp",  "status-ok", String(setTemp) + " &deg;C", String((setTemp * 9.0 / 5.0) + 32) + " &deg;F");
    html += statusCard("Domestic Outlet Temp", "status-ok", String(outletTemp) + " &deg;C", String((outletTemp * 9.0 / 5.0) + 32) + " &deg;F");

    html += statusCard("Inlet Temp", "status-ok", String(inletTemp) + " &deg;C", String((inletTemp * 9.0 / 5.0) + 32) + " &deg;F");
    html += statusCard("Domestic Flow Rate", domesticFlowRate > 0 ? "status-warning": "status-ok", String(domesticFlowRate) + " lpm", String(domesticFlowRate * 0.264172) + " GPM");

    html += statusCard("Total Operating Time", "status-ok", (String(navienSerial.currentState()->gas.total_operating_time / 60.0) + " hr"));
    html += statusCard("Accumulated Usage Cnt", "status-ok", String(navienSerial.currentState()->gas.accumulated_domestic_usage_cnt));

  for (int i = 0; i <= navienSerial.currentState()->max_water_devices_seen; i++) {
    if (i == 0) {
      html += statusCard("Navien Scheduler", navienSerial.currentState()->water[i].schedule_active ? "status-warning" :"status-ok", navienSerial.currentState()->water[i].schedule_active ? "Active":"Inactive");
      html += statusCard("Operating Capacity", navienSerial.currentState()->water[i].operating_capacity > 0 ? "status-warning" :"status-ok", (String(navienSerial.currentState()->water[i].operating_capacity) + " %"));

      html += statusCard("Recirculation", navienSerial.currentState()->water[i].recirculation_active ? "status-warning" :"status-ok", navienSerial.currentState()->water[i].recirculation_active ? "Active":"Inactive");
      html += statusCard("Recirculation Pump", navienSerial.currentState()->water[i].recirculation_running ? "status-warning" :"status-ok", navienSerial.currentState()->water[i].recirculation_running ? "Running":"Stopped");
    } else {
      sprintf(buffer, "Navien Scheduler [%d]", i);
      html += statusCard(buffer, navienSerial.currentState()->water[i].schedule_active ? "status-warning" :"status-ok", navienSerial.currentState()->water[i].schedule_active ? "Active":"Inactive");
      sprintf(buffer, "Operating Capacity [%d]", i);
      html += statusCard(buffer, navienSerial.currentState()->water[i].operating_capacity > 0 ? "status-warning" :"status-ok", (String(navienSerial.currentState()->water[i].operating_capacity) + " %"));

      sprintf(buffer, "Recirculation [%d]", i);
      html += statusCard(buffer, navienSerial.currentState()->water[i].recirculation_active ? "status-warning" :"status-ok", navienSerial.currentState()->water[i].recirculation_active ? "Active":"Inactive");
      sprintf(buffer, "Recirculation Pump", i);
      html += statusCard(buffer, navienSerial.currentState()->water[i].recirculation_running ? "status-warning" :"status-ok", navienSerial.currentState()->water[i].recirculation_running ? "Running":"Stopped");
    }
  }
    html += "</div>";

    html += "<h2>Scheduler Status</h2>"
    "<h3>Device Time: <span id='deviceTime'>" + getFormattedTime() + "</span></h3>"
    + "<div class='status-container'>"
    + statusCard("Scheduler Enabled", scheduler->enabled() ? "status-warning" : "status-ok", scheduler->enabled() ? "Yes" : "No")
    + statusCard("Current State", (scheduler->getCurrentState() == FakeGatoScheduler::Active || scheduler->getCurrentState() == FakeGatoScheduler::Override) ? "status-warning" : "status-ok", FakeGatoScheduler::getSchedulerState(scheduler->getCurrentState()))
    + statusCard("Next Transition", scheduler->enabled() ? "status-warning" : "status-ok", getNextTransitionTime())
    + statusCard("Next State", "status-ok", getNextTransitionState())
    + "</div>";

    html += "<h2>System Log</h2>"
    "<style>.tab2{margin: 0 auto;width:80%;border-collapse:collapse;background:#2a2a3a;color:white;}</style>";

String footerScript = 
    "<script>"
      "document.addEventListener('DOMContentLoaded',()=>{"
        "const toggle = document.getElementById('unitToggle');"
        "if (localStorage.getItem('unitToggle') === 'imperial') {"
          "toggle.checked = true;"
          "document.body.classList.add('imperial-mode'); }"
        "toggle.addEventListener('change', function () {"
          "if (toggle.checked) {"
            "localStorage.setItem('unitToggle', 'imperial');"
            "document.body.classList.add('imperial-mode');"
          "} else {"
            "localStorage.setItem('unitToggle', 'metric');"
            "document.body.classList.remove('imperial-mode');"
        "} });"
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

void setupHomeSpanWeb() {
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
  ".footer{margin-top:20px;font-size:14px;opacity:.7}"
  ".toggle-container{display:flex;align-items:center;gap:10px;justify-content:center;margin-bottom:10px}"
  ".toggle-label{font-size:16px;font-weight:bold;color:white}"
  ".switch input { display: none; }"
  ".switch { position: relative; width: 50px; height: 24px; display: inline-block; }"
  ".slider { position: absolute; cursor: pointer; inset: 0; background: #ccc; border-radius: 24px; transition: 0.4s; }"
  ".slider::before { content: ""; position: absolute; width: 18px; height: 18px; left: 3px; bottom: 3px; background: white; border-radius: 50%; transition: 0.4s; }"
  "input:checked + .slider { background: #4CAF50; }"
  "input:checked + .slider::before { transform: translateX(26px); }"
  ".metric {display: inline;}"
  ".imperial {display: none;}"
  "body.imperial-mode .metric {display: none;}"
  "body.imperial-mode .imperial {display: inline;}";
  homeSpan.setWebLogCSS(cssStyle.c_str());
}