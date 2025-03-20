![Github License](https://img.shields.io/github/license/dacarson/NavienManager) ![Github Release](https://img.shields.io/github/v/release/dacarson/NavienManager?display_name=tag)

# NavienManager
 HomeKit and diagnostics for ESP32 + RS485


## Description
1. Allows for vacation mode
2. Allows for hot water now

When the user sets the thermostat as off, turn off recirculation. This will stay off till the next scheduled event.
When the user says they want heat, turn on the schedule override and turn on recirculation for the next 5 mins.
When the user changes to Auto, then return to the current schedule.

## Setup
### Hardware

### Software

Eve app, Hot Water. Valve position is hidden by default.
## License
This library is licensed under [MIT License](https://opensource.org/license/mit/)
