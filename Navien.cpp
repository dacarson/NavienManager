/*
Copyright (c) 2024 Hovhannes Tumanyan (htumanyan)
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

#include <Arduino.h>
#include "Navien.h"

const uint8_t Navien::COMMAND_HEADER[] = { 0xF7, 0x05, 0x0F, 0x50, 0x10, 0x0C, 0x4F };

bool Navien::seek_to_marker() {
  uint8_t byte;

  int availableBytes = available();
  for (int i = 0; i < availableBytes; i++) {
    byte = peek();
    if (byte == PACKET_MARKER)
      return true;

    read();
  }

  return false;
}

void Navien::parse_water() {
  if (recv_buffer.water.cmd_type != CMD_TYPE_WATER) {
    // Cascading units, have seen F7 13 50 52 10 03 40 00 04
    Serial.println("Unknown water packet");
    return;
  }

  uint8_t device_number = recv_buffer.hdr.packet_type - PACKET_TYPE_WATER_MIN;
  if (device_number > state.max_water_devices_seen)
    state.max_water_devices_seen = device_number;

  state.water[device_number].device_number = device_number;
  state.water[device_number].system_power = (recv_buffer.water.system_power & 0x5) ? 0x1 : 0x0;
  state.water[device_number].flow_state = recv_buffer.water.flow_state;
  state.water[device_number].consumption_active = (recv_buffer.water.flow_state & 0x20) ? 0x1 : 0x0;
  state.water[device_number].recirculation_running = (recv_buffer.water.flow_state & 0x08) ? 0x1 : 0x0;
  state.water[device_number].set_temp = Navien::t2c(recv_buffer.water.set_temp);
  state.water[device_number].outlet_temp = Navien::t2c(recv_buffer.water.outlet_temp);
  state.water[device_number].inlet_temp = Navien::t2c(recv_buffer.water.inlet_temp);
  state.water[device_number].display_metric = (recv_buffer.water.system_status & 0x8) ? 0x1 : 0x0;
  state.water[device_number].schedule_active = (recv_buffer.water.system_status & 0x2) ? 0x1 : 0x0;
  state.water[device_number].hotbutton_active = (recv_buffer.water.system_status & 0x2) ? 0x0 : 0x1;
  state.water[device_number].operating_capacity = 0.5 * recv_buffer.water.operating_capacity;  // 0.5 increments
  state.water[device_number].flow_lpm = Navien::flow2lpm(recv_buffer.water.water_flow);
  state.water[device_number].recirculation_active = (recv_buffer.water.recirculation_enabled & 0x2) ? 0x1 : 0x0;

  if (on_water_packet_cb) on_water_packet_cb(&state.water[device_number]);
}

void Navien::parse_gas() {
  if (recv_buffer.gas.cmd_type != CMD_TYPE_GAS) {
    Serial.println("Unknown gas packet");
    return;
  }
  state.gas.set_temp = Navien::t2c(recv_buffer.gas.set_temp);
  state.gas.outlet_temp = Navien::t2c(recv_buffer.gas.outlet_temp);
  state.gas.inlet_temp = Navien::t2c(recv_buffer.gas.inlet_temp);

  char buffer[10];

  sprintf(buffer, "%d.%d", recv_buffer.gas.controller_version_hi, recv_buffer.gas.controller_version_lo);
  state.gas.controller_version = atof(buffer);
  sprintf(buffer, "%d.%d", recv_buffer.gas.panel_version_hi, recv_buffer.gas.panel_version_lo);
  state.gas.panel_version = atof(buffer);

  // Safely compute accumulated gas usage with overflow protection
  uint32_t raw_gas = (recv_buffer.gas.cumulative_gas_hi << 8 | recv_buffer.gas.cumulative_gas_lo);
  state.gas.accumulated_gas_usage = 0.1f * raw_gas;  // Using float multiplication for better precision
  state.gas.current_gas_usage = recv_buffer.gas.current_gas_hi << 8 | recv_buffer.gas.current_gas_lo;

  // Convert total operating time to minutes using 32-bit arithmetic
  uint32_t raw_time = (recv_buffer.gas.total_operating_time_hi << 8 | recv_buffer.gas.total_operating_time_lo);
  state.gas.total_operating_time = 60 * raw_time;  // Safe with uint32_t

  // Convert domestic usage count using 32-bit arithmetic
  uint32_t raw_usage = (recv_buffer.gas.cumulative_domestic_usage_cnt_hi << 8 | recv_buffer.gas.cumulative_domestic_usage_cnt_lo);
  state.gas.accumulated_domestic_usage_cnt = 10 * raw_usage;  // Safe with uint32_t

  if (on_gas_packet_cb) on_gas_packet_cb(&(state.gas));
}

void Navien::parse_status_packet() {
  uint8_t pkt_type = recv_buffer.hdr.packet_type;

  if (pkt_type >= PACKET_TYPE_WATER_MIN && pkt_type <= PACKET_TYPE_WATER_MAX) {
    parse_water();
    return;
  }
  switch (pkt_type) {
    case Navien::PACKET_TYPE_GAS:
      parse_gas();
      break;
    default:
      if (on_error_cb)
        on_error_cb(__func__, "Unknown status packet type received.");
      Navien::print_buffer(recv_buffer.raw_data, recv_buffer.hdr.len + HDR_SIZE, on_error_cb);
      break;
  }
}

void Navien::parse_announce() {
  if (recv_buffer.announce.cmd_type != CMD_TYPE_ANNOUNCE) {
    Serial.println("Unknown announce packet");
    return;
  }
  //Navien::print_buffer(recv_buffer.raw_data, recv_buffer.hdr.len + HDR_SIZE, on_error_cb);

  // If there are any announce packets seen, then there uis a navilink present
  navilink_present = true;
  state.announce.navilink_present = true;

  if (on_announce_packet_cb) on_announce_packet_cb(&(state));
}

void Navien::parse_command() {
  if (recv_buffer.cmd.cmd_type != CMD_TYPE_CMD) {
    Serial.println("Unknown command packet");
    return;
  }
  //Navien::print_buffer(recv_buffer.raw_data, recv_buffer.hdr.len + HDR_SIZE, on_error_cb);

  memset(&(state.command), 0x0, sizeof(state.command));

  if (recv_buffer.cmd.system_power == Navien::SYSTEM_POWER_ON) {
    state.command.power_command = true;
    state.command.power_on = true;
  } else if (recv_buffer.cmd.system_power == Navien::SYSTEM_POWER_OFF) {
    state.command.power_command = true;
    state.command.power_on = false;
  }

  if (recv_buffer.cmd.set_temp > 0) {
    state.command.set_temp_command = true;
    state.command.set_temp = (float)recv_buffer.cmd.set_temp / 2.0;
  }

  if (recv_buffer.cmd.hot_button_recirculation & Navien::HOT_BUTTON_DOWN) {
    state.command.hot_button_command = true;
  }

  if (recv_buffer.cmd.hot_button_recirculation & Navien::RECIRCULATION_ON) {
    state.command.recirculation_command = true;
    state.command.recirculation_on = true;
  } else if (recv_buffer.cmd.hot_button_recirculation & Navien::RECIRCULATION_OFF) {
    state.command.recirculation_command = true;
    state.command.recirculation_on = false;
  }

  if (on_command_packet_cb) on_command_packet_cb(&(state));
}

void Navien::parse_control_packet() {
  switch (recv_buffer.cmd.cmd_type) {
    case Navien::CONTROL_ANNOUNCE:
      parse_announce();
      break;
    case Navien::CONTROL_COMMAND:
      parse_command();
      break;
    default:
      if (on_error_cb)
        on_error_cb(__func__, "Unknown control packet type received.");
      Navien::print_buffer(recv_buffer.raw_data, recv_buffer.hdr.len + HDR_SIZE, on_error_cb);
      break;
  }
}

void Navien::parse_packet() {
  uint8_t crc_c = 0x00;  // Computed CRC
  uint8_t crc_r = 0x00;  // Received CRC

  char errBuffer[255];

  //Navien::print_buffer(recv_buffer.raw_data, HDR_SIZE + recv_buffer.hdr.len + 1);
  crc_r = recv_buffer.raw_data[HDR_SIZE + recv_buffer.hdr.len];

  switch (recv_buffer.hdr.direction) {
    case Navien::PACKET_DIRECTION_STATUS:
      crc_c = Navien::checksum(recv_buffer.raw_data, HDR_SIZE + recv_buffer.hdr.len, CHECKSUM_SEED_4B);
      if (crc_c != crc_r) {
        sprintf(errBuffer, "Status Packet checksum error: 0x%02X (calc) != 0x%02X (recv)", crc_c, crc_r);
        if (on_error_cb)
          on_error_cb(__func__, errBuffer);
        break;
      }
      parse_status_packet();
      break;
    case Navien::PACKET_DIRECTION_CONTROL:
      crc_c = Navien::checksum(recv_buffer.raw_data, HDR_SIZE + recv_buffer.hdr.len, CHECKSUM_SEED_62);
      if (crc_c != crc_r) {
        sprintf(errBuffer, "Control Packet checksum error: 0x%02X (calc) != 0x%02X (recv)", crc_c, crc_r);
        if (on_error_cb)
          on_error_cb(__func__, errBuffer);
        break;
      }
      parse_control_packet();
      break;
  }
}


void Navien::loop() {
  uint8_t byte;

  char errBuffer[255];

  int availableBytes = available();

  // Check to see if we can send data, and send if possible
  if (recv_state == INITIAL && can_send(availableBytes)) {
    send_cmd();
  }

  if (!availableBytes)
    return;

  // We have moved from Initial, so we must
  // be talking to a real unit.
  if (recv_state != INITIAL && test_mode) {
    test_mode = false;
  }

  while (availableBytes) {
    switch (recv_state) {
      case INITIAL:
        if (seek_to_marker()) {
          recv_state = MARKER_FOUND;
          break;
        }
        // No marker found and no data left to read. Exit and wait
        // for more bytes to come.
        return;
      case MARKER_FOUND:
        availableBytes = available();
        if (availableBytes < HDR_SIZE) {
          return;
        }
        if (!read(recv_buffer.raw_data, HDR_SIZE)) {
          sprintf(errBuffer, "Failed to read header: %d when told %d is available", HDR_SIZE, availableBytes);
          if (on_error_cb)
            on_error_cb(__func__, errBuffer);
          break;
        }
        recv_state = HEADER_PARSED;
        //Navien::print_buffer(recv_buffer.raw_data, HDR_SIZE, on_error_cb);

        // Reset if headerlen is too long
        if (recv_buffer.hdr.len == 0xFF || recv_buffer.hdr.len >= sizeof(PACKET_BUFFER)) {
          if (recv_buffer.hdr.len == 0xFF) {
            if (on_error_cb)
              on_error_cb(__func__, "Invalid header length, are the 485 wires reversed?");
          } else if (on_error_cb)
              on_error_cb(__func__, "Buffer to small for packet length data, dropping packet.");
          recv_state = INITIAL;
          return;
        }

      case HEADER_PARSED:
        availableBytes = available();

        // +1 here is for the checksum - it is in the last byte
        uint8_t len = recv_buffer.hdr.len + 1;
        if (availableBytes < len) {
          return;
        }
        if (!read(recv_buffer.raw_data + HDR_SIZE, len)) {
          sprintf(errBuffer, "Failed to read %d bytes when told %d is available", len, availableBytes);
          if (on_error_cb)
            on_error_cb(__func__, errBuffer);
          break;
        }
        //Navien::print_buffer(recv_buffer.raw_data, len+HDR_SIZE, on_error_cb);
        parse_packet();
        availableBytes = available();
        recv_state = INITIAL;
    }
  }
}

/**
 * Convert flow units to liters/min values
 * flow is reported as 0.1 liter units.
 */
