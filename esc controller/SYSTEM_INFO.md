# ESC Controller — System Reference

## What This Project Is

An Arduino sketch running on an Arduino Nano ESP32 (ESP32-S3) that acts as a throttle/ESC controller with dynamic PWM ramping, three selectable drive modes, and a live OLED display. It sits between a potentiometer/trigger input and a PWM-controlled ESC, shaping the throttle curve so acceleration is smooth and mode-appropriate rather than instantaneous.

---

## Hardware

| Component | Detail |
|-----------|--------|
| Microcontroller | Arduino Nano ESP32 (ESP32-S3, 3.3V) |
| OLED | 128×64 SSD1306 via I2C |
| OLED wiring | SDA → TX/D1/pin1, SCL → RX/D0/pin0 |
| PWM output | LEDC channel 0, ~31 kHz (silent), 8-bit (0–255) |
| ADC | 12-bit (0–4095) |
| Libraries | Adafruit SSD1306, Adafruit GFX, Wire, Arduino |

### Pin Assignments

| Pin | Role |
|-----|------|
| D0 (RX) | OLED SCL |
| D1 (TX) | OLED SDA |
| D2 | Trigger / button (active-low) |
| D4 | ECO mode switch (active-low) |
| D5 | SPORT mode switch (active-low) |
| D9 | PWM output to ESC |
| A0 | Throttle potentiometer |
| D6–D8, D10–D13, A1–A7 | Free / unassigned |

> **Upload method:** Double-tap RST pin to GND → green LED flashes → DFU bootloader active → upload from IDE. On Windows, WinUSB driver via Zadig is required.

---

## File List

| File | Description | Editable? |
|------|-------------|-----------|
| `throttle_controller.ino` | Main and only sketch file | Yes — primary edit target |
| `SYSTEM_INFO.md` | This document — human-readable reference | Update after significant changes |
| `CLAUDE.md` | AI assistant briefing | Update after significant changes |

---

## How the Code Works

### Modes

Selected via active-low switches. ECO wins if both switches are pulled low simultaneously.

| Mode | Ramp Time | Decay Time | Curve |
|------|-----------|------------|-------|
| ECO | ~30 s | 10 s | Exponential: `exp(1.8p) / 2.047` |
| NORMAL | ~15 s | 7.5 s | Shifted sigmoid: `sig(p)*1.5 + 0.15`, center=0.15 |
| SPORT | ~5 s | 7.5 s | Square root: `min(0.5/√p, 5.0)` |

### State Machine

Four states: `IDLE → REENGAGING → RAMPING → HOLDING`

```
IDLE        Trigger released. rampPwm and currentPwm decay at mode rate.
            Output hard-zeroed by trigger safety guard.

REENGAGING  Trigger re-pressed while currentPwm > 2% (RESUME_THRESHOLD).
            rampPwm climbs from 0 to resumeTarget using sport curve at
            REENGAGE_CLIMB_RATE (~3 s). Hands off to RAMPING on arrival.

RAMPING     Climbs toward potPct using the active mode's curve.
            Progress = (rampPwm - rampStartPwm) / 100.0 — always relative
            to segment start, preventing fast-blast on high-value resume.

HOLDING     rampPwm has reached potPct. Tracks pot directly.
            Pot drop: follows immediately.
            Pot rise: re-enters RAMPING with rampStartPwm = rampPwm.
```

### Pot Smoothing

EMA filter: `smoothedPot = POT_ALPHA * raw + (1 - POT_ALPHA) * smoothedPot`  
`POT_ALPHA = 0.2` — filters ±0.5% ADC jitter. Higher = more responsive, lower = smoother.

### OLED Layout (refreshes every 100 ms)

```
NORMAL                RAMP    ← mode left, state right-aligned (size 1)
━━━━━━━━━━━━━━━━━━━━━━━━━━━
        75.0%                 ← output %, size 3, centred, y=13
━━━━━━━━━━━━━━━━━━━━━━━━━━━
Pot:  80.0%                   ← y=42 (size 1)
Ramp: 62.3%                   ← y=54 (size 1)
```

### Serial Telemetry

Emitted every 20 ms (control loop tick):
```
NORMAL | RAMP | pot:80.0% ramp:62.3% resume:0.0% out:62.3%
```

---

## Key Constants / Parameters

| Constant | Value | Meaning |
|----------|-------|---------|
| `ECO_CLIMB_RATE` | 0.00333 | %/ms base climb rate → ~30 s full ramp |
| `NORMAL_CLIMB_RATE` | 0.00667 | %/ms → ~15 s full ramp |
| `SPORT_CLIMB_RATE` | 0.02000 | %/ms → ~5 s full ramp |
| `REENGAGE_CLIMB_RATE` | 0.03333 | %/ms → ~3 s re-engage climb |
| `ECO_DECAY_RATE` | 0.01000 | %/ms → 10 s full decay |
| `NORMAL_DECAY_RATE` | 0.01333 | %/ms → 7.5 s full decay |
| `SPORT_DECAY_RATE` | 0.01333 | %/ms → 7.5 s full decay |
| `RESUME_THRESHOLD` | 2.0 % | Minimum currentPwm to trigger re-engage path |
| `POT_ALPHA` | 0.2 | EMA smoothing coefficient for pot ADC |
| `LEDC_FREQ_HZ` | 31372 | PWM frequency — above human hearing |
| `LEDC_RES_BITS` | 8 | PWM resolution (0–255) |
| `OLED_ADDR` | 0x3C | I2C address of SSD1306 display |
| Control loop | 20 ms | `delay(20)` at bottom of loop() |
| Display refresh | 100 ms | Independent of control loop |

---

## Version History

| Version | Notes |
|---------|-------|
| V1–V15 | Development history (pre-repo) |
| V16 | Re-engage timing fixed; OLED layout finalized; rampStartPwm added to fix fast-blast-on-resume bug |
| V17 | Current version in this folder |
