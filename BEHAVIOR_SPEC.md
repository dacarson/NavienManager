# NavienManager Behavior Specification

## Overview

NavienManager is an ESP32-based bridge between a Navien tankless water heater and the Apple HomeKit ecosystem. It connects to the Navien RS485 bus via UART2, parses proprietary binary packets from the heater, and exposes the heater's state and controls through four interfaces: HomeKit (via HomeSpan), a Telnet CLI, a web status page, and a UDP broadcast stream.

The firmware operates in one of two modes depending on whether a NaviLink control unit is detected on the RS485 bus:

- **Monitor mode** — A NaviLink is present. The ESP32 passively observes all traffic and reports state but does not transmit any commands, to avoid interfering with the NaviLink.
- **Control mode** — No NaviLink is present. The ESP32 takes ownership of the bus, periodically sends announce packets to identify itself, and actively sends commands to the heater in response to HomeKit requests and the internal scheduler.

---

## RS485 / Navien Serial Protocol

### Physical Layer

- UART2 on the ESP32, pins RXD2=16 / TXD2=17
- 19200 baud, 8N1

### Packet Structure

All packets start with a marker byte `0xF7` followed by a 6-byte header (`HEADER`):

| Byte | Field | Notes |
|------|-------|-------|
| 0 | `packet_marker` | Always `0xF7` |
| 1 | `unknown_0x05` | Protocol version, typically `0x05` |
| 2 | `direction` | `0x50` = heater→controller (status); `0x0F` = controller→heater (control) |
| 3 | `packet_type` | `0x50`–`0x57` = water unit 0–7; `0x0F` = gas; `0x4A`/`0x4F` = control |
| 4 | `unknown_0x90` | `0x90` for status, `0x10` for control |
| 5 | `len` | Payload byte count (excludes checksum byte) |

After the header, `len` payload bytes are followed by one checksum byte. Status packets use checksum seed `0x4B`; control packets use seed `0x62`.

### Packet Types

**Status packets** (heater → controller, direction `0x50`):
- **Water packets** (`packet_type` `0x50`–`0x57`): one per cascaded unit, up to 8 units. Carries power state, flow state, set/outlet/inlet temperatures, flow rate (LPM), operating capacity (%), recirculation flags, schedule active flag, hot-button active flag, and display unit preference.
- **Gas packet** (`packet_type` `0x0F`): carries controller/panel firmware versions, set/outlet/inlet temperatures, current gas usage (kcal), accumulated gas usage (m³), total operating time (minutes), domestic usage count, and corroborating temperature data.

**Control packets** (controller → heater, direction `0x0F`):
- **Announce** (`cmd_type` `0x4A`): a 10-byte heartbeat sent by a NaviLink (or by this firmware in Control mode) to claim bus ownership.
- **Command** (`cmd_type` `0x4F`): carries power on/off, set-point temperature, hot-button press/release, and recirculation on/off.

### Receive State Machine

The `Navien::loop()` function implements a three-state machine:

1. **INITIAL** — Scans incoming bytes until marker `0xF7` is found.
2. **MARKER_FOUND** — Reads the 6-byte header; validates that `len` is not `0xFF` and not larger than the packet buffer (128 bytes).
3. **HEADER_PARSED** — Reads the remaining `len + 1` bytes (payload + checksum); verifies the checksum and dispatches to the appropriate parser.

The loop caps at 100 iterations and 100 ms wall-clock time per call to prevent starving the rest of the system.

---

## Monitor vs. Control Mode

### NaviLink Detection

- A NaviLink is considered **present** when any announce packet (`0x4A`) is received on the bus. `navilink_present` is set to `true`.
- If 10 or more consecutive water packets arrive without an announce packet, `navilink_present` is reset to `false` (NaviLink has gone away).

### Monitor Mode

When `navilink_present` is `true`:
- All control functions (`power()`, `setTemp()`, `hotButton()`, `recirculation()`) are blocked and return `-1`.
- `controlAvailable()` returns `false`.
- Packets received from the NaviLink (commands and announces) are still parsed, decoded, and broadcast/reported.
- The HomeKit thermostat reflects the observed state but does not attempt to control the unit.

