# NavienManager Behavior Specification

## Overview

NavienManager is an ESP32-based bridge between a Navien tankless water heater and the Apple HomeKit ecosystem. It connects to the Navien RS485 bus via UART2, parses proprietary binary packets from the heater, and exposes the heater's state and controls through four interfaces: HomeKit (via HomeSpan), a Telnet CLI, a web status page, and a UDP broadcast stream.

### Flash and Memory Constraints

The ESP32 module used in this project has limited flash storage. Flash is a **premium resource** and must be conserved throughout the firmware:

- **Avoid new libraries.** Every additional library can cost tens of kilobytes. Before adding a dependency, verify it is not already available or that the same result cannot be achieved with primitives already in the project (e.g., raw socket APIs instead of a higher-level HTTP server framework).
- **Reuse existing library costs.** Libraries that are already linked (HomeSpan, ArduinoJson, ESPTelnet) incur their flash cost once. Additional use of those libraries is effectively free. Prefer extending existing interfaces over introducing new ones.
- **Use raw ESP-IDF / Arduino core APIs when a library would be overkill.** For example, `WiFiServer` (part of `<WiFi.h>`, already included) provides a raw TCP socket listener at zero additional flash cost; the `WebServer` library layered on top of it adds ~30–50 KB and is unnecessary when the protocol surface is small and fully controlled.
- **Keep handlers small and purpose-specific.** Do not add error-handling paths, generic routing layers, or abstractions for scenarios that will never occur in this deployment.

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

**Scheduler enable persistence:** The user's enable/disable intent is stored in NVS (`SAVED_DATA` / `SCHED_ACTIVE`, `uint8`). On startup the constructor reads `SCHED_ACTIVE` first, falling back to `prog_send_data.schedule_state.schedule_on` for backwards compatibility with older firmware images.

**Schedule state reporting to Eve:** The `schedule_on` field returned to the Eve app inside `ProgramData` is `1` only when `scheduleActive` is true **and** the current state is `Active` or `Override`; otherwise it is `0`. Reporting `1` during an `Inactive` period (between slots) would cause Eve to try to change the Navien set point. Reporting `0` during `Inactive` causes Eve to write back `0`, which is detected and ignored (see below). If Eve responds to a `1` by sending `TargetHeatingCoolingState = HEAT` during an active slot, that is harmless because the unit is already heating.

**Eve write-back suppression:** When Eve reads `schedule_on = 0` (because we are in an `Inactive` period) it writes the value back as `0`. A genuine user disable can only arrive while Eve was showing `1`, which only happens during `Active` or `Override` states. Therefore, a `schedule_on = 0` write received while in `Inactive` state is silently ignored. A `schedule_on = 0` write received during `Active` or `Override` is honoured and disables the scheduler.

**State transitions:** When a scheduled transition time arrives, `initializeCurrentState()` is called to evaluate the current time against the week schedule and determine the correct state. This uses `isTimeWithinSlot()` (which uses `>=` for the slot start boundary), ensuring that a transition firing at exactly the slot start time correctly enters `Active`. When an override expires, `initializeCurrentState()` is similarly called to revert to the correct scheduled state immediately rather than waiting for the next scheduled transition.

**Schedule format:** Up to 4 time slots per day on the wire, Monday–Sunday (Eve convention), converted internally to Sunday–Saturday (C `tm_wday` convention). Each slot is encoded as 10-minute-resolution offsets (value / 6 = hour, value % 6 × 10 = minute). The `CMD_DAY_SCHEDULE` wire struct always carries 4 slots (8 bytes per day); Eve's app UI limits the user to configuring 3 comfort periods, so the 4th slot is always `0xFF` (unused) in practice. The internal `DaySchedule` (SchedulerBase) and the HTTP POST endpoint cap at **3 slots** to match the Eve UI limit, and the schedule learner uses `MAX_SLOTS_PER_DAY = 3` for the same reason.

**Empty/unused slot sentinel:** `0xFF` is the sentinel value for an unused slot in both the Eve wire format and the internal representation:
- In the Eve `CMD_DAY_SCHEDULE` struct, `slot[i].offset_start == 0xFF` marks slot `i` (and all following slots) as unused. The conversion loop in `updateSchedulerWeekSchedule()` stops at the first `0xFF` offset.
- In `SchedulerBase`'s `DaySchedule` struct, `slots[i].startHour == 0xFF` marks slot `i` as unused. All iteration loops (`getNextState`, `getTimeSlot`, `isTimeWithinSlot` callers) stop at the first `0xFF` startHour.
- The full `weekSchedule[7]` array is initialized to `0xFF` via `memset` at the start of `updateSchedulerWeekSchedule()` so that any slots not populated from Eve data are correctly marked unused rather than left with garbage values.

