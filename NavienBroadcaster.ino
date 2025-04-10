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
  String json = "{ \"type\" : \"water\", ";
  json += " \"device_number\" : " + String(water->device_number) + ", ";
  json += " \"system_power\" : " + String(water->system_power) + ", ";
  json += " \"set_temp\" : " + String(water->set_temp) + ", ";
  json += " \"inlet_temp\" : " + String(water->inlet_temp) + ", ";
  json += " \"outlet_temp\" : " + String(water->outlet_temp) + ", ";
  json += " \"flow_lpm\" : " + String(water->flow_lpm) + ", ";
  json += " \"flow_state\" : " + String(water->flow_state) + ", ";
  json += " \"recirculation_active\" : " + String(water->recirculation_active) + ", ";
  json += " \"recirculation_running\" : " + String(water->recirculation_running) + ", ";
  json += " \"display_metric\" : " + String(water->display_metric) + ", ";
  json += " \"schedule_active\" : " + String(water->schedule_active) + ", ";
  json += " \"hotbutton_active\" : " + String(water->hotbutton_active) + ", ";
  json += " \"operating_capacity\" : " + String(water->operating_capacity) + ", ";
  json += " \"consumption_active\" : " + String(water->consumption_active) + ", ";
  json += " \"debug\" : \"" + rawhexstring + "\", ";
  json += " \"unknown_10\" : " + String(navienSerial.rawPacketData()->water.unknown_10) + ", ";
  json += " \"unknown_27\" : " + String(navienSerial.rawPacketData()->water.unknown_27) + ", ";
  json += " \"unknown_28\" : " + String(navienSerial.rawPacketData()->water.unknown_28) + ", ";
  json += " \"unknown_30\" : " + String(navienSerial.rawPacketData()->water.unknown_30) + ", ";
  json += " \"counter_a\" : " + String(navienSerial.rawPacketData()->water.unknown_29 << 8 | navienSerial.rawPacketData()->water.unknown_28) + ", ";
  json += " \"counter_b\" : " + String(navienSerial.rawPacketData()->water.unknown_31 << 8 | navienSerial.rawPacketData()->water.unknown_30);
  json += "}";

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
  String json = "{ \"type\" : \"gas\", ";
  json += " \"controller_version\" : " + String(state->gas.controller_version) + ", ";
  json += " \"set_temp\" : " + String(state->gas.set_temp) + ", ";
  json += " \"inlet_temp\" : " + String(state->gas.inlet_temp) + ", ";
  json += " \"outlet_temp\" : " + String(state->gas.outlet_temp) + ", ";
  json += " \"panel_version\" : " + String((float)state->gas.panel_version) + ", ";
  json += " \"current_gas_usage\" : " + String(state->gas.current_gas_usage) + ", ";
  json += " \"accumulated_gas_usage\" : " + String(state->gas.accumulated_gas_usage) + ", ";
  json += " \"total_operating_time\" : " + String(state->gas.total_operating_time) + ", ";
  json += " \"accumulated_domestic_usage_cnt\" : " + String(state->gas.accumulated_domestic_usage_cnt) + ", ";
  json += " \"debug\" : \"" + rawhexstring + "\", ";
  json += " \"unknown_20\" : " + String(navienSerial.rawPacketData()->gas.unknown_20) + ", ";
  json += " \"unknown_28\" : " + String(navienSerial.rawPacketData()->gas.unknown_28) + ", ";
  json += " \"unknown_32\" : " + String(navienSerial.rawPacketData()->gas.unknown_32) + ", ";
  json += " \"unknown_33\" : " + String(navienSerial.rawPacketData()->gas.unknown_33) + ", ";
  json += " \"unknown_34\" : " + String(navienSerial.rawPacketData()->gas.unknown_34) + ", ";
  json += " \"counter_a\" : " + String(navienSerial.rawPacketData()->gas.unknown_29 << 8 | navienSerial.rawPacketData()->gas.unknown_28) + ", ";
  json += " \"counter_c\" : " + String(navienSerial.rawPacketData()->gas.unknown_34 << 8 | navienSerial.rawPacketData()->gas.unknown_33);
  json += "}";

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
  String json = "{ \"type\" : \"command\", ";
  json += " \"power_command\" : " + String(state->command.power_command) + ", ";
  json += " \"power_on\" : " + String(state->command.power_on) + ", ";
  json += " \"set_temp_command\" : " + String(state->command.set_temp_command) + ", ";
  json += " \"set_temp\" : " + String(state->command.set_temp) + ", ";
  json += " \"hot_button_command\" : " + String(state->command.hot_button_command) + ", ";
  json += " \"recirculation_command\" : " + String(state->command.recirculation_command) + ", ";
  json += " \"recirculation\" : " + String(state->command.recirculation_on) + ", ";
  json += " \"cmd_data\" : " + String(state->command.cmd_data) + ", ";
  json += " \"debug\" : \"" + rawhexstring + "\"";
  json += "}";

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