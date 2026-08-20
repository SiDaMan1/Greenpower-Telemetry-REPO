# CLAUDE.md — ESC Controller

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

A throttle/ESC controller running on a plain **ESP32 WROOM-32 dev board** (switched from an Arduino Nano ESP32 — different chip, different pin map, different upload procedure; see below). A potentiometer sets the target speed; a trigger button enables output; two mode switches (ECO / SPORT) select the ramp curve. PWM is output to an ESC at ~31 kHz via the ESP32 LEDC peripheral. A 128×64 OLED gives live feedback.

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

Both must match. Current version: **V23**. Next edit → V24.

### Hardware Constraints (ESP32 WROOM-32)
- **3.3V logic** — do not connect 5V signals directly to any pin.
- **I2C on standard pins:** SDA = GPIO21, SCL = GPIO22. No Arduino pin-label aliases exist on this board (unlike the Nano ESP32) — pin numbers in code are raw GPIO numbers, full stop.
- **PWM on GPIO25 via LEDC** — do not use `analogWrite()` on ESP32; always use `ledcWrite()`.
- **12-bit ADC** — pot reads 0–4095, not 0–1023. Same on this chip as the Nano ESP32's S3.
- **Active-low inputs** — ECO_PIN, SPORT_PIN, BUTTON_PIN all use `INPUT_PULLUP`. LOW = pressed/active.
- **GPIO6–11 are never usable** — wired internally to the module's flash chip on WROOM-32. Don't route anything to them, ever, even temporarily for testing.
- **GPIO0/2/5/12/15 are boot-strapping pins** — avoided on purpose for every input/output in this sketch. If a future pin change needs one of these, whatever's wired to it must not be able to hold the wrong logic level at power-on, or the board won't boot.
- **THROTTLE_PIN (GPIO34) is deliberately on ADC1, not ADC2** — ADC2 pins stop working entirely whenever WiFi is active. If this board ever gets a WiFi/ESP-NOW link (has been discussed for ESC↔sender telemetry), any *new* analog input added here must also go on an ADC1 pin (GPIO32–39) or it'll silently break the moment WiFi turns on.

### Upload Method (changed with the board swap)
The Nano ESP32's double-tap-RST-to-DFU procedure **no longer applies** — this board doesn't have native USB. WROOM-32 dev boards use a USB-to-serial bridge chip (typically CP2102 or CH340) with automatic reset circuitry, so a normal Arduino IDE upload just works. Board setting in the IDE: **"ESP32 Dev Module"**, not "Arduino Nano ESP32". If upload still fails, the usual WROOM fallback is holding the BOOT button during the "Connecting..." phase — but try a normal upload first.

### Free Pins for Future Use
GPIO4, 15, 18, 19, 23, 26, 32, 33, 35–39 (35/36/39 are input-only). GPIO16/17 are in use for Serial1 UART telemetry.

### Code Style
- Do not introduce abstraction layers beyond what the task requires.
- Do not add error handling for conditions that cannot occur in normal operation.
- The control loop runs at 20 ms (`delay(20)`). The display refreshes every 100 ms. Do not conflate the two.
- `rampStartPwm` is critical — it makes RAMPING progress relative to segment start. Do not remove or bypass it; doing so reintroduces the fast-blast-on-resume bug.

---

## Current State (V23)

- **Hardware changed from Arduino Nano ESP32 to plain ESP32 WROOM-32** — full pin remap, upload procedure changed (no more DFU double-tap), board setting changed to "ESP32 Dev Module"
- Three modes: ECO (~30 s ramp), NORMAL (~15 s), SPORT (~5 s)
- Four states: IDLE, REENGAGING, RAMPING, HOLDING
- Re-engage always uses sport curve at REENGAGE_CLIMB_RATE (~3 s) regardless of mode
- EMA pot smoothing with POT_ALPHA=0.2
- OLED: mode + state top row, large output % centre, pot % and ramp % bottom rows (I2C now on GPIO21/22)
- Serial telemetry emitted every control tick (20 ms)
- **Serial1 UART telemetry active on GPIO17(TX)/GPIO16(RX)** — 20 Hz CSV packets at 115200 baud. Format: `mode,state,setpointPct,livePct,rampPct`. speedMph/batV/rpm/amps are omitted — they come from separate sensors and are merged downstream.

---

## Self-Maintenance Requirement

After every significant change to this project, update **both** `SYSTEM_INFO.md` and `CLAUDE.md`:

- `SYSTEM_INFO.md` — update the version history table, any changed constants/pins/parameters, and the how-it-works section if behaviour changed.
- `CLAUDE.md` — update the **Current State** section and the version number in the Rules section.

"Significant change" means: any new feature, behavioural change, pin reassignment, constant tuning, or state machine modification. Typo fixes and comment edits do not require a doc update (but still require a version bump in the `.ino`).
