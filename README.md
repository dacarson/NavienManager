![Github License](https://img.shields.io/github/license/dacarson/NavienManager) ![Github Release](https://img.shields.io/github/v/release/dacarson/NavienManager?display_name=tag)

# NavienManager
 HomeKit and diagnostics for Navien NPE-240A (though may work with others) using ESP32 + RS485

## Why
While the Navien Tankless Hot Water unit supports Wi-Fi via NaviLink, there are two important features it lacks:
1. **Vacation Mode**  
A way to automate vacation mode — either through a schedule with a defined start and end time, or by integrating with a system like HomeKit — is essential to me. Vacation mode prevents wasting gas on unnecessary recirculation. While I could manually turn the unit off before leaving and back on upon returning, automation allows me to “set it and forget it,” which is far more convenient and reliable.
2. **Hot Water Now + Recirculation Schedule**  
With NaviLink, I can configure a fixed schedule for recirculation. However, this does not work with Navien’s “Hot Button.” You can have one or the other — not both. I wanted a way to trigger recirculation for a fixed period outside of the scheduled times. This allows me to avoid wasting water while waiting for it to get hot.

## Description

My home is a HomeKit home, and I wanted to control the hot water unit through it. I decided to model the hot water unit as a thermostat. This allows me to say things like **“Hey Siri, turn on Hot Water Heater”** or create any number of scenes that activate recirculation. I’ve enabled two modes of operation:

### Monitor Mode
A NaviLink Wi-Fi box is attached and controls the Navien tankless hot water unit. Adjusting any HomeKit settings in this mode has no effect; they will simply revert to reflect the current state of the unit.

- If the HomeKit thermostat shows **HEAT**, it means recirculation is currently running, or that a tap is open and the unit is actively heating water. The setpoint reflects the unit’s configured temperature.
- If the thermostat shows **AUTO**, the system is idle, and the setpoint is set to the minimum value.

### Control Mode (WIP - still validating)
The NaviLink box is not attached, allowing full control via HomeKit. In this mode, a schedule can be defined using the Eve app. The setpoint should be controlled via Apple’s Home app, as Eve does not support the full temperature range of a hot water unit. The temperature set in the Home app overrides any setpoint from the Eve schedule.

- If the thermostat shows **AUTO**, it means a schedule is running but recirculation is currently inactive.
- Switching to **HEAT** activates recirculation for 5 minutes, after which it returns to the schedule.
- Switching to **OFF** turns off recirculation if it was running. The thermostat will return to **AUTO** when the next scheduled event occurs.
- If **Vacation Mode** is enabled via Automation in the Eve app, the hot water unit will be powered off and all schedules will be suspended.
  
## Features
The NavienManager has a number of features that came about as due process when working on a solution. I initially wanted to log the behaviour of the unit, which caused me to create a data broadcast component. Then I wanted to investigate the data that it was sending, which caused me to add Telnet support. Then as I was building out the HomeKit integration, I found that it didn't present all the information I wanted to see, so I built out a web interface. The core functionality for the RS-485 interface was built out by a team of people on [Home Assist](https://community.home-assistant.io/t/navien-esp32-navilink-interface/720567/170), with the foundation of the Navien C++ class by [htumanyan](https://github.com/htumanyan/navien)
### HomeKit Integration and Eve App

The HomeKit integration is described in the [Description](#description) section above, but there are a few additional points worth noting:

- You may see the setpoint fluctuate on the temperature graph. This is intentional and indicates when the unit is actively heating to the setpoint versus when it is idle. The lower setpoint represents the minimum temperature that the hot water unit can be set to.
- The **Valve** graph shows the current operating load. When the valve is open, the unit is actively heating, and the graph reflects its current operating capacity.
- The **Valve** metric is not shown by default in the Eve app. To enable it, edit the Thermostat page and manually add it as a visible characteristic.
  <img width="500" alt="Eve thermostat screen" src="https://github.com/user-attachments/assets/bdcb70b5-9ed8-47b2-a24c-a1740a0730ab" />

   
### Web Server (via [HomeSpan](https://github.com/HomeSpan/HomeSpan))

The current status of the Navien unit can be viewed at any time by visiting its web page. The webpage is hosted at the IP address of the ESP32, e.g., `http://<ip-address>/status`.

At the top of the status page is a green toggle button. Tapping it switches the units displayed on the screen between metric and imperial.

There are two schedulers shown:
1. The built-in Navien scheduler, which does **not** support "Hot Water Now" functionality.
2. A custom scheduler provided by this project, which **does** support "Hot Water Now" requests even while a schedule is running.

If both schedulers are active, the Navien scheduler takes precedence. In the image below, shows the system running in Monitor mode. Attempts to change the state of the hotwater unit are correctly failing.

<img width="1488" alt="Navien Status screen" src="https://github.com/user-attachments/assets/fa161464-81c6-4bcd-b79f-f4b13bdf4a2d" />


### Telnet

Because the hot water unit is not in a convenient location for active development, I added Telnet capabilities to the ESP32. This allows a user to investigate and control the hot water unit remotely, as well as monitor its activity in real time. The Telnet functionality is broken into several categories:

**General operation commands:**
- `bye` — Disconnect
- `wifi` — Print WiFi status
- `fsStat` — Show file system status
- `reboot` — Reboot the ESP32
- `ping` — Test if Telnet commands are working

**History and program state:**
- `eraseHistory` — Erase all history entries
- `erasePgm` — Erase all program state
- `history` — Print history entries in CSV format (optional: specify number of entries)

**Navien hot water monitoring:**
- `gas` — Print current gas state as JSON
- `water` — Print current water state as JSON
- `trace` — Dump interactions (options: `gas`, `water`, `command`, `announce`)
- `stop` — Stop tracing

**Navien control commands:**
- `control` — Check if control commands are available
- `hotButton` — Send Hot Button command
- `power` — Set or get power state (`on`/`off`)
- `recirc` — Set or get recirculation state (`on`/`off`)
- `setTemp` — Set or get temperature setpoint

**Time operations:**
- `time` — Print local and GMT time
- `timezone` — Set or get the current timezone
  
### Data broadcast (UDP)
To continuously monitor and log the status of the Navien unit, the software broadcasts its status over the local network (not the internet) using UDP on port `2025`. Duplicate data is throttled to one broadcast every 5 seconds—if the same packet is observed again within that time, it is dropped. However, if the packet changes in any way, it will be broadcast immediately.  
This broadcasted data can be collected on another machine and used for logging. In the `logging` folder, there is an example script that collects the broadcast data and logs it to InfluxDB. A Grafana template is also provided, which can be used in conjunction with InfluxDB to visualize the collected data.
<img width="1496" alt="Grafana status" src="https://github.com/user-attachments/assets/4846a6d5-f917-4f56-9773-adb856c933ff" />


## Setup
### Hardware

This project requires an **ESP32** and an **RS-485 to serial interface**. Below are the components I personally used, along with why I chose them.

I chose the **ESP32 D1 Mini** because I’ve used WeMos mini boards before, have a number of [shields](https://www.wemos.cc/en/latest/d1_mini_shield/index.html) for them, and find them easy to develop for using the Arduino SDK.

For the RS-485 interface, I found a **WeMos-compatible RS-485 shield** that works perfectly with the D1 Mini. It not only provides the required RS-485 connection, but also includes a **DC-DC converter** that can draw power from the Navien system.  
*(Note: I’m not yet using this feature, as my Navien unit **does not** provide power on the NaviLink cable.)*

To tap into the communication line, I used a **breakout box**. My Navien unit already had a NaviLink module installed, so I wanted to begin by passively monitoring the traffic between the NaviLink and the Navien unit. Placing the breakout box inline allowed me to do just that.
The middle two lines on the breakout box are the two lines I connected to the RS-485 sheild. In the photo below, the Blue wire is attached to the B- terminal of the RS-485 shield, and the Purple wire is attached to the A+ terminal.

<img width="500" alt="RJ45 breakout box" src="https://github.com/user-attachments/assets/d5f5c30d-4ed4-4dfe-a96a-0cd06035718e" />

#### 🧩 Parts List

| Component | Description | Link |
|----------|-------------|------|
| **ESP32 D1 Mini** | Compact ESP32 Dev Board | [AliExpress](https://www.aliexpress.us/item/3256806227686284.html) |
| **RS-485 CANBUS DCDC Shield** | RS-485 & Power Shield for D1 Mini | [Taaralabs](https://taaralabs.eu/rs485-canbus-dcdc-wemos-mini/) |
| **RJ45 Ethernet Breakout Box** | RJ45 Tap for intercepting NaviLink connection | [Amazon](https://www.amazon.com/dp/B0CJM8BVWL) |

---

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

## Acknowledgements
Community members of [Home Assist](https://community.home-assistant.io/t/navien-esp32-navilink-interface/720567/170), who decoded the protocol and much of the packet data for the Navien NaviLink connection  
[htumanya](https://github.com/htumanyan/navien) for the well-crafted base class for Navien  
[simont77](https://github.com/simont77/fakegato-history) for decoding the Eve History logging interface   
[HomeSpan](https://github.com/HomeSpan/HomeSpan) for the HomeKit accessory library for ESP32 devices