**Timezone handling:** The Eve app sends the current local time in the `CURRENT_TIME` packet. The scheduler computes the UTC offset in minutes by comparing the Eve-supplied local time against the system clock and stores it in `_lastKnownUtcOffsetMin` (`_utcOffsetKnown` is set `true`). A human-readable `UTC±N` string is also written to NVS (`SCHEDULER` / `TZ`) for display purposes only. **TZ is display-only:** schedule slots are stored and fired in UTC; a wrong TZ (e.g. written by `guessTimeZone()` when a remote Eve app connects from a different timezone) corrupts the Eve schedule display but cannot cause schedules to fire at the wrong time. `getEffectiveOffsetMin()` is the authoritative source for the UTC↔local offset — it returns `_lastKnownUtcOffsetMin` when `_utcOffsetKnown` is `true`, otherwise falls back to the system TZ derived from `localtime_r`/`gmtime_r`. The timezone string can be overridden or cleared via the Telnet `timezone` command (display only).

**What stays local-time (display only):**

| Location | Data |
|---|---|
| `addMilliseconds()` / `prog_send_data.currentTime` | Eve wire time struct sent back to Eve |
| `updateCurrentScheduleIfNeeded()` | Day-of-week selection for which local day Eve shows |
| `stateChange()` log message | "Next event scheduled for:" timestamp |
| `appendStatusHTML()` | Last recompute timestamp on the web page |
| `TelnetCommands.cpp` (all) | All time displays (local + UTC annotation) |
| `guessTimeZone()` / NVS `TZ` | TZ string; used to convert UTC slots → local for Eve readback |
| `HomeSpanWeb.ino` | Status time strings |

**UTC-native storage and Eve conversion paths:** Slots are stored internally and compared at fire-time in UTC. `SchedulerBase::getNextState()` and `initializeCurrentState()` use `gmtime()` and `proper_timegm()` (from `TimeUtils.h`) — never `localtime()` or `mktime()`. When Eve writes `WEEK_SCHEDULE`, `convertEveSlotsToUTC(_lastKnownUtcOffsetMin)` converts the local-time slots to UTC before they are passed to `updateSchedulerWeekSchedule()` and written to NVS. If `_utcOffsetKnown` is false when `WEEK_SCHEDULE` arrives, the schedule is discarded and a warning is logged — Eve will resend after a `CURRENT_TIME` packet establishes the offset. When `prog_send_data` is assembled for Eve readback, `getEffectiveOffsetMin()` converts stored UTC slots back to local time. `proper_timegm()` in `TimeUtils.h` is the TZ-free `timegm()` equivalent (integer arithmetic, no `setenv("TZ")` side effects); use it whenever a UTC `struct tm` must be converted to `time_t`.

**NVS schedule version migration:** A `schedVersion` key (type `uint8_t`) is stored in the `SAVED_DATA` NVS namespace alongside `PROG_SEND_DATA`. `FakeGatoScheduler::begin()` checks this key before applying `prog_send_data` to `weekSchedule[]`. If the version is not `1` (UTC-format), slots are cleared and `schedVersion` is written to `1`. After flashing, the user must re-push the schedule once from the Eve app (or via `navien_bootstrap.py --push`) to repopulate NVS with UTC-converted slots.

**Control handoff:**
- When `controlAvailable()` becomes `true` (NaviLink disappears) and the scheduler state is known, `takeControl()` is called once. It sends the appropriate power and recirculation commands to match the current scheduler state.
- When `controlAvailable()` becomes `false` (NaviLink reappears), the firmware stops controlling the unit and yields back to the NaviLink.

**ProgramData refresh:** The full `PROG_DATA_FULL_DATA` blob (containing current time, temperatures, schedule state, vacation, and week schedule) is refreshed to the HomeKit characteristic either when the Eve app updates it or every 60 seconds. The current time field is kept up to date by adding the elapsed milliseconds since the last sync. When the schedule genuinely changes (HTTP POST, Eve write, vacation change), the characteristic is updated with `notify=true` so all paired Eve instances receive an EV notification and update their local caches. Periodic time-only refreshes use `notify=false`.

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

