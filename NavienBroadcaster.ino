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



#include <ESPTelnet.h>
#include <AsyncUDP.h>
#include <ArduinoJson.h>
#include "Navien.h"
#include "NavienLearner.h"

const unsigned long broadcastDuplicatePacketThrottle = 5000;  // 5 seconds in milliseconds (5000 ms)
const int udpBroadcastPort = 2025;

extern Navien navienSerial;
extern ESPTelnet telnet;
extern String trace;
extern NavienLearner *learner;

AsyncUDP udp;

// Previous packets recieved, use to check for duplicate broadcasts
char previousWater[385];
char previousGas[385];
char previousCommand[385];
char previousAnnounce[385];
unsigned long previousMillis = 0;

#define JSON_ASSIGN_WATER(field) doc[#field] = water->field;
#define JSON_ASSIGN_WATER_BOOL_TO_INT(field) doc[#field] = (int)(water->field);
#define JSON_ASSIGN_WATER_FLOAT(field) doc[#field] = serialized(String(water->field, 1))
#define JSON_ASSIGN_GAS(field) doc[#field] = gas->field;
#define JSON_ASSIGN_GAS_FLOAT(field) doc[#field] = serialized(String(gas->field, 1))
#define JSON_ASSIGN_GAS_BOOL_TO_INT(field) doc[#field] = (int)(gas->field)
#define JSON_ASSIGN_COMMAND(field) doc[#field] = state->command.field;
#define JSON_ASSIGN_COMMAND_BOOL_TO_INT(field) doc[#field] = (int)(state->command.field);
#define JSON_ASSIGN_ANNOUNCE_BOOL_TO_INT(field) doc[#field] = (int)(state->announce.field);

  /* Each broadcast routine checks to see if the new packet is different to 
  * the previous packet. Only if it is different does it broadcast it. This
  * function forcable resets the previous packet so that the new packet is 
  * broadcasted.
  */

void resetPreviousValues() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= broadcastDuplicatePacketThrottle) {
    previousWater[0] = '\0';
    previousGas[0] = '\0';
    previousCommand[0] = '\0';
    previousAnnounce[0] = '\0';
    previousMillis = currentMillis;  // Reset the last time to the current time
  }
}

String buffer_to_hex_string(const uint8_t *data, size_t length) {
  String hexString = "";

  for (size_t i = 0; i < length; ++i) {
    if (data[i] < 0x10) {
      hexString += "0";
    }
    hexString += String(data[i], HEX);
    hexString += " ";
  }

  hexString.toUpperCase();  // Convert to uppercase if desired.
  return hexString;
}

/* Handle Water packets */
String waterToJSON(const Navien::NAVIEN_STATE_WATER *water, String rawhexstring = "") {
  JsonDocument doc;
  doc["type"] = "water";
  
  JSON_ASSIGN_WATER(device_number);
  JSON_ASSIGN_WATER_BOOL_TO_INT(system_power);
  JSON_ASSIGN_WATER_FLOAT(set_temp);
  JSON_ASSIGN_WATER_FLOAT(inlet_temp);
  JSON_ASSIGN_WATER_FLOAT(outlet_temp);
  JSON_ASSIGN_WATER_FLOAT(flow_lpm);
  JSON_ASSIGN_WATER(flow_state);
  JSON_ASSIGN_WATER_BOOL_TO_INT(recirculation_active);
  JSON_ASSIGN_WATER_BOOL_TO_INT(recirculation_running);
  JSON_ASSIGN_WATER_BOOL_TO_INT(display_metric);
  JSON_ASSIGN_WATER_BOOL_TO_INT(internal_recirculation);
  JSON_ASSIGN_WATER_BOOL_TO_INT(external_recirculation);
  JSON_ASSIGN_WATER_FLOAT(operating_capacity);
  JSON_ASSIGN_WATER_BOOL_TO_INT(consumption_active);
  JSON_ASSIGN_WATER(system_stage);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_idle);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_starting);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_active);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_shutting_down);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_standby);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_demand);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_pre_purge);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_ignition);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_flame_on);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_ramp_up);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_active_combustion);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_water_adjustment);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_flame_off);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_post_purge_1);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_post_purge_2);
  JSON_ASSIGN_WATER_BOOL_TO_INT(stage_dhw_wait);
  JSON_ASSIGN_WATER_BOOL_TO_INT(system_active);
  JSON_ASSIGN_WATER(operation_time);
  doc["debug"] = rawhexstring;
  doc["unknown_30"] = navienSerial.rawPacketData()->water.unknown_30;
  doc["unknown_31"] = navienSerial.rawPacketData()->water.unknown_31;

  String json;
  serializeJson(doc, json);
  return json;
}

void onWaterPacket(Navien::NAVIEN_STATE_WATER *water) {
  if (learner) {
    learner->onNavienState(water->consumption_active,
                           water->recirculation_active,
                           time(nullptr));
  }

  resetPreviousValues();

  const Navien::PACKET_BUFFER *recv_buffer = navienSerial.rawPacketData();
  String rawhexstring = buffer_to_hex_string(recv_buffer->raw_data, Navien::HDR_SIZE + recv_buffer->hdr.len + 1);
  if (strcmp(rawhexstring.c_str(), previousWater) != 0) {
    String json = waterToJSON(water, rawhexstring);
    udp.broadcastTo(json.c_str(), 2025);

    if (trace == "water" || trace == "all")
      telnet.println(json);
  }
  strncpy(previousWater, rawhexstring.c_str(), sizeof(previousWater) - 1);
}

