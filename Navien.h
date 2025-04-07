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

#include <cstddef>
#include <cstdint>
#include <HardwareSerial.h>

#ifndef Navien_h
#define Navien_h

/* Navien RS424 / NaviLink interface
** Usage:
* The class inherits from HardwareSerial, so it needs to be constructed with
* a Serial port number, and when it is told to begin(), Serial IO pin numbers
* need to be provided.
*     Navien navienSerial(2);  // Serial Port 2
*
*     #define RXD2 16 // Second serial port on WeMos D1 Mini ESP32 board
*     #define TXD2 17 
*     navienSerial.begin(RXD2, TXD2);
*
* To be notified of observed data, you need to register for the respective
* callback. Note that only the structure for the current callback should be
* considered valid. With Gas Packer callback, only state.gas structure 
* should be examined. 
* Callback functions must be defined as:
*   void onStatePacket(Navien::NAVIEN_STATE *state) {}
* For example:
*     navienSerial.onGasPacket(onGasPacket);
*     navienSerial.onWaterPacket(onWaterPacket);
*     navienSerial.onCommandPacket(onCommandPacket);
*     navienSerial.onAnnouncePacket(onAnnouncePacket);
* For errors, the callback function is defined as:
*   void onError(const char *function, const char *errorMessage) {}
* To register call:
*     navienSerial.onError(onError);
*
* Then in the loop, make sure to call the loop function of navien so that
* it can process any packets.
*     void loop() {
*       navienSerial.loop();
*     }
*
** Basic class operation:
* Observer operation.
* In the loop() function, the class builds up a recieve packet, PACKET_BUFFER
* until it is either a full packet (defined by the packet length in the header)
* or it is too large for the buffer. 
* Once a full packet is recieved, it determines the type of packet, and then
* parses it into the correct NAVIEN_STATE object.
* If a Callback is defined for the packet type, it is called with the 
* recieved packet. The raw packet is also available while in the callback
* function.
*
* Publisher operation.
* Sending commands is only available if a NaviLink device is not detected. If
* one is detected, commands are not sent as it may confuse the state of the 
* hot water unit and the NaviLink unit.
* Calling any of the control functions, the hot water unit state will be 
* adjusted.
*/

class Navien : public HardwareSerial {
public:
  typedef struct {
    /**
   * Unknown value, but could be the protocol version (very likely)
   */
    uint8_t packet_marker;

    /**
   * Unknown value, but could be the protocol version (very likely)
   */
    uint8_t unknown_0x05;

    /**
   * Direction of the packet. Not unlikely to be "recipient address" essentially.
   * Needs to be captured in multi-unit installation to see if it is simply a direction or a version.
   *
   * Navien to control device - 0x50, PACKET_DIRECTION_STATUS
   * Control Device to Navien - 0x0F, PACKET_DIRECTION_CONTROL
   */
    uint8_t direction;

    /**
   * There are two known packet types with somewhat overlapping
   * data but also with unique data points in each
   * 0x50 - water flow and temperature data - PACKET_TYPE_WATER
   * 0x0F - gas flow and also water temperature - PACKET_TYPE_GAS
   */
    uint8_t packet_type;

    /**
   * Unknown value. Observed to be 0x90 for navien-device direction
   * and 0x10 in device-to-navien (control packets).
   */
    uint8_t unknown_0x90;

    /**
   * Length of the packet including the header and checksum, i.e. total number of bytes
   * in the packet, including everything
   */
    uint8_t len;
  } HEADER;

  static const uint8_t HDR_SIZE = sizeof(HEADER);
  static const uint8_t PACKET_MARKER = 0xF7;
  // Default send command header
  static const uint8_t COMMAND_HEADER[];

enum PacketDirection {
  PACKET_DIRECTION_CONTROL = 0x0F,
  PACKET_DIRECTION_STATUS = 0x50
};

enum PacketType {
  PACKET_TYPE_WATER = 0x50,
  PACKET_TYPE_GAS = 0x0F
};

enum ControlPacketType {
  CONTROL_ANNOUNCE = 0x4A,
  CONTROL_COMMAND = 0x4F
};

enum CommandActionSystemPower {
  SYSTEM_POWER_ON = 0x0A,
  SYSTEM_POWER_OFF = 0x0B
};

enum CommandActionHotButton {
  HOT_BUTTON_DOWN = 0x1,
  RECIRCULATION_ON = 0x8,
  RECIRCULATION_OFF = 0x10
};

