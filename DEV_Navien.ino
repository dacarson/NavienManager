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
#include "FakeGatoHistoryService.h"
extern Navien navienSerial;

// Global so that Telnet can dump history state.
FakeGatoHistoryService *historyService;
FakeGatoScheduler *scheduler;

//#define TEST 1


CUSTOM_CHAR(ValvePosition, E863F12E-079E-48FF-8F27-9C2605A29F52, PR+EV, UINT8, 0, 0, 100, true);
CUSTOM_CHAR_DATA(ProgramCommand, E863F12C-079E-48FF-8F27-9C2605A29F52, PW + EV);

Characteristic::SerialNumber *serialNumber;

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

  // Navien 240A has temperature range for domestic hot water (DHW) of 37degC to 60degC
  int TARGET_TEMP_MIN = 37;
  int TARGET_TEMP_MAX = 60;

  // Declare the Themo characteristics
  Characteristic::CurrentHeatingCoolingState *currentState;
  Characteristic::TargetHeatingCoolingState *targetState;
  Characteristic::CurrentTemperature *currentTemp;
  Characteristic::TargetTemperature *targetTemp;
  Characteristic::TemperatureDisplayUnits displayUnits{ 0, true };

  // Declare the Custom characteristics
  Characteristic::ProgramCommand *programCommand;
  Characteristic::ValvePosition *valvePosition;

  bool serialNumberSet = false;

  DEV_Navien()
    : Service::Thermostat() {
    Serial.printf("\n*** Creating Navien Thermostat***\n");

    // Current Heating Cooling State (Read-Only)
    currentState = new Characteristic::CurrentHeatingCoolingState();

    // Target Heating Cooling State (User Selectable)
    targetState = new Characteristic::TargetHeatingCoolingState(OFF);  // Default: Off
    targetState->setValidValues(OFF, HEAT, AUTO);

    // Current Temperature
    currentTemp = new Characteristic::CurrentTemperature(45.0);  // Default 22°C

    // Target Temperature
    targetTemp = new Characteristic::TargetTemperature(40.0);     // Default 40°C
    targetTemp->setRange(TARGET_TEMP_MIN, TARGET_TEMP_MAX, 0.5);  // Set min/max/step values

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
      switch (targetState->getNewVal()) {
        case OFF:
          ret = navienSerial.recirculation(0);
          WEBLOG("Turn OFF Recirculation: %s\n", ret==0 ? "Success" : "Failed");
          break;
        case HEAT:
          ret = navienSerial.hotButton();
          WEBLOG("Device requesting heat now. Set point at %s: %s\n", temp2String(targetTemp->getVal<float>()).c_str(), ret < 0 ? "Success" : "Failed");
          break;
        case AUTO:
          ret = navienSerial.recirculation(1);
          WEBLOG("Turn ON Recirculation with set point at %s: %s\n", temp2String(targetTemp->getVal<float>()).c_str(), ret < 0 ? "Success" : "Failed");
          break;
      }
    }

    else if (targetTemp->updated()) {
      ret = navienSerial.setTemp(targetTemp->getNewVal<float>());
      WEBLOG("Temperature target changed to %s: %s\n", temp2String(targetTemp->getNewVal<float>()).c_str(), ret < 0 ? "Success" : "Failed");
    }

    if (programCommand->updated()) {
      int len = programCommand->getNewData(0, 0);
      uint8_t *data = (uint8_t *)malloc(sizeof(uint8_t) * len);
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

    float setTemp = navienSerial.currentState()->gas.set_temp;
  #if defined (TEST)
    setTemp = 46.0;
  #endif
    if ((targetTemp->getVal<float>() != setTemp) && (setTemp >= TARGET_TEMP_MIN) && (setTemp <= TARGET_TEMP_MAX)) {
      targetTemp->setVal(setTemp);
      Serial.printf("Navien target Temperature is %s.\n", temp2String(targetTemp->getNewVal<float>()).c_str());
    }

    // display_metric is 1 for Metric, 0 for Imperial.
    bool display_metric = navienSerial.currentState()->water.display_metric;
    if (display_metric && displayUnits.getVal() != CELSIUS) {
      displayUnits.setVal(CELSIUS);
    } else if (!display_metric && displayUnits.getVal() != FAHRENHEIT) {
      displayUnits.setVal(FAHRENHEIT);
    }

    // Try updating the version number
    if (serialNumber && !serialNumberSet) {
      char serial[30];
      sprintf(serial, "%0.2f - %0.2f", navienSerial.currentState()->gas.panel_version, navienSerial.currentState()->gas.controller_version);
      serialNumber->setString(serial);
      serialNumberSet = true;
    }

    int operatingCap = (int)roundf(navienSerial.currentState()->water.operating_capacity);
    if (operatingCap != valvePosition->getVal()) {
      valvePosition->setVal(operatingCap);
    }

    // Check the state of the Navien and update appropriately
    int heating_state = navienSerial.currentState()->gas.current_gas_usage > 0 ? HEATING : IDLE;

  #if defined (TEST)
    heating_state = targetState->getVal() == HEAT ? HEATING : IDLE;
  #endif
    if (currentState->getVal() != heating_state) {
      Serial.printf("Updating heat state %d\n", heating_state);
      currentState->setVal(heating_state);
    }
    
    if (navienSerial.currentState()->water.recirculation_active) {
      
      if (targetState->getVal() != AUTO) {
        targetState->setVal(AUTO);
      }
    } else { // recirculation isn't active, but we may be heating.
      if (targetState->getVal() != HEAT && heating_state == HEATING) {
        targetState->setVal(HEAT);
      } else if (targetState->getVal() != OFF && heating_state == IDLE) {
        targetState->setVal(OFF);
      }
    }

  #if defined (TEST)
    historyService->accumulateLogEntry(26.6, setTemp, (uint8_t)0, targetState->getVal(), 0);
  #else
    historyService->accumulateLogEntry(outletTemp, setTemp, (uint8_t)operatingCap, targetState->getVal(), 0);
  #endif
  

    scheduler->loop();
  }

  // This "helper" function makes it easy to display temperatures on the serial monitor in either F or C depending on TemperatureDisplayUnits

  String temp2String(float temp) {
    String t = displayUnits.getVal() ? String(round(temp * 1.8 + 32.0)) : String(temp);
    t += displayUnits.getVal() ? " F" : " C";
    return (t);
  }
};

void setupHomeSpanAccessories() {

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Manufacturer("Navien");
  new Characteristic::Model("NPE-240A");
#if defined (TEST)
  new Characteristic::SerialNumber("1.0 test");
#else
  serialNumber = new Characteristic::SerialNumber("1.0");
#endif

  new DEV_Navien();
}