/* Handle Gas packets */

String gasToJSON(const Navien::NAVIEN_STATE_GAS *gas, String rawhexstring = "") {
  JsonDocument doc;
  doc["type"] = "gas";

  JSON_ASSIGN_GAS_FLOAT(controller_version);
  JSON_ASSIGN_GAS_FLOAT(set_temp);
  JSON_ASSIGN_GAS_FLOAT(inlet_temp);
  JSON_ASSIGN_GAS_FLOAT(outlet_temp);
  JSON_ASSIGN_GAS_FLOAT(panel_version);
  JSON_ASSIGN_GAS(current_gas_usage);
  JSON_ASSIGN_GAS(target_gas_usage);
  JSON_ASSIGN_GAS_FLOAT(accumulated_gas_usage);
  JSON_ASSIGN_GAS_FLOAT(accumulated_water_usage);
  JSON_ASSIGN_GAS(total_operating_time);
  JSON_ASSIGN_GAS(elapsed_install_days);
  JSON_ASSIGN_GAS(accumulated_domestic_usage_cnt);
  JSON_ASSIGN_GAS_BOOL_TO_INT(recirculation_enabled);
  doc["debug"] = rawhexstring;

  String json;
  serializeJson(doc, json);
  return json;
}

void onGasPacket(Navien::NAVIEN_STATE_GAS *gas) {
  resetPreviousValues();

  const Navien::PACKET_BUFFER *recv_buffer = navienSerial.rawPacketData();
  String rawhexstring = buffer_to_hex_string(recv_buffer->raw_data, Navien::HDR_SIZE + recv_buffer->hdr.len + 1);
  if (strcmp(rawhexstring.c_str(), previousGas) != 0) {
    String json = gasToJSON(gas, rawhexstring);
    udp.broadcastTo(json.c_str(), 2025);

    if (trace == "gas" || trace == "all")
      telnet.println(json);
  }
  strncpy(previousGas, rawhexstring.c_str(), sizeof(previousGas) - 1);
}

/* Handle Command packets */

String commandToJSON(const Navien::NAVIEN_STATE *state, String rawhexstring = "") {
  JsonDocument doc;
  doc["type"] = "command";

  JSON_ASSIGN_COMMAND_BOOL_TO_INT(power_command);
  JSON_ASSIGN_COMMAND_BOOL_TO_INT(power_on);
  JSON_ASSIGN_COMMAND_BOOL_TO_INT(set_temp_command);
  JSON_ASSIGN_COMMAND(set_temp);
  JSON_ASSIGN_COMMAND_BOOL_TO_INT(hot_button_command);
  JSON_ASSIGN_COMMAND_BOOL_TO_INT(recirculation_command);
  JSON_ASSIGN_COMMAND_BOOL_TO_INT(recirculation_on);
  JSON_ASSIGN_COMMAND(cmd_data);
  doc["debug"] = rawhexstring;

  String json;
  serializeJson(doc, json);
  return json;
}

void onCommandPacket(Navien::NAVIEN_STATE *state) {
  resetPreviousValues();

  const Navien::PACKET_BUFFER *recv_buffer = navienSerial.rawPacketData();
  String rawhexstring = buffer_to_hex_string(recv_buffer->raw_data, Navien::HDR_SIZE + recv_buffer->hdr.len + 1);
  if (strcmp(rawhexstring.c_str(), previousCommand) != 0) {
    String json = commandToJSON(state, rawhexstring);
    udp.broadcastTo(json.c_str(), 2025);

    if (trace == "command" || trace == "all")
      telnet.println(json);
  }
  strncpy(previousCommand, rawhexstring.c_str(), sizeof(previousCommand) - 1);
}

/* Handle Announce packets */

String announceToJSON(const Navien::NAVIEN_STATE *state, String rawhexstring = "") {
  JsonDocument doc;
  doc["type"] = "announce";
  JSON_ASSIGN_ANNOUNCE_BOOL_TO_INT(navilink_present);
  doc["debug"] = rawhexstring;

  String json;
  serializeJson(doc, json);
  return json;
}

void onAnnouncePacket(Navien::NAVIEN_STATE *state) {
  resetPreviousValues();

  const Navien::PACKET_BUFFER *recv_buffer = navienSerial.rawPacketData();
  String rawhexstring = buffer_to_hex_string(recv_buffer->raw_data, Navien::HDR_SIZE + recv_buffer->hdr.len + 1);
  if (strcmp(rawhexstring.c_str(), previousAnnounce) != 0) {
    String json = announceToJSON(state, rawhexstring);
    udp.broadcastTo(json.c_str(), 2025);

    if (trace == "announce" || trace == "all")
      telnet.println(json);
  }
  strncpy(previousAnnounce, rawhexstring.c_str(), sizeof(previousAnnounce) - 1);
}

/* Report any errors that occurred */

void onError(const char *function, const char *errorMessage) {
  telnet.print(F("Error "));
  telnet.print(function);
  telnet.print(F(" "));
  telnet.println(errorMessage);
}

void setupNavienBroadcaster() {
  navienSerial.onGasPacket(onGasPacket);
  navienSerial.onWaterPacket(onWaterPacket);
  navienSerial.onCommandPacket(onCommandPacket);
  navienSerial.onAnnouncePacket(onAnnouncePacket);
  navienSerial.onError(onError);

  Serial.println(F("UDP Broadcast started"));
}