float Navien::flow2lpm(uint8_t f) {
  return (float)f / 10.f;
}

float Navien::t2c(uint8_t c) {
  return (float)c / 2.f;
}


void Navien::print_buffer(const uint8_t *data, size_t length, ErrorCallbackFunction on_error_cb) {
  char hex_buffer[100];
  hex_buffer[(3 * 32) + 1] = 0;
  for (size_t i = 0; i < length; i++) {
    snprintf(&hex_buffer[3 * (i % 32)], sizeof(hex_buffer), "%02X ", data[i]);
    if (i % 32 == 31) {
      if (on_error_cb)
        on_error_cb(__func__, hex_buffer);
    }
  }
  if (length % 32) {
    // null terminate if incomplete line
    hex_buffer[3 * (length % 32) + 2] = 0;
    if (on_error_cb)
      on_error_cb(__func__, hex_buffer);
  }
}

int Navien::queue_send_cmd(const PACKET_BUFFER& pkt) {
  if (send_queue_count >= QUEUE_CAPACITY)
    return -1;
  send_array[send_queue_tail] = pkt;
  send_queue_tail = (send_queue_tail + 1) % QUEUE_CAPACITY;
  ++send_queue_count;
  return 0;
}

bool Navien::can_send(int curr_available) {
  static unsigned long last_received = 0;

  if (curr_available > 0) {
    last_received = millis();
    return false;
  }

  if (millis() - last_received > 10) { // e.g. 10ms of silence
    return true;
  }

  return false;
} 