  static const uint16_t CHECKSUM_SEED_4B = 0x4b;
  static const uint16_t CHECKSUM_SEED_62 = 0x62;


  typedef struct {
    uint8_t cmd_type;  // Always 0x42 Water
    uint8_t unknown_07; // 0x00
    uint8_t flow_state;  // 8 0x20 if Consumption is Active 0x08 if recirculating_running
    uint8_t system_power;  // 9
    uint8_t unknown_10;    // 0x37
    uint8_t set_temp;      // 11
    uint8_t outlet_temp;   // 12
    uint8_t inlet_temp;    // 13
    uint8_t unknown_14; // 0x00
    uint8_t unknown_15; // 0x00
    uint8_t unknown_16; // 0x00
    uint8_t operating_capacity;  // 17
    uint8_t water_flow;          // 18
    uint8_t unknown_19; // 0x00
    uint8_t unknown_20; // 0x88
    uint8_t unknown_21; // 0xC2
    uint8_t unknown_22; // 0x00
    uint8_t unknown_23; // 0x20
    uint8_t system_status;  // 24
    uint8_t unknown_25; // 0x00
    uint8_t unknown_26; // 0x00
    uint8_t unknown_27; // 0x00 0x01 varies
    uint8_t unknown_28; // Counter A_lo - Seems to match Gas Counter C (* 12.015)
    uint8_t unknown_29; // Counter A_hi
    uint8_t unknown_30; // Counter B_lo
    uint8_t unknown_31; // Counter B_hi
    uint8_t unknown_32; // 0x00
    uint8_t recirculation_enabled;  // 33
    uint8_t unknown_34; // 0x00
    uint8_t unknown_35; // 0x00
    uint8_t unknown_36; // 0x00
    uint8_t unknown_37; // 0x00
    uint8_t unknown_38; // 0x00
    uint8_t unknown_39; // 0x00
  } WATER_DATA;

  typedef struct {
    uint8_t cmd_type; // Always 0x45 Gas
    uint8_t unknown_07; // 0x00
    uint8_t unknown_08; // 0x01
    uint8_t unknown_09; // 0x01
    uint8_t controller_version_lo;  // 10
    uint8_t controller_version_hi;  // 11
    uint8_t panel_version_lo;  // 12
    uint8_t panel_version_hi;  // 13
    uint8_t set_temp;     // 14
    uint8_t outlet_temp;  // 15
    uint8_t inlet_temp;   // 16
    uint8_t unknown_17; // 0x00
    uint8_t unknown_18; // 0x00
    uint8_t unknown_19; // 0x00
    uint8_t unknown_20; // Same current_gas_usage * 250, but has some info prefex, also extremely similar to operating_capactiy
    uint8_t unknown_21; // 0x01
    uint8_t current_gas_lo;     // 22
    uint8_t current_gas_hi;     // 23
    uint8_t cumulative_gas_lo;  // 24
    uint8_t cumulative_gas_hi;  // 25
    uint8_t unknown_26; // 0x00
    uint8_t unknown_27; // 0x00
    uint8_t unknown_28; // Counter A_lo
    uint8_t unknown_29; // Counter A_hi
    uint8_t cumulative_domestic_usage_cnt_lo; // 30 Domestic Usage Counter in 10 usage increments
    uint8_t cumulative_domestic_usage_cnt_hi; // 31
    uint8_t unknown_32; // 0x9E 0x01 0xB7 0x46
    uint8_t unknown_33; // Counter C_lo - Seems to match Water Counter A (/ 12.015)
    uint8_t unknown_34; // Counter C_hi
    uint8_t unknown_35; // 0x00
    uint8_t total_operating_time_lo;  // 36
    uint8_t total_operating_time_hi;  // 37
    uint8_t unknown_38; // 0x00
    uint8_t unknown_39; // 0x00
    uint8_t unknown_40; // 0x00
    uint8_t unknown_41; // 0x00
    uint8_t unknown_42; // 0xA6
    uint8_t unknown_43; // 0x49
    uint8_t unknown_44; // 0x00
    uint8_t unknown_45; // 0x00
    uint8_t unknown_46; // 0x01
    uint8_t unknown_47; // 0x00
    // length is 47, ie 0 - 46
  } GAS_DATA;

