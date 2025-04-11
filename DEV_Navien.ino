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
#include "esp_mac.h"  // required - exposes esp_mac_type_t values
#include "FakeGatoScheduler.h"
#include "FakeGatoHistoryService.h"
extern Navien navienSerial;

// Global so that Telnet can dump history state.
FakeGatoHistoryService *historyService;
FakeGatoScheduler *scheduler;

CUSTOM_CHAR(ValvePosition, E863F12E-079E-48FF-8F27-9C2605A29F52, PR+EV, UINT8, 0, 0, 100, true);
CUSTOM_CHAR_DATA(ProgramCommand, E863F12C-079E-48FF-8F27-9C2605A29F52, PW + EV);

Characteristic::FirmwareRevision *firmwareRevision;
Characteristic::HardwareRevision *hardwareRevision;

struct DEV_Navien : Service::Thermostat {

  enum {
    OFF = 0,
    HEAT = 1,
    COOL = 2,
    AUTO = 3
  };  // Target States

  enum {
    CELSIUS = 0,
    FAHRENHEIT = 1
  };  // Display Units

  enum {
    IDLE = 0,
    HEATING = 1,
    COOLING = 2
  };  // Current States


  // Declare the Themo characteristics
  Characteristic::CurrentHeatingCoolingState *currentState;
  Characteristic::TargetHeatingCoolingState *targetState;
  Characteristic::CurrentTemperature *currentTemp;
  Characteristic::TargetTemperature *targetTemp;
  Characteristic::ProgramMode programMode;
  Characteristic::TemperatureDisplayUnits displayUnits{ 0, true };

  // Declare the Custom characteristics
  Characteristic::ProgramCommand *programCommand;
  Characteristic::ValvePosition *valvePosition;

  bool accessoryInfoSet = false;

  DEV_Navien()
    : Service::Thermostat() {
    Serial.printf("\n*** Creating Navien Thermostat***\n");

    // Current Heating Cooling State (Read-Only)
    currentState = new Characteristic::CurrentHeatingCoolingState();

    // Target Heating Cooling State (User Selectable)
    targetState = new Characteristic::TargetHeatingCoolingState(OFF);  // Default: Off
    targetState->setValidValues(OFF, HEAT);

    // Current Temperature
    currentTemp = new Characteristic::CurrentTemperature(45.0);  // Default 22°C

    // Target Temperature
    targetTemp = new Characteristic::TargetTemperature(40.0);     // Default 40°C
    targetTemp->setRange(Navien::TEMPERATURE_MIN, Navien::TEMPERATURE_MAX, 0.5);  // Set min/max/step values

    valvePosition = new Characteristic::ValvePosition(0);
    valvePosition->setDescription("Actuation");
    valvePosition->setUnit("percentage");

    programCommand = new Characteristic::ProgramCommand();
    // Create the scheduler class that will handle schedule management
    // and operations.
    scheduler = new FakeGatoScheduler();
    scheduler->begin();

    // Create a history service
    historyService = new FakeGatoHistoryService();
  }

  boolean update() override {

    int ret = 0;

    if (targetState->updated()) {
      // Check to see if the user overrides current schedule
      switch (targetState->getNewVal()) {
        case OFF:
          if (navienSerial.currentState()->water[0].recirculation_active) {
            ret = navienSerial.recirculation(0);
            WEBLOG("Ignore schedule, turning off recirculation: %s\n", ret > 0 ? "Success" : "Failed");
          } else {
            WEBLOG("Ignore schedule, leaving recirculation off\n");
          }
          break;
        case HEAT:
          // If the schedule is not enabled, or is enabled and not active, then allow override
          // If the scheduler is enabled and active, override doesn't do anything
          if (!scheduler->enabled() || (scheduler->enabled() && !scheduler->isActive())) {
            if (scheduler->getCurrentState() != SchedulerBase::Override) {
              scheduler->activateOverride();
              WEBLOG("Requesting Heat NOW for 5 mins");
            } else {
              Serial.println("Requesting override twice, so ignoring it");
            }
          } else {
            WEBLOG("Ignoring Heat NOW request as it's already running via schedule");
          }

          break;
      }
    }

    if (targetTemp->updated()) {
      // Ignore temparature requests that are not valid
      float newSetPoint = targetTemp->getNewVal<float>();
      if (newSetPoint >= Navien::TEMPERATURE_MIN && newSetPoint <= Navien::TEMPERATURE_MAX) {
        ret = navienSerial.setTemp(targetTemp->getNewVal<float>());
        WEBLOG("Temperature target changed to %s: %s\n", temp2String(targetTemp->getNewVal<float>()).c_str(), ret > 0 ? "Success" : "Failed");
      } else {
        WEBLOG("Ignoring Temperature target of %s as it is out of range", temp2String(targetTemp->getNewVal<float>()).c_str());
      }
    }

    if (programCommand->updated()) {
      int len = programCommand->getNewData(0, 0);
      uint8_t* data = new uint8_t[len];
      programCommand->getNewData(data, len);
      scheduler->parseProgramData(data, len);
      delete data;
    }

    return (true);
  }


