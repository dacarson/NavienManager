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
The ESP32 runs an **on-device schedule learner** that watches your actual hot-water usage in real time, detects cold-start events, and recomputes the recirculation schedule every night — all without needing a Raspberry Pi or a recurring cron job. After a one-time bootstrap to seed it with your existing InfluxDB history, it runs autonomously and self-corrects as your habits change.

The web dashboard and Telnet CLI show live efficiency metrics — what fraction of your hot-water demand was pre-heated by the schedule, how that compares to the schedule's predicted coverage, and a per-day gap that highlights when habits have drifted.

### Vacation Mode — set it and forget it
Tell the Eve app you're on vacation. The heater powers off and all recirculation stops. When you're back, re-enable it and the schedule picks up exactly where it left off.

### Monitor everything in real time
- A live **web dashboard** shows current temperatures, flow rate, gas usage, recirculation state, scheduler status, learner efficiency, and more.
- A **Telnet CLI** lets you inspect raw packet data, tweak settings, trace live bus traffic, and view learner status — useful for diagnostics and protocol research.
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

### On-device learner (primary)

The ESP32 itself learns your household's hot-water habits and recomputes the recirculation schedule every night at midnight. No Raspberry Pi, no cron job, no cloud service required after the initial setup.

**How it works:**

1. Every RS-485 packet is observed for **cold-start events** — the first hot-water tap after pipes have been cold for at least 10 minutes. These are the moments where pre-heating is most valuable.
2. Each event is weighted by **demand quality** (a long genuine draw counts more than a brief accidental tap) and stored into a compact per-day, per-5-minute-bucket histogram on flash (`buckets.bin`).
3. Every night at midnight the device runs a **peak-finding pass** over the histogram, finds up to 3 dominant activity windows per day, and updates the active recirculation schedule immediately.
4. An **annual decay** (applied on Jan 1) gradually down-weights older data so recent habit changes win over stale history.

**Efficiency metrics** are tracked continuously and visible on the web dashboard and via `learnerStatus` in the Telnet CLI:

| Metric | What it means |
|---|---|
| **Predicted** | Fraction of demand buckets that fall inside (or within 15 min after) a scheduled slot |
| **Measured** | Fraction of actual cold-start events that had recirculation already running — rolling 4-week window |
| **Gap** | Predicted − Measured: green < 10%, amber 10–25%, red > 25% |

```
> learnerStatus
Learner Status
  Last recompute:  2026-04-01 00:02  (8h ago)
  Bucket fill:     1621 / 2016 non-zero (80.4%)

  Day         Predicted  Measured   Gap      Cold-starts (4wk)
  -----------------------------------------------------------------
  Sunday         79.1%      76.3%     -2.8%   18
  Monday         83.3%      81.0%     -2.3%   22
  ...
```

### One-time bootstrap

Without seeding, the on-device learner starts cold and needs 2–4 weeks to accumulate enough data for a meaningful schedule. The bootstrap process eliminates this wait:

```bash
# Step 1 — push a schedule derived from your full InfluxDB history
python3 Logger/navien_bootstrap.py --push

# Step 2 — seed the on-device bucket histogram with the same history
python3 Logger/navien_bucket_export.py --push
```

Run both steps once after first flash (or after a LittleFS wipe). After that, the Pi cron job can be disconnected permanently.

### Pi-based learner (optional / legacy)

The original `Logger/navien_schedule_learner.py` script is still included for reference or for one-off pushes. It queries InfluxDB, runs peak-finding on the Pi, and POSTs the result to the ESP32:

```bash
# Preview without pushing
python3 Logger/navien_schedule_learner.py --verbose

# Push to the ESP32
python3 Logger/navien_schedule_learner.py --push
```

Run with `--verbose` to see a per-day breakdown of peaks found and an efficiency table showing estimated dollar cost of any misses.

---

## Web dashboard

Visit `http://<esp32-ip-address>` to see a live status page for the heater. Toggle between metric and imperial units with the button at the top. The page auto-refreshes every 60 seconds. A **Learner Status** section at the bottom shows predicted and measured efficiency per day with colour-coded gap indicators.

<img width="1488" alt="Navien Status screen" src="https://github.com/user-attachments/assets/fa161464-81c6-4bcd-b79f-f4b13bdf4a2d" />

---

## Grafana + InfluxDB logging

The `Logger/` folder contains a script that listens for the UDP broadcast stream and writes all data to InfluxDB, plus a Grafana dashboard template to visualize it. Once set up, you get a full historical record of temperatures, gas usage, flow rates, recirculation events, and learner efficiency metrics — the nightly `learner` UDP packet carries the full per-day predicted/measured/gap table so every recompute is logged automatically.

<img width="1496" alt="Grafana status" src="https://github.com/user-attachments/assets/4846a6d5-f917-4f56-9773-adb856c933ff" />

---

## Telnet CLI

Connect to the ESP32 on port 23 for a full command-line interface:

| Category | Commands |
|---|---|
| Diagnostics | `gas`, `water`, `trace [gas\|water\|command\|announce]`, `stop` |
| Control | `power on\|off`, `recirc on\|off`, `setTemp <°C>`, `hotButton` |
| Scheduler | `scheduler on\|off`, `timezone <tz>` |
| Learner | `learnerStatus` |
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
| `NavienLearner.*` | On-device schedule learner (cold-start detection, peak-finding, efficiency tracking) |
| `TelnetCommands.*` | Telnet CLI commands |
| `Logger/` | UDP listener, InfluxDB logger, Grafana templates, bootstrap and schedule learner scripts |

---

## Compatibility

Built and tested against a **Navien NPE-240A**. The RS485 protocol is shared across the NPE-A series and likely works with other Navien tankless models that use the NaviLink interface, though this has not been independently verified for every model.

---

## Acknowledgements

- Community members of [Home Assistant](https://community.home-assistant.io/t/navien-esp32-navilink-interface/720567/170) who decoded the Navien RS485 protocol and packet structure
- [htumanyan](https://github.com/htumanyan/navien) for the well-crafted base Navien C++ class
- [ificator](https://github.com/ificator/navien_ha) for continued packet structure deciphering 
- [simont77](https://github.com/simont77/fakegato-history) for decoding the Eve history logging interface
- [HomeSpan](https://github.com/HomeSpan/HomeSpan) for the HomeKit accessory library for ESP32

---

## License

[MIT License](https://opensource.org/license/mit/)