A `setStatusCallback` lambda is registered in `setup()`. When HomeSpan fires `HS_OTA_STARTED` (just before the OTA transfer begins and the device reboots), the callback calls `learner->saveMeasured()` to flush the rolling measured-efficiency window to LittleFS so it survives the update.

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
| `scheduler` | (no arg) | Prints scheduler enabled state, current state, next transition time and target state, and full weekly schedule. Slots are shown as both local time and UTC (`HH:MM-HH:MM (UTC HH:MM-HH:MM)`); when the UTC day differs from the local day, `prev day` is appended. |
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
| `learnerStatus` | — | Prints on-device schedule learner status: last recompute time, bucket fill percentage, and a per-day table showing predicted efficiency, measured efficiency, gap, and rolling 4-week cold-start count. |
| `saveLearner` | — | Immediately persists the rolling measured-efficiency window to `/navien/measured.bin` on LittleFS. Useful before a planned reboot that is not triggered through OTA. |
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

A **Learner Status** section is appended to the page by `NavienLearner::appendStatusHTML()`. It renders the same data as `learnerStatus` Telnet: last recompute time, bucket fill, and a per-day table with Predicted %, Measured %, Gap, and 4-week cold-start count. The Gap column is colour-coded: green (< 10%), amber (10–25%), red (> 25%). The HTML is built on demand into the existing page buffer and is not cached.

---

## UDP Broadcasting

An `AsyncUDP` socket broadcasts JSON-encoded packets to **port 2025** on the local subnet broadcast address.

### Broadcast Throttling

Each packet type maintains a "previous raw hex" string. A broadcast is only sent when the raw bytes differ from the previous broadcast. Additionally, `resetPreviousValues()` clears all previous strings every 5 seconds, ensuring that even unchanged state is re-broadcast at least once every 5 seconds when packets are actively received.

### JSON Packet Formats

RS485-derived packets (`water`, `gas`, `command`, `announce`) include a `"debug"` field containing the raw packet as a hex string (uppercase, space-separated bytes). The `learner` packet also includes `"debug"` but it is always an empty string — it is a computed packet with no corresponding raw RS485 bytes.

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

**Learner packet** (`"type": "learner"`):

Emitted by `NavienLearner::broadcastUDP()` at the end of every `RECOMPUTE_WRITE` — both the nightly midnight recompute and any manual recompute triggered by `requestRecompute()` (e.g. after seeding buckets via `POST /buckets`). Not subject to the raw-hex duplicate throttle used by RS485-derived packets.

All per-day fields use a 3-letter day prefix: `sun`, `mon`, `tue`, `wed`, `thu`, `fri`, `sat`. The structure is fully flat so that `navien_listener.py` can pass `payload['fields'] = data` directly to InfluxDB without custom flattening.

| Field | Type | Description |
|---|---|---|
| `last_recompute` | int | Unix timestamp of the completed recompute |
| `bucket_fill_pct` | float (1 dp) | Percentage of 2016 buckets (7 days × 288 five-minute slots) that have at least one cold-start recorded |
| `{pfx}_slots` | string | Recirculation schedule slots for that day, encoded as `"HH:MM-HH:MM,..."` (empty string if no slots were found) |
| `{pfx}_predicted_pct` | float (1 dp) | Predicted efficiency: fraction of schedulable demand buckets (those inside a slot OR within 15 min after a slot end) that fall inside a slot; omitted if bucket data is insufficient |
| `{pfx}_measured_pct` | float (1 dp) | Measured efficiency from the rolling 4-week cold-start window; omitted if no cold-starts have been observed yet for that day |
| `{pfx}_gap_pct` | float (1 dp) | `predicted - measured`; omitted if either value is unavailable |
| `{pfx}_cold_starts_4wk` | int | Cold-starts observed for that day across the rolling 4-week window; always present |
| `debug` | string | Always empty (`""`) |

Float fields (`bucket_fill_pct`, `{pfx}_predicted_pct`, etc.) use `serialized(String(x, 1))` and appear as bare JSON numbers (e.g. `7.0`), not quoted strings. Python's `json.loads()` parses them as floats; InfluxDB stores them as float fields.

---

## External Schedule Configuration

The scheduler time slots can be configured from outside the device in two ways: through the **Eve app** (via HomeKit `ProgramCommand`, described in the HomeKit section), or through an **HTTP POST** pushed by an external script. The HTTP interface exists so that a data-driven tool running on another machine (e.g., a Raspberry Pi or home server) can push a learned schedule derived from historical usage data.

### HTTP Endpoint

The firmware listens for HTTP POST requests on **port 8080**. HomeSpan owns port 80 and provides no public API for custom POST handlers, so a raw `WiFiServer` (part of `<WiFi.h>`, zero additional flash cost) is used instead of a separate HTTP server library.

Two paths are dispatched by `loopScheduleEndpoint()` on the same port:

#### `POST /schedule`

- **Content-Type:** `application/json`
- **Response 200:** schedule was valid and applied.
- **Response 400:** JSON was malformed or contained out-of-range values.
- Uses a fixed 2 KB static body buffer.

#### `POST /buckets` (bootstrap bucket ingest — Phase 9)

- **Content-Type:** `application/json`
- **Response 200:** `{"status":"ok","buckets_written":<n>,"replaced":<bool>}`
- **Response 400:** schema version mismatch, JSON parse failure, body exceeds 6 KB, or LittleFS write error.
- **Response 503:** learner object was never instantiated, or `begin()` failed (learner disabled). Both indicate the ingest service is unavailable; the body is `Learner unavailable` in either case.
- Uses a separate 6 KB static body buffer (never allocated to `/schedule`).
- **Merge** (`"replace": false`, default): adds `raw` and `score` to existing in-RAM bucket values, then writes once to LittleFS.
- **Replace** (`"replace": true`): zeros all buckets in RAM first, then applies incoming data and writes once. Safe to re-run if a prior upload was incorrect.
- Sets `_recomputeRequested` after the write completes so Core 0 runs peak-finding immediately rather than waiting for midnight.

The endpoint is started in `setupScheduleEndpoint()` (called from `onWifiConnected`) and polled in `loopScheduleEndpoint()` (called from the main loop). Both paths share a 1-second read timeout — sufficient for a LAN client.

### JSON Format — `POST /schedule`

```json
{
  "schedule": [
    { "slots": [ { "startHour": 6, "startMinute": 57, "endHour": 9, "endMinute": 3 } ] },
    { "slots": [ { "startHour": 7, "startMinute": 0,  "endHour": 9, "endMinute": 0 },
                 { "startHour": 18, "startMinute": 0, "endHour": 21, "endMinute": 0 } ] },
    ...
  ]
}
```

