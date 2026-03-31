![Github License](https://img.shields.io/github/license/dacarson/NavienManager) ![Github Release](https://img.shields.io/github/v/release/dacarson/NavienManager?display_name=tag)

# NavienManager

**Turn your Navien tankless water heater into a smart home appliance — no NaviLink required.**

NavienManager is an ESP32-based bridge that connects your Navien tankless water heater directly to Apple HomeKit. Control recirculation with Siri, schedule it intelligently, visualize live data on a Grafana dashboard, and even let the system *learn your household's hot water habits* and build a custom schedule automatically.

---

## What you can do with it

### "Hey Siri, turn on Hot Water"
A dedicated **Hot Water** switch in HomeKit lets you trigger a 5-minute recirculation burst with a voice command. No more waiting at the tap — just ask Siri, wait 30 seconds, and enjoy hot water instantly.

### Set a smart recirculation schedule
Use the **Eve app** to define a weekly recirculation schedule — morning showers, evening dishes, whatever fits your routine. The scheduler runs recirculation only when you actually need it, keeping gas and water bills in check.

### Let the system learn your schedule for you
A companion Python script (`navien_schedule_learner.py`) queries your InfluxDB history, identifies when your household typically turns on hot water after the pipes have gone cold, and pushes a learned recirculation schedule directly to the ESP32. Run it as a weekly cron job and the schedule stays in sync with your evolving habits automatically.

### Vacation Mode — set it and forget it
Tell the Eve app you're on vacation. The heater powers off and all recirculation stops. When you're back, re-enable it and the schedule picks up exactly where it left off.

### Monitor everything in real time
- A live **web dashboard** shows current temperatures, flow rate, gas usage, recirculation state, scheduler status, and more.
- A **Telnet CLI** lets you inspect raw packet data, tweak settings, and trace live bus traffic — useful for diagnostics and protocol research.
- A **UDP broadcast** stream feeds live JSON data to any listener on your local network, ready to log to InfluxDB and visualize in Grafana.

---

## How it works

NavienManager connects to the Navien RS485 bus (the same two-wire bus the NaviLink module uses) and speaks the Navien proprietary serial protocol natively. It operates in one of two modes:

**Monitor Mode** — if a NaviLink is already attached, the ESP32 passively observes all traffic and reports state to HomeKit without interfering. Your existing NaviLink setup is untouched.

**Control Mode** — if there is no NaviLink, the ESP32 takes ownership of the bus and actively controls power, set-point temperature, and recirculation in response to HomeKit commands and the built-in scheduler.

---

## HomeKit integration

NavienManager exposes two HomeKit accessories:

### Thermostat
Models the water heater as a thermostat so it appears naturally in the Home app and Eve app.

| What you see | What it means |
|---|---|
| **HEAT** | Recirculation is active or the heater is actively burning gas |
| **OFF** | System is idle |
| Set-point temperature | The heater's configured hot water temperature |
| **Valve / Actuation** graph | Burner operating capacity (0–100%) — shows exactly how hard the heater is working |

The temperature graph in Eve gives a clean on/off history: the set-point drops to the minimum when idle, rises to your configured temperature when heating. The Valve graph tells you at a glance whether the heater was working lightly or at full capacity.

> **Tip:** The Valve characteristic is hidden by default in Eve. Edit the Thermostat page and add it manually to see the operating capacity graph.

<img width="500" alt="Eve thermostat screen" src="https://github.com/user-attachments/assets/bdcb70b5-9ed8-47b2-a24c-a1740a0730ab" />

### Hot Water switch
A simple on/off switch that triggers a 5-minute recirculation override — perfect for Siri voice commands and HomeKit automations. The switch turns itself off automatically when the override expires or recirculation stops, so it always reflects the true state of the heater.

---

## Intelligent schedule learning

The included `Logger/navien_schedule_learner.py` script turns your InfluxDB history into a custom recirculation schedule:

1. Queries historical `consumption_active` and `recirculation_running` data at 10-second resolution.
2. Identifies **cold-start events** — the first hot-water tap after pipes have gone cold — as the moments pre-heating is most valuable.
3. Weights events by **recency** (this year counts more than last year) and **demand quality** (a long genuine tap counts more than a brief accidental one). Weights are further scaled by the actual dollar cost of a cold-start (wasted water) versus a wasted recirc cycle (wasted gas).
4. Applies a **rolling seasonal window** (±4 weeks around today's date) so your summer schedule adapts to summer habits and your winter schedule adapts to winter ones.
5. Finds **activity peaks** using a smoothed local-maximum algorithm and builds ±30-minute windows around each peak, shifted back by a configurable preheat margin.
6. POSTs the resulting schedule (up to 4 time slots per day, Sunday–Saturday) to the ESP32 over HTTP — no manual entry required.

Run with `--verbose` to see a per-day breakdown of peaks found and an efficiency table showing what fraction of schedulable cold-starts the proposed schedule would cover and the estimated dollar cost of any misses.

```
# Preview without pushing
python3 navien_schedule_learner.py --verbose

# Push to the ESP32
python3 navien_schedule_learner.py --push

# Cron: every Sunday at 2am
0 2 * * 0  /home/pi/navien/venv/bin/python3 /home/pi/navien_schedule_learner.py --esp32_host navien.local --push
```

---

## Web dashboard

Visit `http://<esp32-ip-address>` to see a live status page for the heater. Toggle between metric and imperial units with the button at the top. The page auto-refreshes every 60 seconds.

<img width="1488" alt="Navien Status screen" src="https://github.com/user-attachments/assets/fa161464-81c6-4bcd-b79f-f4b13bdf4a2d" />

---

## Grafana + InfluxDB logging

The `Logger/` folder contains a script that listens for the UDP broadcast stream and writes all data to InfluxDB, plus a Grafana dashboard template to visualize it. Once set up, you get a full historical record of temperatures, gas usage, flow rates, and recirculation events — the same data the schedule learner uses.

<img width="1496" alt="Grafana status" src="https://github.com/user-attachments/assets/4846a6d5-f917-4f56-9773-adb856c933ff" />

---

## Telnet CLI

Connect to the ESP32 on port 23 for a full command-line interface:

| Category | Commands |
|---|---|
| Diagnostics | `gas`, `water`, `trace [gas\|water\|command\|announce]`, `stop` |
| Control | `power on\|off`, `recirc on\|off`, `setTemp <°C>`, `hotButton` |
| Scheduler | `scheduler on\|off`, `timezone <tz>` |
| System | `wifi`, `memory`, `fsStat`, `time`, `reboot`, `bye` |
| History | `history [N]`, `eraseHistory`, `erasePgm` |

---

## Hardware setup

You need an **ESP32** and an **RS-485 adapter**. The ESP32 connects to the Navien NaviLink port (RJ45) using the two RS485 signal wires — the same wires a NaviLink module would use.

To tap into the bus without removing an existing NaviLink, use an RJ45 breakout box inline. The two middle pins of the NaviLink cable carry the RS485 A+ and B− signals.

<img width="500" alt="RJ45 breakout box" src="https://github.com/user-attachments/assets/d5f5c30d-4ed4-4dfe-a96a-0cd06035718e" />

### Parts list

| Component | Description | Link |
|---|---|---|
| **ESP32 D1 Mini** | Compact ESP32 dev board | [AliExpress](https://www.aliexpress.us/item/3256806227686284.html) |
| **RS-485 CANBUS DCDC Shield** | RS-485 + power shield for D1 Mini | [Taaralabs](https://taaralabs.eu/rs485-canbus-dcdc-wemos-mini/) |
| **RJ45 Ethernet Breakout Box** | Inline tap for the NaviLink connection | [Amazon](https://www.amazon.com/dp/B0CJM8BVWL) |

The RS-485 shield also includes a DC-DC converter that can draw power from the Navien unit directly — check whether your model provides power on the NaviLink cable.

---

## Software setup

### Dependencies
- [HomeSpan](https://github.com/HomeSpan/HomeSpan) — HomeKit accessory library for ESP32
- [ArduinoJson](https://arduinojson.org) — JSON parsing
- [ESPTelnet](https://github.com/LennartHennigs/ESPTelnet) — Telnet server

Flash the project using the Arduino IDE or `arduino-cli`. HomeSpan handles WiFi provisioning and HomeKit pairing on first boot.

### Logger dependencies
```
pip3 install influxdb requests
```

### Source layout

| File | Purpose |
|---|---|
| `NavienManager.ino` | Main startup and main loop |
| `Navien.*` | RS485 packet parsing and command sending |
| `DEV_Navien.*` | HomeKit thermostat and Hot Water switch |
| `FakeGatoScheduler.*` | Eve schedule parsing, built on `SchedulerBase` |
| `SchedulerBase.*` | Generic day-of-week scheduler |
| `FakeGatoHistoryService.*` | Eve history protocol |
| `HomeSpanWeb.*` | Live status web page |
| `NavienBroadcaster.*` | UDP broadcast of live packet data |
| `TelnetCommands.*` | Telnet CLI commands |
| `Logger/` | UDP listener, InfluxDB logger, Grafana templates, schedule learner |

---

## Compatibility

Built and tested against a **Navien NPE-240A**. The RS485 protocol is shared across the NPE-A series and likely works with other Navien tankless models that use the NaviLink interface, though this has not been independently verified for every model.

---

## Acknowledgements

- Community members of [Home Assistant](https://community.home-assistant.io/t/navien-esp32-navilink-interface/720567/170) who decoded the Navien RS485 protocol and packet structure
- [htumanyan](https://github.com/htumanyan/navien) for the well-crafted base Navien C++ class
- [simont77](https://github.com/simont77/fakegato-history) for decoding the Eve history logging interface
- [HomeSpan](https://github.com/HomeSpan/HomeSpan) for the HomeKit accessory library for ESP32

---

## License

[MIT License](https://opensource.org/license/mit/)