### Control Mode

When `navilink_present` is `false` AND the firmware has successfully transmitted at least one announce packet (`last_periodic_announce_time != 0`) — or is in test mode:
- `controlAvailable()` returns `true`.
- The firmware sends a periodic announce packet every 5 seconds to maintain bus ownership.
- Commands are enqueued in a 5-slot circular send queue and transmitted when the RS485 bus is idle.

### Bus Collision Avoidance

Before transmitting, `can_send()` enforces:
1. No bytes currently available in the receive buffer (bus is quiet).
2. The receive state machine is back to `INITIAL` (not mid-packet).
3. At least 50 ms of silence since the last received byte.
4. At least 30 ms since the last complete packet was parsed.

A minimum 2-second interval is enforced between any two consecutive commands (`MIN_COMMAND_INTERVAL_MS`).

### RS485 Echo Suppression

The RS485 transceiver used is the **THVD1406** (Texas Instruments), which features automatic direction control — no explicit DE/RE pin toggling is required. However, per the THVD1406 datasheet, due to the auto-direction control timing (`tdevice-auto-dir`), the receiver is briefly re-enabled after a transmission and will echo the TX data back onto RX. This is the hardware root cause of the self-echo problem.

Because RS485 is half-duplex, the ESP32 sees its own transmissions. Two suppression mechanisms prevent self-echo from being processed:
- **General command echo**: packets matching the last sent control packet within a 900 ms window are silently discarded.
- **Announce echo**: the fixed 10-byte announce packet is suppressed for 5 seconds after transmission.
- **Announce deduplication**: the same announce packet content received twice within 500 ms (local TX path + RS485 echo) triggers only one callback.

### Command Sequencing

- **hotButton**: queues a press (HOT_BUTTON_DOWN) followed by a release (0x00), both with the same `cmd_data` token derived from time-of-day seconds (increments every 39 seconds, rolls over every ~2.75 hours).
- **recirculation**: queues the recirculation on/off command followed by a follow-up zero command, both with the same `cmd_data` token.
- **CONTROL_COMMAND deferral**: if no announce has been sent yet (`last_periodic_announce_time == 0`), control commands are re-queued rather than dropped, ensuring the bus takeover sequence (announce first) is respected.

---

## HomeKit Support