  typedef struct {
    uint8_t cmd_type; // Always 4F Command
    uint8_t unknown_07;    // 00
    uint8_t system_power;  // 08 0a == On, 0b == Off
    uint8_t set_temp;         // 09 5E == 117F
    uint8_t unknown_10;       // 0x00
    uint8_t hot_button_recirculation;  // 11 CommandActionHotButton
    uint8_t cmd_data;         // 12 Seems to vary
    uint8_t unknown_13;       // 0x00
    uint8_t unknown_14;       // 0x00
    uint8_t unknown_15;       // 0x00
    uint8_t unknown_16;       // 0x00
    uint8_t unknown_17;       // 0x00
    // cmd_type (4F) length is
  } CMD_DATA;

  typedef struct {
    uint8_t cmd_type; // Always 4A Announce
    uint8_t unknown_07;  // 0x00
    uint8_t unknown_08;  // 0x01
    // cmd_type (4A) length is 9, ie 0 - 8
  } ANNOUNCE_DATA;

  typedef union {
    struct {
      HEADER hdr;
      union {
        GAS_DATA gas;
        WATER_DATA water;
        CMD_DATA cmd;
        ANNOUNCE_DATA announce;
      };
    };
    uint8_t raw_data[128];
  } PACKET_BUFFER;

  static const uint8_t commandHeader[];

  // Parsed known values from the respective packets
  // Structure is updated before calling the callback functions.
  typedef struct {
    struct {
      bool system_power;
      float set_temp; // degree C
      float outlet_temp; // degree C
      float inlet_temp; // degree C
      float flow_lpm; // Water flow velocity, via Recirculation or Tap being on 
      bool recirculation_active; // Recirculation mode is current ON
      bool recirculation_running; // Recirculation pump is currently running
      bool display_metric; // True == degree C; False == degree F
      bool schedule_active;
      bool hotbutton_active;
      float operating_capacity; // Percentage 0.0 - 100.0 %
      bool consumption_active; // Tap is turned on
      uint8_t flow_state;
    } water;

    struct {
      float set_temp;  // degree C
      float outlet_temp; // degree C
      float inlet_temp; // degree C
      float controller_version;
      float panel_version;
      float accumulated_gas_usage;    // m^3 (ccf = m^3 / 2.832, Therms = m^3 / 2.832 * 1.02845 )
      uint16_t current_gas_usage;     // kcal (btu == kcal * 3.965667)
      uint32_t total_operating_time;  // minutes 
      uint32_t accumulated_domestic_usage_cnt;  // Counter for domestic usage, increments every 10 usages
    } gas;

    struct {
      bool power_command; // Observe the power on command
      bool power_on; // Power On command, True == turn On; False == turn OFF

      bool set_temp_command; // Observe the Set Temperature command
      float set_temp; // Set the Set Temperature to this value in deg. C

      bool hot_button_command; // Send Hot Button command

      bool recirculation_command; // Observe the recirculation command
      bool recirculation_on; // Recirculation command, True == turn On; False == turn OFF

      uint8_t cmd_data;
    } command;

    struct {
      bool navilink_present; // Navilink unit is present controlling the Navien
    } announce;

  } NAVIEN_STATE;

  typedef enum {
    INITIAL,
    MARKER_FOUND,
    HEADER_PARSED
  } READ_STATE;

enum NavienTemperatureRange {
    // Navien 240A has temperature range for domestic hot water (DHW) of 37degC to 60degC
  TEMPERATURE_MIN = 37,
  TEMPERATURE_MAX = 60
};

  typedef void (*PacketCallbackFunction)(NAVIEN_STATE *state);
  typedef void (*ErrorCallbackFunction)(const char* functionName, const char* error);

public:
  Navien(uint8_t uart_nr)
    : HardwareSerial(uart_nr), navilink_present(false), test_mode(true), recv_state(INITIAL) {}
  
