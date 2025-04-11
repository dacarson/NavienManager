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

const unsigned long broadcastDuplicatePacketThrottle = 5000;  // 5 seconds in milliseconds (5000 ms)
const int udpBroadcastPort = 2025;

extern Navien navienSerial;
extern ESPTelnet telnet;
extern String trace;

AsyncUDP udp;

// Previous packets recieved, use to check for duplicate broadcasts
String previousWater;
String previousGas;
String previousCommand;
String previousAnnounce;
unsigned long previousMillis = 0;

#define JSON_ASSIGN_WATER(field) doc[#field] = water->field;
#define JSON_ASSIGN_WATER_BOOL_TO_INT(field) doc[#field] = (int)(water->field);
#define JSON_ASSIGN_GAS(field) doc[#field] = state->gas.field;
#define JSON_ASSIGN_COMMAND(field) doc[#field] = state->command.field;
#define JSON_ASSIGN_COMMAND_BOOL_TO_INT(field) doc[#field] = (int)(state->command.field);

  /* Each broadcast routine checks to see if the new packet is different to 
  * the previous packet. Only if it is different does it broadcast it. This
  * function forcable resets the previous packet so that the new packet is 
  * broadcasted.
  */

void resetPreviousValues() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= broadcastDuplicatePacketThrottle) {
    previousWater = "";
    previousGas = "";
    previousCommand = "";
    previousAnnounce = "";
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
  JSON_ASSIGN_WATER(system_power);
  JSON_ASSIGN_WATER(set_temp);
  JSON_ASSIGN_WATER(inlet_temp);
  JSON_ASSIGN_WATER(outlet_temp);
  JSON_ASSIGN_WATER(flow_lpm);
  JSON_ASSIGN_WATER(flow_state);
  JSON_ASSIGN_WATER_BOOL_TO_INT(recirculation_active);
  JSON_ASSIGN_WATER_BOOL_TO_INT(recirculation_running);
  JSON_ASSIGN_WATER_BOOL_TO_INT(display_metric);
  JSON_ASSIGN_WATER_BOOL_TO_INT(schedule_active);
  JSON_ASSIGN_WATER_BOOL_TO_INT(hotbutton_active);
  JSON_ASSIGN_WATER(operating_capacity);
  JSON_ASSIGN_WATER_BOOL_TO_INT(consumption_active);
  doc["debug"] = rawhexstring;
  doc["unknown_10"] = navienSerial.rawPacketData()->water.unknown_10;
  doc["unknown_27"] = navienSerial.rawPacketData()->water.unknown_27;
  doc["unknown_28"] = navienSerial.rawPacketData()->water.unknown_28;
  doc["unknown_30"] = navienSerial.rawPacketData()->water.unknown_30;
  doc["counter_a"] = navienSerial.rawPacketData()->water.unknown_29 << 8 | navienSerial.rawPacketData()->water.unknown_28;
  doc["counter_b"] = navienSerial.rawPacketData()->water.unknown_31 << 8 | navienSerial.rawPacketData()->water.unknown_30;

  String json;
  serializeJson(doc, json);
  return json;
}

void onWaterPacket(Navien::NAVIEN_STATE_WATER *water) {
  resetPreviousValues();

  const Navien::PACKET_BUFFER *recv_buffer = navienSerial.rawPacketData();
  String rawhexstring = buffer_to_hex_string(recv_buffer->raw_data, Navien::HDR_SIZE + recv_buffer->hdr.len + 1);
  if (rawhexstring != previousWater) {
    String json = waterToJSON(water, rawhexstring);
    udp.broadcastTo(json.c_str(), 2025);

    if (trace == "water" || trace == "all")
      telnet.println(json);
  }
  previousWater = rawhexstring;
}

/* Handle Gas packets */

String gasToJSON(const Navien::NAVIEN_STATE *state, String rawhexstring = "") {
  JsonDocument doc;
  doc["type"] = "gas";

  JSON_ASSIGN_GAS(controller_version);
  JSON_ASSIGN_GAS(set_temp);
  JSON_ASSIGN_GAS(inlet_temp);
  JSON_ASSIGN_GAS(outlet_temp);
  JSON_ASSIGN_GAS(panel_version);
  JSON_ASSIGN_GAS(current_gas_usage);
  JSON_ASSIGN_GAS(accumulated_gas_usage);
  JSON_ASSIGN_GAS(total_operating_time);
  JSON_ASSIGN_GAS(accumulated_domestic_usage_cnt);
  doc["debug"] = rawhexstring;
  doc["unknown_20"] = navienSerial.rawPacketData()->gas.unknown_20;
  doc["unknown_28"] = navienSerial.rawPacketData()->gas.unknown_28;
  doc["unknown_32"] = navienSerial.rawPacketData()->gas.unknown_32;
  doc["unknown_33"] = navienSerial.rawPacketData()->gas.unknown_33;
  doc["unknown_34"] = navienSerial.rawPacketData()->gas.unknown_34;
  doc["counter_a"] = navienSerial.rawPacketData()->gas.unknown_29 << 8 | navienSerial.rawPacketData()->gas.unknown_28;
  doc["counter_c"] = navienSerial.rawPacketData()->gas.unknown_34 << 8 | navienSerial.rawPacketData()->gas.unknown_33;

  String json;
  serializeJson(doc, json);
  return json;
}

void onGasPacket(Navien::NAVIEN_STATE *state) {
  resetPreviousValues();

  const Navien::PACKET_BUFFER *recv_buffer = navienSerial.rawPacketData();
  String rawhexstring = buffer_to_hex_string(recv_buffer->raw_data, Navien::HDR_SIZE + recv_buffer->hdr.len + 1);
  if (rawhexstring != previousGas) {
    String json = gasToJSON(state, rawhexstring);
    udp.broadcastTo(json.c_str(), 2025);

    if (trace == "gas" || trace == "all")
      telnet.println(json);
  }
  previousGas = rawhexstring;
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
  if (rawhexstring != previousCommand) {
    String json = commandToJSON(state, rawhexstring);
    udp.broadcastTo(json.c_str(), 2025);

    if (trace == "command" || trace == "all")
      telnet.println(json);
  }
  previousCommand = rawhexstring;
}

/* Handle Announce packets */

String announceToJSON(const Navien::NAVIEN_STATE *state, String rawhexstring = "") {
  String json = "{ \"type\" : \"announce\", ";
  json += " \"navilink_present\" : " + String(state->announce.navilink_present) + ", ";
  json += " \"debug\" : \"" + rawhexstring + "\"";
  json += "}";

  return json;
}

void onAnnouncePacket(Navien::NAVIEN_STATE *state) {
  resetPreviousValues();

  const Navien::PACKET_BUFFER *recv_buffer = navienSerial.rawPacketData();
  String rawhexstring = buffer_to_hex_string(recv_buffer->raw_data, Navien::HDR_SIZE + recv_buffer->hdr.len + 1);
  if (rawhexstring != previousAnnounce) {
    String json = announceToJSON(state, rawhexstring);
    udp.broadcastTo(json.c_str(), 2025);

    if (trace == "announce" || trace == "all")
      telnet.println(json);
  }
  previousAnnounce = rawhexstring;
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