int Navien::send_cmd() {

  if (send_queue_count == 0)
    return -1;
  PACKET_BUFFER send_buffer = send_array[send_queue_head];
  send_queue_head = (send_queue_head + 1) % QUEUE_CAPACITY;
  --send_queue_count;

  // +1 to include the crc value
  int len = HDR_SIZE + send_buffer.hdr.len + 1;

  int sent_len = -1;
  if (!navilink_present) {
    sent_len = write(send_buffer.raw_data, len);
    // if the command was actually sent, then echo it to the client
    if (sent_len == len) {
      recv_buffer = send_buffer;
      parse_command();
    }
  } else if (on_error_cb) {
    on_error_cb(__func__, "Failed to send the command (navilink present):");
    Navien::print_buffer(send_buffer.raw_data, len, on_error_cb);
  }

  return sent_len;
}

int Navien::power(bool power_on) {
  PACKET_BUFFER send_buffer{};
  memcpy(&send_buffer, COMMAND_HEADER, sizeof(COMMAND_HEADER));

  if (power_on) {
    send_buffer.cmd.system_power = 0x0a;
  } else {
    send_buffer.cmd.system_power = 0x0b;
  }
  uint8_t crc = Navien::checksum(send_buffer.raw_data, HDR_SIZE + send_buffer.hdr.len, CHECKSUM_SEED_62);
  send_buffer.raw_data[HDR_SIZE + send_buffer.hdr.len] = crc;

  if (test_mode) {
    state.water[0].system_power = power_on;
    return(HDR_SIZE + send_buffer.hdr.len);
  }

  // Queue the command
  return queue_send_cmd(send_buffer);
}


