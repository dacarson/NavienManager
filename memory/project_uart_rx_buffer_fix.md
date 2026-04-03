---
name: UART Rx Buffer Overflow Fix
description: Root cause and fix for recurring RS485 packet parse errors (checksum errors)
type: project
---

Increasing ESP32 HardwareSerial Rx buffer from default 256 bytes to 1024 bytes (via `navienSerial.setRxBufferSize(1024)` before `navienSerial.begin()`) eliminated recurring "Status Packet checksum error" messages.

**Why:** `homeSpan.poll()` occasionally stalls long enough for the 256-byte default UART ring buffer to overflow at 19200 baud (~133ms to fill). Dropped bytes corrupted the tail of in-flight packets — the symptom was the next packet's header bytes (`F7 05 50...`) appearing at the end of the current packet's body. At 1024 bytes the buffer holds ~533ms, enough headroom for HomeSpan WiFi/HomeKit activity.

**How to apply:** If parse errors reappear in future, check whether something new is blocking `loop()` for >533ms (OTA updates, heavy WiFi reconnects, etc.). The fix is already in place in NavienManager.ino. Also note: the GAS_DATA struct comment said "// length is 47, ie 0 - 46" but the struct has 42 fields matching `len=0x2A=42`; corrected to "// length is 42, ie 0 - 41".