- `schedule` is an array of exactly **7** day objects, index **0 = Sunday** through **6 = Saturday** (matching `SchedulerBase`'s `tm_wday` convention).
- Each day object has a `slots` array of 0–3 slot objects (matching Eve's UI limit of 3 comfort periods per day).
- Each slot has `startHour` (0–23), `startMinute` (0–59), `endHour` (0–23), `endMinute` (0–59).
- Out-of-range values or a missing/wrong-length `schedule` array produce a 400 response.

### JSON Format — `POST /buckets`

```json
{
  "schema_version": 2,
  "current_year": 2025,
  "replace": false,
  "days": [
    {
      "dow": 0,
      "buckets": [
        { "b": 72, "raw": 5,  "score": 12.0 },
        { "b": 73, "raw": 3,  "score":  7.5 }
      ]
    }
  ]
}
```

- `schema_version` must equal `BUCKET_SCHEMA_VERSION` (2) — mismatches produce a 400. Bucket dow/bucket indices are UTC (matching the UTC-native firmware).
- `current_year` — optional; if present and non-zero, written into the `BucketFile` header. If omitted or zero: in merge mode the existing header year is left unchanged; in replace mode the year is derived from the system clock (same fallback as `BucketStore::begin()`), so the header is never left at a stale value.
- `replace` — `false` (default): merge into existing data; `true`: zero all buckets first.
- `days[].dow` — day of week, 0 = Sunday … 6 = Saturday.
- `days[].buckets[].b` — 5-minute bucket index (0–287).
- `days[].buckets[].raw` — unweighted cold-start count to add.
- `days[].buckets[].score` — weighted score to add.
- Only non-zero buckets need be included; absent buckets are unchanged (merge) or zero (replace).

### Internal Mapping

`FakeGatoScheduler::setWeekScheduleFromJSON()` translates the received schedule into both internal representations and persists both:

1. **Eve binary format** (`prog_send_data.weekSchedule`): days are re-ordered from Sunday-first (JSON) to Monday-first (Eve), and times are encoded as 10-minute offsets (`hour × 6 + minute / 10`). Unused slots are set to `0xFF`.
2. **SchedulerBase format** (`weekSchedule[7]`): populated by calling `updateSchedulerWeekSchedule()`, which converts the Eve offsets back to `{startHour, startMinute, endHour, endMinute}` structs and handles the Monday→Sunday to Sunday→Saturday index shift.

Slots beyond the third in any day are silently dropped. The full `PROG_DATA_FULL_DATA` blob is then committed to NVS (`SAVED_DATA` / `PROG_SEND_DATA`), `initializeCurrentState()` is called to apply the new schedule immediately, and `refreshProgramData` is set so all paired Eve instances receive an EV notification with the updated schedule.

`setWeekScheduleFromJSON()` receives slots already in UTC (from the on-device learner or a Python push) and stores them verbatim. It does **not** apply `sanitizeScheduleToLocalLimit()` — JSON-pushed schedules are already within the 3-slot-per-UTC-day limit. `sanitizeScheduleToLocalLimit()` is called **only** from `convertEveSlotsToUTC()` (the Eve→device path); calling it on a UTC schedule incorrectly drops valid same-day slots when cross-day UTC slots from adjacent days fill slot positions first.

### config.py

`Logger/config.py` is a shared configuration module imported by `navien_schedule_learner.py` and any other Logger-side scripts. It defines:

| Constant | Value | Description |
|---|---|---|
| `INFLUX_HOST` | `"localhost"` | InfluxDB hostname |
| `INFLUX_PORT` | `8086` | InfluxDB port |
| `INFLUX_DB` | `"navien"` | InfluxDB database name |
| `GAS_RATE_USD_PER_KCAL` | `0.41601 / 29308` | Gas cost (~$0.00001420/kcal) |
| `WATER_RATE_USD_PER_L` | `11.40 / (748 × 3.785)` | Water cost (~$0.004024/L) |
| `COLD_PIPE_DRAIN_MINUTES` | `3.0` | Assumed drain time when recirc hasn't recently run |
| `RECIRC_WINDOW_MINUTES` | `15.0` | How long after a recirc cycle pipes stay hot |
| `SAMPLE_INTERVAL_SECONDS` | `5` | InfluxDB polling interval |
| `OUTPUT_MEASUREMENT` | `"navien_efficiency"` | InfluxDB measurement name for scored output |
| `OUTPUT_TAG_DEVICE` | `"navien"` | InfluxDB tag value for scored output |

### navien_schedule_learner.py

`Logger/navien_schedule_learner.py` learns a recirculation schedule from InfluxDB history and pushes it to the ESP32. It runs in five steps:

**Step 1 — Fetch cold-start events**

Queries InfluxDB for `MAX(consumption_active)` and `MAX(recirculation_running)` from the `water` measurement at **10-second resolution** (`GROUP BY time(10s) FILL(none)`). Queries cover a **rolling ±`window_weeks` (default 4) seasonal band** around today's calendar date in each configured year, so only seasonally relevant data is used.

For each year, cold-start events are extracted: the first active bucket after ≥ `cold_gap_minutes` (default 10) of inactivity. Each event is assigned a **combined weight** = `recency_weight × demand_weight × cost_multiplier`:

- `demand_weight` depends on whether recirculation was running at the cold-start and the run duration (measured in 10-second buckets):
  - `recirculation_running=0`, duration < 6 buckets (< 1 min): `0.5` (short/accidental tap)
  - `recirculation_running=0`, duration ≥ 6 buckets: `1.0` (genuine cold-pipe demand)
  - `recirculation_running=1`, duration < 3 buckets (< 30s): `0.0` (discarded)
  - `recirculation_running=1`, duration ≥ 3 buckets: `1.0` (genuine demand, pipes already hot)
- `cost_multiplier` is normalised to [0, 1] using `COLD_START_WASTE_USD` (≈ $0.097 per cold-start at 8 L/min × 3 min × water rate) as the reference ceiling. Short cold-pipe taps get a halved cost multiplier.
- `recency_weight`: per-year multipliers, most-recent first (default `[3, 2]` — current year ×3, previous year ×2). Configurable via `--recency_weights`.

**Step 2 — Bin into per-day buckets**

Cold-start events are binned into two parallel structures keyed by day-of-week (0 = Sunday … 6 = Saturday) and 5-minute bucket:
- `raw_counts[dow][bucket]` — unweighted hit count (used for `min_occurrences` filter)
- `weighted_scores[dow][bucket]` — sum of combined weights (used for score threshold)

**Step 3 — Peak-finding and window construction**

For each day, dominant activity peaks are found using:
1. Smooth per-bucket scores with a ±2-bucket sliding average.
2. Find local maxima with a minimum `min_peak_separation` (default 45) minute separation.
3. Greedy non-maximum suppression: accept peaks by descending score, reject those within `min_peak_separation` of an already-accepted peak.

An **adaptive threshold** loop starts at `min_weighted_score` (default 6.0) and steps down by 1.0 until `MAX_SLOTS_PER_DAY` (3) peaks are found or `min_score_floor` (default 3.0) is reached. A second pass also relaxes `min_occurrences` by 1 if needed.

Around each accepted peak, a ± `peak_half_width` (default 30) minute window is built. The window start is shifted back by `preheat_minutes` (default = `COLD_PIPE_DRAIN_MINUTES` = 3.0 min from config.py). All boundaries are rounded to the nearest 10-minute increment to match the firmware's 10-minute-resolution encoding.

**Step 4 — Print / estimate**

Always prints the learned schedule. With `--verbose`:
- Shows per-day peak-finding detail.
- Prints a **slot width comparison** table (±25 min vs ±30 min) showing efficiency % for each option.
- Prints an **expected efficiency table** for the coming week: of schedulable cold-starts (those falling inside a slot or within `RECIRC_WINDOW_MINUTES` = 15 min after slot end), what fraction are covered; missed-water-waste and wasted-gas-cycle costs in USD.

`--debug_day <DayName>` shows a detailed per-bucket breakdown for one day including all bucket hit counts, scores, filtering, and peak-selection results, then exits.

**Step 5 — Push**

Pass `--push` to POST the schedule to `http://<esp32_host>:<esp32_port>/schedule`. By default the script is a dry run and prints the JSON that would be sent without pushing.

**Timezone handling:** The script has no local timezone dependency. All cold-start events, bucket indices, `dow`, and `minute_of_day` values are derived from UTC datetimes. Schedule output posted to `POST /schedule` contains UTC hours/minutes. SF morning peaks appear around 14:00–16:00 UTC.

**Defaults and CLI flags:**

| Flag | Default | Description |
|---|---|---|
| `--influxdb_host` | `config.INFLUX_HOST` | InfluxDB hostname |
| `--influxdb_port` | `config.INFLUX_PORT` | InfluxDB port |
| `--influxdb_db` | `config.INFLUX_DB` | InfluxDB database |
| `--esp32_host` | `navien.local` | ESP32 mDNS name or IP |
| `--esp32_port` | `8080` | ESP32 HTTP port |
| `--window_weeks` | `4` | Half-width of rolling seasonal window (±weeks) |
| `--recency_weights` | `3 2` | Per-year multipliers, most-recent first |
| `--cold_gap_minutes` | `10` | Inactivity gap (min) that defines a cold-start |
| `--min_duration_genuine` | `6` | Min 10s buckets (cold pipes) for full weight |
| `--min_duration_recirc` | `3` | Min 10s buckets (recirc on) to count at all |
| `--preheat_minutes` | `3` | Start recirc this many minutes before predicted demand |
| `--gap_minutes` | `10` | Merge events closer than this into one window |
| `--min_occurrences` | `3` | Minimum raw hits to pass noise filter |
| `--min_weighted_score` | `6.0` | Starting score threshold for adaptive filter |
| `--min_score_floor` | `3.0` | Lowest score the adaptive threshold relaxes to |
| `--peak_half_width` | `30` | Half-width (min) of window built around each peak |
| `--min_peak_separation` | `45` | Minimum minutes between two accepted peaks |
| `--push` | off | Push schedule to ESP32 (default: dry run) |
| `--dry_run` | — | Explicit dry-run flag (same as omitting `--push`) |
| `--debug_day` | — | Show per-bucket detail for one day and exit |
| `-v` / `--verbose` | off | Per-day peak detail, slot comparison, efficiency table |

The script is designed to run as a weekly cron job (e.g., Sunday at 2 am):
```
0 2 * * 0  /home/pi/navien/venv/bin/python3 /home/pi/navien_schedule_learner.py --influxdb_host localhost --esp32_host navien.local --push
```

Dependencies: `pip3 install influxdb requests`

---

## On-Device Schedule Learner

The `NavienLearner` class autonomously learns and recomputes the recirculation schedule from live RS-485 observations, eliminating the need for a recurring Pi cron job after an initial bootstrap. It detects cold-start events in real time on Core 1, accumulates bucket data on Core 0 via a FreeRTOS queue, recomputes the schedule nightly, and hands the result to `FakeGatoScheduler` exactly as the Pi's `POST /schedule` does.

### Cold-Start Detection

`NavienLearner::onNavienState()` is called from the water packet callback (Core 1) on every RS-485 packet. A cold-start is detected when `consumption_active` transitions 0→1 after at least `cold_gap` of inactivity:

- **`cold_gap`** = 600 seconds (10 minutes) — inactivity window that separates independent demand events.
- **`min_duration_genuine`** = 60 seconds — minimum tap duration (no recirc) to count at full weight.
- **`min_duration_recirc`** = 30 seconds — minimum tap duration (recirc was on) to count at all.

**Demand weight rules:**

| Condition | `demand_weight` |
|---|---|
| Recirc on at start, duration < 30 s | 0.0 — discard |
| Recirc on at start, duration ≥ 30 s | 1.0 |
| Recirc off at start, duration < 60 s | 0.5 (short cold-pipe tap) |
| Recirc off at start, duration ≥ 60 s | 1.0 |

**Day-of-week and bucket index are pinned to tap-open time**, not tap-close time. A run that opens at 23:58 belongs to the 23:55 bucket of that day, even if it closes after midnight.

**Use `recirculation_active` with a 15-minute lookback to detect covered demands.** The Navien heater cycles the recirc pump on and off to maintain pipe temperature throughout a scheduled slot — `recirculation_running` (pump physically spinning, `flow_state & 0x08`) oscillates between 1 and 0 while the slot is active. A tap opened during the hot/idle phase between pump cycles will see `recirculation_running = 0` even though the pipes are fully pre-heated. `recirculation_active` (`recirculation_enabled & 0x2`) reflects whether recirc *mode* is on and stays true throughout the slot regardless of pump cycling.

Additionally, pipes stay hot for up to 15 minutes after a recirc slot ends (`RECIRC_HOT_WINDOW_SEC = 900`, matching `config.py RECIRC_WINDOW_MINUTES`). The learner tracks `_lastRecircActiveTime` and sets `_recircAtStart = true` if `recirculation_active` is currently true **or** was true within the last 15 minutes. This matches `navien_efficiency.py`'s lookback window exactly.

`recirculation_active` comes from the `recirculation_enabled` byte, independent of `flow_state`, so it does not clear atomically with `consumption_active` — no previous-packet lookback is needed.

> **Diagnostic:** if Measured efficiency shows 0.0% for days with a significant Cold-starts count, the cause is that `_recircAtStart` is never true. Verify: (1) `onNavienState()` receives `water->recirculation_active` (not `water->recirculation_running`); (2) `_lastRecircActiveTime` is updated whenever `recirculation_active` is true; (3) `_recircAtStart` checks both `recirculation_active` and the 15-minute window.

**Queue failure:** If the FreeRTOS cold-start queue cannot be created, `begin()` sets `_learnerDisabled = true` and returns false. All subsequent `onNavienState()` calls return immediately. The rest of the firmware continues normally using the existing NVS/Eve schedule.

### Background Recompute — Core 0 Task

The learner task runs on Core 0. It triggers a full recompute at **midnight + 2 minutes** local time (the previous day's cold-starts are complete and the day-of-week index has just rolled). The recompute processes **one day per iteration** with a 10 ms `vTaskDelay` between days to yield to other Core 0 work.

Manual recomputes (e.g. triggered by `POST /buckets`) set `_recomputeRequested` and follow the same state machine.

**Schedule handoff:** Core 0 never calls `setWeekScheduleFromJSON()` directly. It writes the JSON to `_pendingScheduleJSON` and sets `_newScheduleReady` under a mutex. `FakeGatoScheduler::loop()` on Core 1 takes the mutex non-blockingly each iteration; if a new schedule is ready it copies the JSON, releases the mutex, then calls `setWeekScheduleFromJSON()`. Missing one check is harmless — the schedule is applied on the next loop pass.

### Annual Decay at Year Rollover

On Jan 1 (detected by a `current_year` mismatch in the `buckets.bin` header):

- All `weighted_score` values are multiplied by **2/3** (scaling data that was accumulated at recency weight ×3 to the "last year" weight ×2).
- **`raw_count` is NOT decayed** — it represents actual occurrence count used by the noise filter regardless of year.

If the device is powered off over New Year and boots in January, the mismatch is detected on first boot and decay is applied before any new data is written.

### Peak-Finding Parameters

| Parameter | Value |
|---|---|
| `peak_half_width` | 30 minutes |
| `min_peak_separation` | 45 minutes |
| `preheat_minutes` | 3 minutes |
| `min_weighted_score` | 6.0 (adaptive start) |
| `min_score_floor` | 3.0 (adaptive floor) |
| `smooth_radius` | 2 buckets |
| `MAX_SLOTS_PER_DAY` | 3 |

**Adaptive threshold:** starts at `min_weighted_score` = 6.0, steps down by 1.0 until `MAX_SLOTS_PER_DAY` peaks are found or `min_score_floor` is reached. If still fewer peaks than needed, `min_occurrences` is relaxed by 1 and the pass repeats.

### Efficiency Tracking

Two efficiency metrics are maintained continuously and cached for display.

**Predicted efficiency** — computed at the end of each recompute from the new schedule and current bucket data. For each day, each bucket is checked against the slot boundaries:

- A bucket is *covered* if it falls inside a slot.
- A bucket is *schedulable* if it falls inside a slot **or** within 15 minutes after a slot ends (the hot-water window).
- `predicted% = covered / schedulable × 100`

**Measured efficiency** — rolling 4-week window of actual observations. Each cold-start event records whether recirculation was already running at tap-open time (`recircAtStart`). The counters (`total[dow]` and `covered[dow]`) are updated on Core 0 when each `PendingColdStart` is consumed from the queue. On Sunday midnight the oldest week slot is zeroed and the head advances.

`measured% = covered / total × 100` summed across all 4 weeks per day. Days with no cold-starts in the window report N/A rather than zero.

**Measured efficiency persistence** — the window (`_measured[4]` + `_measuredHead`) is persisted to `/navien/measured.bin` on LittleFS and reloaded on `begin()`, so it survives reboots and OTA firmware updates. The file uses the same atomic `.tmp` → rename write strategy as `buckets.bin`. It is saved automatically at three points:
- **Sunday midnight** — when `advanceMeasuredWeek()` rotates the window.
- **OTA start** — via the `HS_OTA_STARTED` HomeSpan status callback, just before the device reboots to apply the update.
- **On demand** — via the Telnet `saveLearner` command, for planned reboots not triggered through OTA.

**The gap metric:**

```
gap[dow] = predicted[dow] - measured[dow]
```

| Gap | Interpretation |
|---|---|
| < 10% | Schedule and habits are well aligned |
| 10–25% | Normal drift — nightly recompute should self-correct within days |
| > 25% | Habits have shifted significantly; consider rerunning bootstrap |
| Predicted N/A | Insufficient bucket data for this day |
| Measured N/A | No cold-starts observed yet in the rolling window |

### Bootstrap

Without seeding, the device starts cold: `buckets.bin` is empty and the nightly recompute produces no useful schedule for roughly 2–4 weeks. The bootstrap process eliminates this gap.

**Why two steps are required:**

- **Step 1** (`navien_bootstrap.py --push`): queries full InfluxDB history, runs peak-finding on the Pi, and POSTs a finished schedule to `POST /schedule`. This gives the device a correct working schedule immediately.
- **Step 2** (`navien_bucket_export.py --push`): extracts the same historical data and seeds `buckets.bin` via `POST /buckets`. Without this step, the first midnight recompute runs against empty buckets and overwrites the Step 1 schedule with an empty one.

Both steps must be run in order after first flash. They are also used when `buckets.bin` is suspected corrupt or after parameter re-tuning.

**When to run bootstrap:** first flash, LittleFS wiped, corrupt `buckets.bin` suspected, or after algorithm parameter changes. Run only when the device is quiet — avoid the 00:00–00:05 recompute window and any time with active cold-start events being processed.

**Fallback without bootstrap:** if bootstrap is skipped, meaningful peaks emerge after ~2 weeks of live data; the schedule stabilizes after ~4 weeks. The existing NVS/Eve schedule (if any) remains active and unchanged until the first successful recompute.

---

## Startup Sequence

1. UART2 initialized for Navien RS485 at 19200 baud.
2. `NavienLearner::begin()` — mounts LittleFS, loads `buckets.bin`, loads `measured.bin` (restoring the rolling measured-efficiency window; silently starts from zero if absent), and starts the Core 0 learner task.
3. HomeSpan initialized; WiFi credentials managed by HomeSpan pairing. OTA enabled. `HS_OTA_STARTED` status callback registered to save the measured window before any OTA reboot.
4. HomeSpan web log configured with custom CSS and the `navienStatus` callback.
5. HomeKit accessories registered: `DEV_Navien` thermostat with Eve history and scheduler.
6. On WiFi connect:
   - `setupNavienBroadcaster()`: registers Navien packet callbacks; starts UDP broadcast.
   - `setupTelnetCommands()`: registers all commands; starts Telnet server on port 23.
   - `setupScheduleEndpoint()`: starts raw `WiFiServer` on port 8080 for pushed schedule updates and bucket bootstrap ingest.
7. Main loop runs: `telnet.loop()`, `loopScheduleEndpoint()`, `navienSerial.loop()`, `homeSpan.poll()`.
8. Once NTP sync is confirmed (`time(nullptr) > 1700000000L`), `homeSpan.assumeTimeAcquired()` is called to unlock time-dependent HomeKit features. A timezone (`TZ` env var) is no longer required for this gate; TZ is used only for local-time display.
