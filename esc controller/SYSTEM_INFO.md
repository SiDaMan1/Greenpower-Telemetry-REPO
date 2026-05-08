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
| D10 | Serial1 TX → display_receiver RX |
| D11 | Serial1 RX → display_receiver TX |
| D6–D8, D12, D13, A1–A7 | Free / unassigned |

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

### Serial Telemetry (USB debug)

Emitted every 20 ms (control loop tick):
```
NORMAL | RAMP | pot:80.0% ramp:62.3% resume:0.0% out:62.3%
```

### UART Telemetry (Serial1 → display_receiver)

Emitted every 50 ms (20 Hz) on D10/D11 at 115200 baud. Sends only the fields this device knows — speedMph, batV, rpm, and amps come from separate sensors and are merged downstream.

```
mode,state,setpointPct,livePct,rampPct
```

Example:
```
NORMAL,RAMP,80.0,62.3,62.3
```
Wiring: D10(TX) → receiver RX, D11(RX) → receiver TX, GND → GND.

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
| `UART_TX_PIN` | 21 (GPIO for D10) | Serial1 TX pin (→ display_receiver RX) |
| `UART_RX_PIN` | 38 (GPIO for D11) | Serial1 RX pin (→ display_receiver TX) |
| `UART_MS` | 50 ms | UART send interval (20 Hz) |
| Control loop | 20 ms | `delay(20)` at bottom of loop() |
| Display refresh | 100 ms | Independent of control loop |
| UART send | 50 ms | Independent timer, 115200 baud |

---

## Version History

| Version | Notes |
|---------|-------|
| V1–V15 | Development history (pre-repo) |
| V16 | Re-engage timing fixed; OLED layout finalized; rampStartPwm added to fix fast-blast-on-resume bug |
| V17 | In this folder at repo creation |
| V18 | Added Serial1 UART telemetry on D10/D11 — 20 Hz CSV to display_receiver |
| V19 | Removed speedMph/batV/rpm/amps from UART packet — supplied by separate sensors downstream |
| V20 | Fixed UART GPIO bug — Serial1.begin() needs GPIO numbers (21/38), not Arduino pin labels (10/11) |