HomeKit integration uses the [HomeSpan](https://github.com/HomeSpan/HomeSpan) library. The device presents itself as a **Thermostat** accessory with the following characteristics:

### Accessory Information

| Characteristic | Value |
|---|---|
| Manufacturer | Navien |
| Model | NPE-240A |
| Serial Number | ESP32 fused MAC address (no colons) |
| Firmware Revision | Controller firmware version (read from gas packet) |
| Hardware Revision | Panel firmware version (read from gas packet) |

### Thermostat Characteristics

| Characteristic | Direction | Behavior |
|---|---|---|
| `CurrentHeatingCoolingState` | Read | `HEATING` when current gas usage > 0; `IDLE` otherwise |
| `TargetHeatingCoolingState` | Read/Write | Valid values: `OFF` and `HEAT` only. `HEAT` is automatically set when recirculation is active or gas is burning. `OFF` is set when neither is occurring. User setting `HEAT` triggers a 5-minute override; user setting `OFF` turns off recirculation. |
| `CurrentTemperature` | Read | Outlet temperature from gas packet, updated when it changes by > 0.5°C and at least 5 seconds have elapsed. |
| `TargetTemperature` | Read/Write | Set-point temperature from gas packet. Write range: 37.0°C–60.0°C in 0.5°C steps. Writes are sent to the heater via `setTemp()`. |
| `TemperatureDisplayUnits` | Read | Mirrors the Navien panel's own Metric/Imperial setting from the water packet. |
| `ProgramMode` | Read | `0` = no program; `1` = scheduled; `2` = override active. |
| `ValvePosition` (custom) | Read | Operating capacity from water packet (0–100%). Labeled "Actuation", unit "percentage". |
| `ProgramCommand` (custom) | Write | Receives Eve app schedule data; dispatched to `FakeGatoScheduler::parseProgramData()`. |

### Eve History Service

The firmware implements the Elgato Eve history protocol, allowing the Eve app to display a history graph. History entries contain:

| Field | Description |
|---|---|
| `currentTemp` | Outlet temperature (°C × 100, stored as uint16) |
| `targetTemp` | Set-point when heating; `TEMPERATURE_MIN` (37°C) when idle — produces a clean on/off graph |
| `valvePercent` | Operating capacity (0–100) |
| `thermoTarget` | Target state from HomeKit (0 = Off, 1 = Heat) |
| `openWindow` | Always 0 (not implemented) |

**Logging cadence:**
- Default: every 10 minutes (averaged).
- When `valvePercent > 0` (heating active): switches to 1-minute logging.
- Returns to 10-minute logging when `valvePercent` returns to 0.
- Key-value changes (set-point, thermoTarget, or valve opening) trigger an immediate entry if at least 60 seconds have passed since the last entry.
- Duplicate entries (all fields unchanged) are suppressed.

History is persisted to LittleFS (`/history.bin`) on every new entry. Up to `historySize` entries are stored in a circular buffer. On startup, history is reloaded and the `refTime` is validated; a corrupt or pre-2025 `refTime` is repaired using the oldest valid entry timestamp.

### Scheduler (FakeGatoScheduler / SchedulerBase)

The Eve app's thermostat schedule is parsed and stored in NVS (`SAVED_DATA` / `PROG_SEND_DATA`). The scheduler drives recirculation on/off based on the weekly time-slot program.

**Scheduler states:**

| State | Navien Action |
|---|---|
| `Unknown` | No action taken; control not assumed. |
| `Active` | Power on; recirculation on. |
| `InActive` | Power on; recirculation off. |
| `Vacation` | Power off; recirculation off. |
| `Override` | Power on; recirculation on (for configured override duration, default 5 minutes). |

**Enabling and disabling the scheduler:** The scheduler can be enabled or disabled from the Eve app (via the `SCHEDULE_STATE` block in `ProgramData`) or from the Telnet `scheduler on` / `scheduler off` commands. When enabled, `initializeCurrentState()` is called immediately so the scheduler reflects the correct slot state before any HomeKit commands (such as `TargetHeatingCoolingState = HEAT`) arrive — this prevents a spurious override from being triggered when the schedule is turned on while already inside a scheduled time slot.

**Scheduler enable persistence:** The user's enable/disable intent is stored in NVS (`SAVED_DATA` / `SCHED_ACTIVE`, `uint8`). This key is written separately from `PROG_DATA_FULL_DATA` because the `schedule_on` field inside that blob is always persisted as `0` (see fake-state reporting below). On startup the constructor reads `SCHED_ACTIVE` first, falling back to `prog_send_data.schedule_state.schedule_on` for backwards compatibility with older firmware images.

**Fake-state reporting to Eve:** The `schedule_on` field returned to the Eve app inside `ProgramData` is always reported as `0`, regardless of the actual enable state. This prevents the Eve app from autonomously sending `TargetHeatingCoolingState = HEAT` when it sees the schedule is on and the current temperature is below the target. Because `prog_send_data.schedule_state.schedule_on` is therefore always `0`, it cannot be used as the persisted source of truth for the enable state (see above).

**State transitions:** When a scheduled transition time arrives, `initializeCurrentState()` is called to evaluate the current time against the week schedule and determine the correct state. This uses `isTimeWithinSlot()` (which uses `>=` for the slot start boundary), ensuring that a transition firing at exactly the slot start time correctly enters `Active`. When an override expires, `initializeCurrentState()` is similarly called to revert to the correct scheduled state immediately rather than waiting for the next scheduled transition.

**Schedule format:** Up to 4 time slots per day, Monday–Sunday (Eve convention), converted internally to Sunday–Saturday (C `tm_wday` convention). Each slot is encoded as 10-minute-resolution offsets (value / 6 = hour, value % 6 × 10 = minute).

**Timezone handling:** The Eve app sends the current local time in program data. The scheduler computes the UTC offset by comparing the Eve-supplied local time against the system clock (before any TZ is set) and stores a `UTC±N` string in NVS (`SCHEDULER` / `TZ`). Once set, the TZ is used for all `localtime()` calculations. The timezone can be overridden or cleared via the Telnet `timezone` command.

**Control handoff:**
- When `controlAvailable()` becomes `true` (NaviLink disappears) and the scheduler state is known, `takeControl()` is called once. It sends the appropriate power and recirculation commands to match the current scheduler state.
- When `controlAvailable()` becomes `false` (NaviLink reappears), the firmware stops controlling the unit and yields back to the NaviLink.

**ProgramData refresh:** The full `PROG_DATA_FULL_DATA` blob (containing current time, temperatures, schedule state, vacation, and week schedule) is refreshed to the HomeKit characteristic either when the Eve app updates it or every 60 seconds. The current time field is kept up to date by adding the elapsed milliseconds since the last sync.

**Temperature intercept:** The Eve app's temperature schedule only supports up to 30°C. The firmware silently replaces the comfort and default temperature values with the heater's actual set-point before storing them, preserving the user's set-point while satisfying the protocol.

### Hot Water Switch (Override Trigger)

A second HomeKit accessory named **"Hot Water"** is exposed as a `Service::Switch`. Its sole purpose is to allow Siri voice commands ("Hey Siri, turn on hot water") to trigger the recirculation override.

#### Accessory Structure

```
SpanAccessory("Hot Water")
├── Service::AccessoryInformation
│   └── Characteristic::Name("Hot Water")
└── Service::Switch (DEV_HotWaterSwitch)
    └── Characteristic::On (Read/Write boolean)
```

#### Switch On Behavior

When the switch is set **On**, the same guard logic as `TargetHeatingCoolingState = HEAT` is applied:

- If the scheduler is **disabled**, or the scheduler is enabled but **not currently in Active state**: call `scheduler->activateOverride()` to start a 5-minute recirculation override.
- If the scheduler is enabled and already in **Active state** (recirculation is already running via schedule): do nothing — the override is redundant.
- If the scheduler is already in **Override state**: do nothing — a second simultaneous override is ignored.

#### Switch Off Behavior

Setting the switch **Off** has no effect. The switch cannot be used to stop recirculation. The switch turns off automatically when the override expires or the heater becomes idle (see State Reflection below).

#### Switch State Reflection

The switch `On` characteristic is kept in sync with the actual heater state in the `loop()` method:

- Switch is **On** when recirculation is active (`recirculation_active`) **or** gas is currently burning (`current_gas_usage > 0`).
- Switch is **Off** otherwise.

This ensures the switch automatically turns off in HomeKit when an override expires or the scheduled slot ends — without requiring any user action.

#### Relationship to Thermostat

The Hot Water switch and the thermostat `TargetHeatingCoolingState` characteristic are independent HomeKit controls that both drive the same underlying override mechanism. They will reflect identical states: when one shows heating active, the other will also show its active state.

### OTA Updates

OTA is enabled (HomeSpan `enableOTA(false, false)`) without requiring a password and without forcing a reboot.

---

## Telnet Support

A Telnet server listens on **port 23**. It is started after WiFi connects. All commands are dispatched through a `std::map`-based command registry.

### Command Reference

| Command | Arguments | Description |
|---|---|---|
| `help` | — | Lists all registered commands with descriptions. |
| `ping` | — | Replies "Pong!" to verify connectivity. |
| `wifi` | — | Prints SSID, IP address, and RSSI. |
| `memory` | — | Prints free heap and max allocatable block. |
| `trace` | `gas` \| `water` \| `command` \| `announce` | Streams all matching decoded JSON packets to the Telnet session. |
| `trace` | (no arg) | Streams all packet types. |
| `stop` | — | Stops packet streaming. |
| `gas` | — | Prints current gas state as JSON. |
| `water` | — | Prints current water state as JSON (array if multiple units). |
| `control` | — | Reports whether control commands can be sent. |
| `setTemp` | (no arg) | Prints current set-point temperature. |
| `setTemp` | `<°C>` | Sets the heater set-point (20°C–60°C range check). |
| `power` | (no arg) | Prints current power state for all units. |
| `power` | `on` \| `off` | Turns the heater on or off. |
| `recirc` | (no arg) | Prints current recirculation state for all units. |
| `recirc` | `on` \| `off` | Enables or disables recirculation. |
| `hotButton` | — | Sends a hot-button press+release sequence. |
| `scheduler` | (no arg) | Prints scheduler enabled state, current state, next transition time and target state, and full weekly schedule. |
| `scheduler` | `on` \| `off` | Enables or disables the scheduler (persisted to NVS). |
| `timezone` | (no arg) | Prints the currently stored timezone. |
| `timezone` | `<tz string>` | Sets the timezone (e.g. `UTC-8`). |
| `timezone` | `clear` | Erases the stored timezone from NVS. |
| `time` | — | Prints local and GMT time. |
| `erasePgm` | — | Erases Eve program data from NVS (`PROG_SEND_DATA`). Requires reboot. |
| `history` | (no arg) | Dumps all history entries as CSV. |
| `history` | `<N>` | Dumps the last N history entries as CSV. |
| `eraseHistory` | — | Erases all history entries from LittleFS and memory. |
| `fsStat` | — | Prints LittleFS partition total, used, and free bytes. |
| `reboot` | — | Disconnects the Telnet client and restarts the ESP32. |
| `bye` | — | Disconnects the Telnet session. |

### Packet Trace Format

`trace` streams live packet JSON to the Telnet session as packets arrive. The `debug` field in each JSON object contains the raw hex bytes of the packet.

### Error Reporting

Parse errors (checksum mismatches, unknown packet types, buffer overflows) are forwarded to the Telnet session via the `onError` callback as: `Error <function_name> <message>`.

---

## Web Support

The HomeSpan web log endpoint is extended with a custom status page. The page auto-refreshes every 60 seconds and is accessible at the HomeSpan web server URL.

### Status Dashboard

The page renders a dark-themed card grid with color-coded status indicators:
- **Green** (`status-ok`): normal / off states.
- **Yellow** (`status-warning`): active/running states (gas burning, flow active, recirculation running, etc.).
- **Red** (`status-error`): power-off states.

**Heater cards (per unit, labeled `[N]` for cascade units):**

| Card | Content |
|---|---|
| Hotwater Power | On / Off |
| Domestic Consumption | Yes / No |
| Current Gas Usage | kcal / BTU |
| Accumulated Gas | m³ / Therms |
| Domestic Outlet Set Temp | °C / °F |
| Domestic Outlet Temp | °C / °F |
| Inlet Temp | °C / °F |
| Domestic Flow Rate | LPM / GPM |
| Total Operating Time | Hours |
| Accumulated Usage Count | Count |
| Navien Scheduler | Active / Inactive |
| Operating Capacity | % |
| Recirculation | Active / Inactive |
| Recirculation Pump | Running / Stopped |

**Scheduler section:**

| Card | Content |
|---|---|
| Scheduler Enabled | Yes / No (reflects `SCHED_ACTIVE` NVS key) |
| Current State | Unknown / Active / Inactive / Vacation / Override |
| Next Transition | Local time of next state change |
| Next State | State after next transition |

A units toggle (metric / imperial) is provided in the page header. The preference is persisted in `localStorage`.

The page header firmware version (controller / panel) is displayed as a sub-heading. The first `h2` heading is wrapped as a link to the GitHub repository.

The system log table (HomeSpan's built-in `tab1`) is hidden; only the custom status content is shown.

---

## UDP Broadcasting

An `AsyncUDP` socket broadcasts JSON-encoded packets to **port 2025** on the local subnet broadcast address.

### Broadcast Throttling

Each packet type maintains a "previous raw hex" string. A broadcast is only sent when the raw bytes differ from the previous broadcast. Additionally, `resetPreviousValues()` clears all previous strings every 5 seconds, ensuring that even unchanged state is re-broadcast at least once every 5 seconds when packets are actively received.

### JSON Packet Formats

All packets include a `"debug"` field containing the raw packet as a hex string (uppercase, space-separated bytes).

**Water packet** (`"type": "water"`):

| Field | Type | Description |
|---|---|---|
| `device_number` | int | Unit index (0–7) |
| `system_power` | int (0/1) | Power state |
| `set_temp` | float (1 dp) | Set-point °C |
| `inlet_temp` | float (1 dp) | Inlet °C |
| `outlet_temp` | float (1 dp) | Outlet °C |
| `flow_lpm` | float (1 dp) | Flow rate LPM |
| `flow_state` | int | Raw flow state byte |
| `recirculation_active` | int (0/1) | Recirculation mode enabled |
| `recirculation_running` | int (0/1) | Recirculation pump running |
| `display_metric` | int (0/1) | Display units (1=metric) |
| `schedule_active` | int (0/1) | Navien internal schedule active |
| `hotbutton_active` | int (0/1) | Hot-button active |
| `operating_capacity` | float (1 dp) | Burner capacity % |
| `consumption_active` | int (0/1) | Hot tap open |
| `unknown_10`, `unknown_27`, `unknown_28`, `unknown_30` | int | Raw bytes for protocol research |
| `counter_a`, `counter_b` | int | 16-bit counters from unknown bytes |

**Gas packet** (`"type": "gas"`):

| Field | Type | Description |
|---|---|---|
| `controller_version` | float (1 dp) | Controller firmware |
| `panel_version` | float (1 dp) | Panel firmware |
| `set_temp` | float (1 dp) | Set-point °C |
| `inlet_temp` | float (1 dp) | Inlet °C |
| `outlet_temp` | float (1 dp) | Outlet °C |
| `current_gas_usage` | int | Current gas usage kcal |
| `accumulated_gas_usage` | float (1 dp) | Accumulated gas m³ |
| `total_operating_time` | int | Total minutes operated |
| `accumulated_domestic_usage_cnt` | int | Domestic usage counter |
| `unknown_20`, `unknown_28`, `unknown_32`, `unknown_33`, `unknown_34` | int | Raw bytes |
| `counter_a`, `counter_c` | int | 16-bit counters from unknown bytes |

**Command packet** (`"type": "command"`):

| Field | Type | Description |
|---|---|---|
| `power_command` | int (0/1) | Packet contains a power command |
| `power_on` | int (0/1) | Power direction (if `power_command`) |
| `set_temp_command` | int (0/1) | Packet contains a set-temp command |
| `set_temp` | float | New set-point °C (if `set_temp_command`) |
| `hot_button_command` | int (0/1) | Hot-button press observed |
| `recirculation_command` | int (0/1) | Packet contains a recirculation command |
| `recirculation_on` | int (0/1) | Recirculation direction (if `recirculation_command`) |
| `cmd_data` | int | Raw cmd_data token byte |

**Announce packet** (`"type": "announce"`):

| Field | Type | Description |
|---|---|---|
| `navilink_present` | int (0/1) | Whether a NaviLink was detected |

---

## Startup Sequence

1. UART2 initialized for Navien RS485 at 19200 baud.
2. HomeSpan initialized; WiFi credentials managed by HomeSpan pairing. OTA enabled.
3. HomeSpan web log configured with custom CSS and the `navienStatus` callback.
4. HomeKit accessories registered: `DEV_Navien` thermostat with Eve history and scheduler.
5. On WiFi connect:
   - `setupNavienBroadcaster()`: registers Navien packet callbacks; starts UDP broadcast.
   - `setupTelnetCommands()`: registers all commands; starts Telnet server on port 23.
6. Main loop runs: `telnet.loop()`, `navienSerial.loop()`, `homeSpan.poll()`.
7. Once a timezone is available (`TZ` env var set) and SNTP sync is confirmed, `homeSpan.assumeTimeAcquired()` is called to unlock time-dependent HomeKit features.
