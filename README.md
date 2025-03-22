![Github License](https://img.shields.io/github/license/dacarson/NavienManager) ![Github Release](https://img.shields.io/github/v/release/dacarson/NavienManager?display_name=tag)

# NavienManager
 HomeKit and diagnostics for Navien NPE-240A (though may work with others) using ESP32 + RS485

## Why
1. Allows for vacation mode
2. Allows for hot water now

## Description
Two modes of operation. 
- Monitor mode - a NaviLink Wifi box is attached and controlling the Navien tankless hotwater unit.
- Control mode - NaviLink box is not attached, and can be controlled via HomeKit.

The features of this software are:
### Homekit integration, Eve app integration.
   Eve app, Hot Water. Valve position is hidden by default.
   
### Web server (via HomeSpan)
### Telnet
   * bye - Disconnect
   * wifi - Print WiFi status
   * reboot - Reboot ESP32
   * ping - Test if telnet commands are working

  * eraseHistory - Erase all history entries
  * erasePgm - Erase all Program State
  * history - Print history entries in CSV format (optional: number of entries)
  
  * gas - Print current gas state as JSON
  * water - Print current water state as JSON
  * trace - Dump interactions (options: gas/water/command/announce)
  * stop - Stop tracing
  
  * control - Check if control commands are available
  * hotButton - Send hot button command
  * power - Set or get power state (on/off)
  * recirc - Set or get recirculation state (on/off)
  * setTemp - Set or get set point temperature
  
  * time - Print local and gmt time
  * timezone - Set or get current timezone
  * fsStat - File system status
  
### Data broadcast (UDP)


## Setup
### Hardware
Requires a ESP32 unit and a RS-485 to serial unit. I personally used these components:
1. Wemos Mini D1 ESP32
2. RS485 board for Mini D1
3. RJ-45 break out block


### Software
The code is broken up logically by functionality.
1. NavienManager - Main startup file.
2. Navien - Handles communication to the Navien Hot Water unit
3. HomeSpanWeb - Contains all the web functions for HomeSpan to present a pretty web page
4. DEV_Navien - HomeKit Themostat Service implementation
5. FakeGatoScheduler - Handles the Eve specific Program Data, built on top of SchedulerBase, used in conjection with DEV_Navien
6. SchedulerBase - generic baseclass to implement a day-of-week scheduler. 
7. FakeGatoHistoryService - HomeKit FakeGatoHistory Service implementation
8. NavienBroadcaster - Receives callbacks from Navien of state changes and broadcasts over UDP
9. TelnetCommands - Implements all the telnet commands.
10. Logging - Folder contains the scripts listen for Navien broadcasts and write them to InfluxDB as well as Grafana templates


## License
This library is licensed under [MIT License](https://opensource.org/license/mit/)