  void loop() override {

    float outletTemp = navienSerial.currentState()->gas.outlet_temp;
    if (currentTemp->timeVal() > 5000 && fabs(currentTemp->getVal<float>() - outletTemp) > 0.50) {  // if it's been more than 5 seconds since last update, and temperature has changed
      currentTemp->setVal(outletTemp);
      Serial.printf("Navien current Temperature is %s.\n", temp2String(currentTemp->getNewVal<float>()).c_str());
    }

    int operatingCap = (int)roundf(navienSerial.currentState()->water[0].operating_capacity);
    if (operatingCap != valvePosition->getVal()) {
      valvePosition->setVal(operatingCap);
    }

    if (!scheduler->enabled() &&  !navienSerial.currentState()->water[0].schedule_active) {
      programMode.setVal(0);
    } else if (scheduler->getCurrentState() == SchedulerBase::Override) {
      programMode.setVal(2);
    } else {
      programMode.setVal(1);
    }

    // Navien is actively maintaining the set point
    bool navienActivelyMaintainingTemp = 
      navienSerial.currentState()->water[0].recirculation_active || navienSerial.currentState()->gas.current_gas_usage > 0;

    float setTemp = navienSerial.currentState()->gas.set_temp;
    if ((targetTemp->getVal<float>() != setTemp) && (setTemp >= Navien::TEMPERATURE_MIN) && (setTemp <= Navien::TEMPERATURE_MAX)) {
      targetTemp->setVal(setTemp);
      Serial.printf("Navien target Temperature is %s.\n", temp2String(targetTemp->getNewVal<float>()).c_str());
    }

    // Check the state of the Navien and update appropriately
    int heating_state = navienSerial.currentState()->gas.current_gas_usage > 0 ? HEATING : IDLE;
    if (currentState->getVal() != heating_state) {
      Serial.printf("Updating heat state %d\n", heating_state);
      currentState->setVal(heating_state);
    }

    if (navienActivelyMaintainingTemp) {
        if (targetState->getVal() != HEAT) {
          targetState->setVal(HEAT);
          Serial.println("Forcing target state to Heat");
        }
    } else { // Scheduler is off and we're not heating so we are off
        if (targetState->getVal() != OFF) {
          targetState->setVal(OFF);
          Serial.println("Forcing target state to Off");
        }
    }

        // display_metric is 1 for Metric, 0 for Imperial.
    bool display_metric = navienSerial.currentState()->water[0].display_metric;
    if (display_metric && displayUnits.getVal() != CELSIUS) {
      displayUnits.setVal(CELSIUS);
    } else if (!display_metric && displayUnits.getVal() != FAHRENHEIT) {
      displayUnits.setVal(FAHRENHEIT);
    }

    // Try updating the version number
    if (!accessoryInfoSet) {
      firmwareRevision->setString(String(navienSerial.currentState()->gas.controller_version).c_str());
      hardwareRevision->setString(String(navienSerial.currentState()->gas.panel_version).c_str());
      accessoryInfoSet = true;
    }

    // Make the logging pretty. Bounce the setTemp in the log *only* between the actual setTemp when
    // it is heating and 10 when it isn't
    float logSetTemp = navienActivelyMaintainingTemp ? setTemp : 10.0;
    historyService->accumulateLogEntry(outletTemp, logSetTemp, (uint8_t)operatingCap, targetState->getVal(), 0);
  
    scheduler->loop();
  }

  // This "helper" function makes it easy to display temperatures on the serial monitor in either F or C depending on TemperatureDisplayUnits

  String temp2String(float temp) {
    String t = displayUnits.getVal() ? String(round(temp * 1.8 + 32.0)) : String(temp);
    t += displayUnits.getVal() ? " F" : " C";
    return (t);
  }
};

// Fetch the fused MAC address as WiFi object may not be connected yet, and it's value will be zero
// Use the mac address without colons as the device serial number.
String getSerialNumber() {

  String mac = "";

  unsigned char mac_base[6] = {0};

  if (esp_efuse_mac_get_default(mac_base) == ESP_OK) {
    char buffer[13];  // 6*2 characters for hex + 1 character for null terminator
    sprintf(buffer, "%02X%02X%02X%02X%02X%02X", mac_base[0], mac_base[1], mac_base[2], mac_base[3], mac_base[4], mac_base[5]);
    mac = buffer;
  }

  return mac;
}

void setupHomeSpanAccessories() {

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Manufacturer("Navien");
  new Characteristic::Model("NPE-240A");
  firmwareRevision = new Characteristic::FirmwareRevision("-");
  hardwareRevision = new Characteristic::HardwareRevision("-");
  new Characteristic::SerialNumber(getSerialNumber().c_str());

  new DEV_Navien();
}