int Navien::setTemp(float temp_degC) {
  PACKET_BUFFER send_buffer{};
  memcpy(&send_buffer, COMMAND_HEADER, sizeof(COMMAND_HEADER));

  send_buffer.cmd.set_temp = int(temp_degC * 2.0);

  uint8_t crc = Navien::checksum(send_buffer.raw_data, HDR_SIZE + send_buffer.hdr.len, CHECKSUM_SEED_62);
  send_buffer.raw_data[HDR_SIZE + send_buffer.hdr.len] = crc;

  if (test_mode) {
    state.gas.set_temp = temp_degC;
    state.water[0].set_temp = temp_degC;
    return(HDR_SIZE + send_buffer.hdr.len);
  }

  // Queue the command
  return queue_send_cmd(send_buffer);
}


int Navien::hotButton() {
  PACKET_BUFFER send_buffer{};
  memcpy(&send_buffer, COMMAND_HEADER, sizeof(COMMAND_HEADER));

  send_buffer.cmd.hot_button_recirculation = Navien::HOT_BUTTON_DOWN;

  uint8_t crc = Navien::checksum(send_buffer.raw_data, HDR_SIZE + send_buffer.hdr.len, CHECKSUM_SEED_62);
  send_buffer.raw_data[HDR_SIZE + send_buffer.hdr.len] = crc;

  // Queue the command
  int sent_len = queue_send_cmd(send_buffer);

  // queue up the button release command
  memcpy(&send_buffer, COMMAND_HEADER, sizeof(COMMAND_HEADER));
  send_buffer.cmd.hot_button_recirculation = 0x00;
  crc = Navien::checksum(send_buffer.raw_data, HDR_SIZE + send_buffer.hdr.len, CHECKSUM_SEED_62);
  send_buffer.raw_data[HDR_SIZE + send_buffer.hdr.len] = crc;

  return queue_send_cmd(send_buffer);
}

int Navien::recirculation(bool recirc_on) {
  PACKET_BUFFER send_buffer{};
  memcpy(&send_buffer, COMMAND_HEADER, sizeof(COMMAND_HEADER));

  send_buffer.cmd.hot_button_recirculation = recirc_on ? Navien::RECIRCULATION_ON : Navien::RECIRCULATION_OFF;

  uint8_t crc = Navien::checksum(send_buffer.raw_data, HDR_SIZE + send_buffer.hdr.len, CHECKSUM_SEED_62);
  send_buffer.raw_data[HDR_SIZE + send_buffer.hdr.len] = crc;

  // Queue the command
  int sent_len = queue_send_cmd(send_buffer);

  // queue up the button release command
  memcpy(&send_buffer, COMMAND_HEADER, sizeof(COMMAND_HEADER));
  send_buffer.cmd.hot_button_recirculation = 0x00;
  crc = Navien::checksum(send_buffer.raw_data, HDR_SIZE + send_buffer.hdr.len, CHECKSUM_SEED_62);
  send_buffer.raw_data[HDR_SIZE + send_buffer.hdr.len] = crc;

  if (test_mode) {
    state.water[0].recirculation_active = recirc_on;
    state.water[0].recirculation_running = recirc_on;
    state.gas.current_gas_usage = recirc_on ? 200 : 0;
    state.water[0].operating_capacity = recirc_on ? 15 : 0;
    sent_len = HDR_SIZE + send_buffer.hdr.len;
  }

  return queue_send_cmd(send_buffer);
}

uint8_t Navien::checksum(const uint8_t *buffer, uint8_t len, uint16_t seed) {
  uint16_t result;

  if (len < 2) {
    result = 0x00;
  } else {
    result = 0xff;

    for (int i = 0; i < len; i++) {
      result = result << 1;

      if (result > 0xff) {
        result = (result & 0xff) ^ seed;
      }

      // this is important!!
      // the checksum is calculated
      // based on the lower byte, i.e.
      // only the lower byte is XOR-ed
      result = ((uint8_t)result) ^ (uint16_t)buffer[i];
    }
  }
  return result;
}
