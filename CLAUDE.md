# NavienManager — Claude Code Guidelines

## Project overview
ESP32-based Navien tankless water heater manager running HomeSpan (HomeKit). The firmware
is Arduino/ESP-IDF; supporting tools live in `Logger/` (Python).

## Startup sequencing — CRITICAL

The ESP32 boots in two phases:
1. **Pre-WiFi** — `setup()` runs, HomeSpan initialises, `NavienLearner::begin()` is called.
   At this point WiFi is **not connected** and the `AsyncUDP udp` object (defined in
   `NavienBroadcaster.ino`) is **not initialised**. The system clock (`time()`) returns a
   small positive or zero value and **cannot be trusted**.
2. **Post-WiFi / post-NTP** — WiFi connects, NTP syncs, and `time()` returns a valid Unix
   timestamp (> ~1.7 billion). Only at this point is it safe to call `udp.broadcastTo()`
   or rely on wall-clock time.

**`NavienLearner::broadcastUDP()` calls `udp.broadcastTo()`. Calling it before WiFi is up
will assert-fault the FreeRTOS queue and put the device into a reboot loop.**

### Rules that follow from this

- **Never call `broadcastUDP()` (directly or via `recomputeWrite()`) from `begin()`** or
  from any code path that runs synchronously during `setup()`.
- **Never set `_recomputeRequested = true` from `begin()`** — the recompute task will
  call `recomputeWrite()` → `broadcastUDP()` before WiFi is ready.
- The correct place to trigger a startup recompute is inside `idleStep()`, gated on
  `_startupDecayDone` (which is only set once `time() > 0`, i.e. after NTP sync). This is
  exactly what the current `_taskState = DECAY_CHECK` line does after the startup decay.
- Any other code that needs a valid clock or an active network socket must similarly be
  deferred until `_startupDecayDone` is true or until WiFi events fire.

## Architecture notes

- **Core 0** runs `NavienLearner::learnerTask` (state machine: IDLE → DECAY_CHECK →
  RECOMPUTE_LOAD → RECOMPUTING → RECOMPUTE_WRITE → IDLE).
- **Core 1** runs the Arduino `loop()` / HomeSpan / HTTP handlers.
- `ingestBucketPayload()` runs on Core 1 and mutates `BucketStore` directly; it must only
  be called while the Core 0 task is idle (bootstrap / quiet period).
- `_recomputeRequested` is the only cross-core signal; it is set on Core 1 and consumed on
  Core 0 in `idleStep()`.
