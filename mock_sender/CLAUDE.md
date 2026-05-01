# CLAUDE.md — Mock Sender

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

A test-utility Arduino sketch for an ESP32. It transmits simulated telemetry data via **ESP-NOW** to a companion `display_receiver` device, allowing the receiver's display and parsing logic to be developed and tested without any live hardware producing real data.

It runs a 10-second looping ride scenario (ramp → hold → re-engage burst → idle) and streams it as CSV packets at 20 Hz. It has no display, no sensors, and no user input — it just sends.

---

## Files in This Folder

| File | Role | May be edited? |
|------|------|----------------|
| `mock_sender.ino` | Entire sketch — simulation, ESP-NOW setup, send loop | **Yes — primary target** |
| `SYSTEM_INFO.md` | Human-readable reference doc | **Yes — update after significant changes** |
| `CLAUDE.md` | This file | **Yes — update after significant changes** |

There are no other source files. All logic lives in the single `.ino`.

---

## Rules and Constraints

### Receiver MAC — Critical
`RECEIVER_MAC` must match the MAC address printed by `display_receiver` on boot. If it's wrong, all packets are silently dropped. Current value: `{0x44, 0x1B, 0xF6, 0xCA, 0x38, 0xE4}`.

### Packet Format — Must Not Change Without Coordinating with display_receiver
The CSV field order is:
```
speedMph,batV,rpm,amps,mode,state,setpointPct,livePct,rampPct
```
`display_receiver`'s `parsePacket()` expects exactly this order. If you add or reorder fields here, update the receiver too.

### Version Tracking
The source currently has **no version string**. If adding one, place it in `setup()` as:
```cpp
Serial.println("Mock sender ready. V2");
```
and increment on every subsequent edit.

### Hardware Constraints
- Any ESP32 board works — no peripherals are required.
- Wi-Fi must be in station mode (`WIFI_STA`) for ESP-NOW to function.
- Do not enable AP mode or connect to a router — ESP-NOW is peer-to-peer only.

### Simulation Fidelity
The `buildFrame()` function mirrors the demo tick in `display_receiver`. If the receiver's demo scenario changes, update `buildFrame()` to match so the two stay in sync during testing.

### Code Style
- Do not add persistent state (globals that accumulate across loop iterations) beyond what already exists (`lastSend`).
- `SEND_MS = 50` (20 Hz) is intentional — do not raise above 100 Hz without reason; the receiver does not benefit.
- Keep `buildFrame()` pure (no side effects, no `Serial` calls inside) so it can be tested in isolation.

---

## Current State (V1 — no version string in source)

- Single target: `RECEIVER_MAC = {0x44, 0x1B, 0xF6, 0xCA, 0x38, 0xE4}`
- Send rate: 20 Hz (SEND_MS = 50)
- 10-second cycle: RAMPING (0–4.5 s) → HOLD (4.5–6.5 s) → REENG burst (6.5–7.2 s) → IDLE decay (7.2–10 s)
- Battery simulation: 25.4 V draining at 2 V/hr, resets at 20 V floor
- Mode cycles: NORMAL → SPORT → ECO every 12 s
- No display, no sensors, no user input on this device

---

## Self-Maintenance Requirement

After every significant change to this project, update **both** `SYSTEM_INFO.md` and `CLAUDE.md`:

- `SYSTEM_INFO.md` — update the version history table, packet format section, constants table, and simulation description if behaviour changed.
- `CLAUDE.md` — update the **Current State** section and the receiver MAC if it changed.

"Significant change" means: packet format change, new simulation phase, timing adjustment, MAC update, or new hardware dependency. Comment edits do not require a doc update.
