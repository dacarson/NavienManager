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
  
## Features
The features of this software are:
### Homekit integration, Eve app integration.
   Eve app, Hot Water. Valve position is hidden by default.
   
### Web server (via HomeSpan)
The current status of the Navien unit can be viewed at any time by browsing to it's web page. The webpage is hosted at the IP address of the ESP32, ie `http://<ip-address>/status`. 

### Telnet
   * `bye` - Disconnect
   * `wifi` - Print WiFi status
   * `reboot` - Reboot ESP32
   * `ping` - Test if telnet commands are working

  * `eraseHistory` - Erase all history entries
  * `erasePgm` - Erase all Program State
  * `history` - Print history entries in CSV format (optional: number of entries)
  
  * `gas` - Print current gas state as JSON
  * `water` - Print current water state as JSON
  * `trace` - Dump interactions (options: gas/water/command/announce)
  * `stop` - Stop tracing
  
  * `control` - Check if control commands are available
  * `hotButton` - Send hot button command
  * `power` - Set or get power state (on/off)
  * `recirc` - Set or get recirculation state (on/off)
  * `setTemp` - Set or get set point temperature
  
  * `time` - Print local and gmt time
  * `timezone` - Set or get current timezone
  * `fsStat` - File system status
  
### Data broadcast (UDP)
To continuously monitor and log the status of the Navien unit, the software broadcasts its status over the local network (not the internet) using UDP on port `2025`. Duplicate data is throttled to one broadcast every 5 seconds—if the same packet is observed again within that time, it is dropped. However, if the packet changes in any way, it will be broadcast immediately.  
This broadcasted data can be collected on another machine and used for logging. In the `logging` folder, there is an example script that collects the broadcast data and logs it to InfluxDB. A Grafana template is also provided, which can be used in conjunction with InfluxDB to visualize the collected data.

## Setup
### Hardware
Requires a ESP32 unit and a RS-485 to serial unit. The components I personally used are below. My reasoning is that I have used ESP32 WeMos mini boards before, have a number of [sheilds](https://www.wemos.cc/en/latest/d1_mini_shield/index.html) for them, and found them easy to develop for using the Arduino SDK. I found a RS-485 shield for the wemos, that works perfectly with the D1 Mini. It not only provides the required RS-485 interface, but also has a DCDC adapter allowing me to draw power from the Navien (I am not yet taking advantage of this as my unit *does not* have power on the NaviLink cable). Then the breakout box. My Navien already had the NaviLink unit attached, so I wanted to start by monitoring the communications between the NaviLink unit and the Navien Hot Water unit. Attaching the breakout box in line allowed me to do so. 
1. [D1 mini ESP32 ESP-32](https://www.aliexpress.us/item/3256806227686284.html)
2. [RS-485 CANBUS DCDC WeMos d1 Mini32](https://taaralabs.eu/rs485-canbus-dcdc-wemos-mini/)
3. [RJ45 Ethernet Dual Female Terminal Breakout Board](https://www.amazon.com/dp/B0CJM8BVWL)


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
