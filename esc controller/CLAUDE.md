# CLAUDE.md — ESC Controller

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

A throttle/ESC controller running on an Arduino Nano ESP32. A potentiometer sets the target speed; a trigger button enables output; two mode switches (ECO / SPORT) select the ramp curve. PWM is output to an ESC at ~31 kHz via the ESP32 LEDC peripheral. A 128×64 OLED gives live feedback.

The core feature is **dynamic ramp shaping**: instead of jumping instantly to the pot position, the output climbs toward it along a mode-specific curve, with smooth re-engagement after the trigger is released and re-pressed mid-speed.

---

## Files in This Folder

| File | Role | May be edited? |
|------|------|----------------|
| `throttle_controller.ino` | Entire firmware — setup, loop, state machine, display | **Yes — primary target** |
| `SYSTEM_INFO.md` | Human-readable reference doc | **Yes — update after significant changes** |
| `CLAUDE.md` | This file | **Yes — update after significant changes** |

There are no other source files. All logic lives in the single `.ino`.

---

## Rules and Constraints

### Version Bump — MANDATORY
Every edit to `throttle_controller.ino` **must** increment the version number in two places:
1. `Serial.println("Throttle controller ready. VX");` inside `setup()`
2. `display.println("Throttle Ctrl VX");` in the OLED boot message inside `setup()`

Both must match. Current version: **V20**. Next edit → V21.

### Hardware Constraints
- **3.3V logic** — do not connect 5V signals directly to any pin.
- **I2C pins are remapped:** SDA = D1 (TX), SCL = D0 (RX). Do not use default Wire pins.
- **PWM on D9 via LEDC** — do not use `analogWrite()` on ESP32; always use `ledcWrite()`.
- **12-bit ADC** — pot reads 0–4095, not 0–1023.
- **Active-low inputs** — ECO_PIN, SPORT_PIN, BUTTON_PIN all use `INPUT_PULLUP`. LOW = pressed/active.

### Upload Constraint
Double-tap RST to GND to enter DFU bootloader before uploading. Standard upload will not work without this step.

### Free Pins for Future Use
D6, D7, D8, D12, D13, A1–A7. D10 and D11 are now in use for Serial1 UART telemetry.

### Code Style
- Do not introduce abstraction layers beyond what the task requires.
- Do not add error handling for conditions that cannot occur in normal operation.
- The control loop runs at 20 ms (`delay(20)`). The display refreshes every 100 ms. Do not conflate the two.
- `rampStartPwm` is critical — it makes RAMPING progress relative to segment start. Do not remove or bypass it; doing so reintroduces the fast-blast-on-resume bug.

---

## Current State (V17)

- Three modes: ECO (~30 s ramp), NORMAL (~15 s), SPORT (~5 s)
- Four states: IDLE, REENGAGING, RAMPING, HOLDING
- Re-engage always uses sport curve at REENGAGE_CLIMB_RATE (~3 s) regardless of mode
- EMA pot smoothing with POT_ALPHA=0.2
- OLED: mode + state top row, large output % centre, pot % and ramp % bottom rows
- Serial telemetry emitted every control tick (20 ms)
- **Serial1 UART telemetry active on D10(TX)/D11(RX)** — 20 Hz CSV packets at 115200 baud. Format: `mode,state,setpointPct,livePct,rampPct`. speedMph/batV/rpm/amps are omitted — they come from separate sensors and are merged downstream.

---

## Self-Maintenance Requirement

After every significant change to this project, update **both** `SYSTEM_INFO.md` and `CLAUDE.md`:

- `SYSTEM_INFO.md` — update the version history table, any changed constants/pins/parameters, and the how-it-works section if behaviour changed.
- `CLAUDE.md` — update the **Current State** section and the version number in the Rules section.

"Significant change" means: any new feature, behavioural change, pin reassignment, constant tuning, or state machine modification. Typo fixes and comment edits do not require a doc update (but still require a version bump in the `.ino`).