  /* Navien is 19200 baud, 8 bit, no parity, 1 stop bit */
  void begin(int8_t rxPin, int8_t txPin) { HardwareSerial::begin(19200, SERIAL_8N1, rxPin, txPin); }

  // Call this in the loop function
  void loop();

  // Set the callback functions, called when packet is processed
  void onGasPacket(PacketCallbackFunction f) {
    on_gas_packet_cb = f;
  }
  void onWaterPacket(PacketCallbackFunction f) {
    on_water_packet_cb = f;
  }
  void onCommandPacket(PacketCallbackFunction f) {
    on_command_packet_cb = f;
  }

  void onAnnouncePacket(PacketCallbackFunction f) {
    on_announce_packet_cb = f;
  }

  // Set the error callback function
  void onError(ErrorCallbackFunction f) {
    on_error_cb = f;
  }

  // Get the current state
  const NAVIEN_STATE *currentState() {
    return &state;
  }

  // Only valid while inside a packet callback function
  const PACKET_BUFFER *rawPacketData() {
    return &recv_buffer;
  }

// Control functions
// These are only available if there is no NaviLink control unit attached

  // Control available
  // Returns true if control is available
  bool controlAvailable() { return !navilink_present; }

  // Turn on or off the unit
  // Return number of bytes sent, -1 on failure
  int power(bool on);

  // Change the set point. Value specified in degC, 
  // rounded to nearest 0.5 degree
  // Return number of bytes sent, -1 on failure
  int setTemp(float temp_degC);

  // Press the hot button
  // Return number of bytes sent, -1 on failure
  int hotButton();

  // Turn on or off Recirculation
  // Return number of bytes sent, -1 on failure
  int recirculation(bool on);


protected:
  // Debug helper to print hex buffers
  static void print_buffer(const uint8_t *data, size_t length, ErrorCallbackFunction on_error_cb);

  // Write the send_buffer command
  // Returns number of bytes sent, or -1 for failure
  int send_cmd();

  static float t2c(uint8_t);         // Temperature to degC
  static float flow2lpm(uint8_t f);  // Flow to LPM

  /**
   * 
   */
  static uint8_t checksum(const uint8_t *buffer, uint8_t len, uint16_t seed);

  // Callback functions that will be called when packets are received
  PacketCallbackFunction on_gas_packet_cb = NULL;
  PacketCallbackFunction on_water_packet_cb = NULL;
  PacketCallbackFunction on_command_packet_cb = NULL;
  PacketCallbackFunction on_announce_packet_cb = NULL;

  // General error callback function to call if an error is encountered.
  ErrorCallbackFunction on_error_cb = NULL;
  

protected:
  bool seek_to_marker();


  /**
   * Data extraction routines.
   * Copy the raw data from PACKET_BUFFER to internal representation
   * in this->state where it is stored and reported upon "update" calls.
   */

  // Common entry point that is always called upon receipt of a valid packet
  // calls parse_water/gas depending on the paket type
  void parse_packet();

  // Called when we observe a control packet from another
  // control device (not us).
  void parse_control_packet();

  // Called when we receive a status packet from Navien device
  void parse_status_packet();

  // Called when the direction is from Navien to reporting device
  // and type field is PACKET_TYPE_WATER
  void parse_water();

  // Called when the direction is from Navien to reporting device
  // and the type field is PACKET_TYPE_GAS
  void parse_gas();

  // Called when the direction reporting device to Navien
  // and the cmd_type field is CONTROL_ANNOUNCE
  void parse_announce();

  // Called when the direction reporting device to Navien
  // and the cmd_type field is CONTROL_COMMAND
  void parse_command();

  // Generate a command packet. Command packets are always 4F (79)
  // in length.
  void prepare_command_send();

protected:
  // Keeps track of the state machine and iterates through
  // initialized -> marker found -> header parsed -> data parsed -> initialized
  READ_STATE recv_state;

  // Data received off the wire
  PACKET_BUFFER recv_buffer;

  // Data to send over the wire
  PACKET_BUFFER send_buffer;

  // Data, extracted from gas and water packers and stored
  NAVIEN_STATE state;

  // Control available
  bool navilink_present;

  // Assume running in test mode *until* we see a packet
  bool test_mode;
};

#endif  // Navien_h