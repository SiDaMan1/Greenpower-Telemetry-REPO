# CLAUDE.md — Greenpower Sender

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

The **real** telemetry sender for the Greenpower vehicle, currently built on a **breadboard** around a Heltec ESP32-S3 LoRa WiFi V4. It reads live sensors and prints the results to USB serial. Radio transmission (LoRa via the board's on-board SX1262, and ESP-NOW to the steering wheel [`display_receiver`](../steering_wheel_display/CLAUDE.md)) is **not implemented yet** — that's deliberately deferred to a later pass. `telemetry_packet_t` in `config.h` is kept ready as the future payload shape so the data model won't need to change when radio support is added.

There is no ESC/UART link in this build — that was dropped in the V2 rework since the breadboard setup doesn't include the ESC controller.

---

## Files in This Folder

| File | Role | May be edited? |
|------|------|----------------|
| `greenpower_sender.ino` | Entire sketch — sensor reads, RPM ISRs, serial debug dump | **Yes — primary target** |
| `config.h` | `telemetry_packet_t` — the future shared packet struct | **Yes — but see "Packet struct" below** |
| `CLAUDE.md` | This file | **Yes — update after significant changes** |

There is no `SYSTEM_INFO.md` in this folder yet.

---

## Hardware (breadboard, all point-to-point wiring)

| Signal | ESP32-S3 pin | Component |
|---|---|---|
| GPS RX (from GPS TX) | GPIO34 | Adafruit Ultimate GPS Breakout V3, Serial1 |
| GPS TX (to GPS RX) | GPIO33 | " |
| I2C SDA | GPIO17 | shared bus: IMU + ADS1115 |
| I2C SCL | GPIO18 | " |
| Temp probe (analog) | GPIO6 (ADC1_CH5) | NTC thermistor |
| Motor RPM | GPIO4 | laser-interrupt disc sensor, unbranded |
| Wheel RPM | GPIO3 (boot-strapping pin — fine as input post-boot) | laser-interrupt disc sensor, unbranded |
| VEXT power rail | GPIO36 (active LOW) | Heltec V4 external sensor rail |

**ADS1115 (Lonely Binary board, I2C addr 0x48)** — shared bus with the IMU:
| Channel | Signal |
|---|---|
| A0 | motorVolt, via 5:1 voltage divider (VCC < 25 V) |
| A1 | battVolt, via 5:1 voltage divider (VCC < 25 V) |
| A2 | Vout — YHDC HSTS016L 100A current sensor output |
| A3 | Vref — YHDC HSTS016L reference, read directly (no assumed zero) |

**IMU** is an HW-123 breakout (MPU6050-compatible), read with `Adafruit_MPU6050` at the default address `0x68`.

### Critical: ADS1115 must be powered from 5V, not 3.3V
The voltage-divider channels (A0/A1) can swing up to ~5V (25V max input ÷ 5:1 divider). If the ADS1115's VDD is 3.3V, those channels will clip regardless of gain setting. Wire ADS1115 VDD to a 5V rail.

---

## Rules and Constraints

### `config.h` / `telemetry_packet_t` — not yet used for transmission, but keep it receiver-ready
The struct is unused by any radio code right now, but it's the intended future LoRa/ESP-NOW payload. Treat field changes the same as if a receiver already depended on it: update the size comment (currently 66 bytes, packed) whenever a field is added, removed, or reordered.

### ADS1115 gain is switched per-read — don't assume a fixed gain
`readDividerVoltage()` sets `GAIN_TWOTHIRDS` (±6.144V FSR) before reading A0/A1; `readCurrentAmps()` sets `GAIN_ONE` (±4.096V FSR) before the A2-A3 differential read. These are deliberately different — the divider channels need the wider range to avoid clipping near 5V, the current channel wants the extra resolution. If you add a new ADS1115 channel, set its own gain explicitly rather than assuming the previous read's gain is still active.

### Current sensing is differential, not single-ended
`readCurrentAmps()` uses `ads.readADC_Differential_2_3()` (Vout − Vref) instead of a fixed `CURRENT_ZERO_V` constant like the old design did. This is intentional — the sensor's Vref pin is wired to A3 specifically so the true zero point is measured every sample instead of assumed. Don't reintroduce a hardcoded zero-offset constant; only `CURRENT_SENS` (V per A) needs calibration.

### Calibration constants that still need real numbers
These are placeholders pending bench calibration against known references — don't treat them as verified:
- `CURRENT_SENS` (V/A for the YHDC HSTS016L) — depends on the sensor's actual supply voltage.
- `NTC_SERIES_R`, `NTC_R0`, `NTC_BETA` — assume a common 10 kΩ/B=3950 NTC probe wired as `3.3V → series R → ADC tap → NTC → GND`. If the real probe's datasheet gives different values, or the divider is wired the opposite way (NTC on top), update `readTempF()` accordingly.
- `VDIV_RATIO` (currently exactly `1/5`) — confirm against the actual measured resistor values, not just the nominal "5:1" spec.

### GPIO45 is not usable for analog input
This was a real wiring mistake caught during the V2 rework: GPIO45/46 on the ESP32-S3 are dedicated strapping pins with no ADC channel. The temp probe was moved to GPIO6 (ADC1_CH5) for this reason — don't route any new analog sensor to 45 or 46.

### RPM sensing — polarity-agnostic, period-based (V2.2 rework)
The original ISR design assumed the sensors idled LOW and pulsed HIGH (`RISING` + `INPUT_PULLDOWN`) — wrong for these unbranded modules, so it read zero. It was also wired to the wrong pins entirely (GPIO47/48, never connected — the sensors actually landed on **GPIO4 (motor) / GPIO3 (wheel)**; confirmed working via `rpm_pin_test.ino`). Once wiring was fixed, RPM values only moved in large fixed steps (~60 RPM per detected edge) because the first rework counted edges in a fixed 500 ms window — coarse by construction at low pulse rates.

The current version instead measures the **time between pulses** and converts that period directly to RPM, giving a smooth continuous value that updates on every single pulse instead of once per window:

- Edges are still detected with `CHANGE` (polarity-agnostic — works whether the sensor idles HIGH or LOW). Since one beam-block event produces two edges, only every *other* debounced edge (tracked via `motorParity`/`wheelParity`) is treated as a pulse boundary; the time between two such boundaries is a genuine full-pulse period regardless of which physical direction is rising vs. falling.
- `RPM_PIN_MODE` defaults to `INPUT_PULLUP` (idle HIGH) — the most common behavior for these break-beam modules. If a sensor's output floats or reads inverted, this is the first thing to try changing.
- If RPM reads exactly double the real value (verify by spinning a disc by hand and counting), flip `RPM_COUNT_BOTH_EDGES` to `0` and pick a single edge via `RPM_SINGLE_EDGE_MODE` — each detected edge then directly marks a pulse boundary.
- `RPM_STALE_MS` (1000 ms default) reports 0 RPM once a channel has gone that long without a new pulse, instead of freezing on the last computed speed when the disc actually stops.
- `RPM_DEBOUNCE_US` (1.5 ms default) is an electrical-noise floor only, not meant to filter real slot pulses — shrink it if RPM looks capped at very high speed.
- The serial dump still prints raw edge counts *and* live `digitalRead()` pin state every cycle for wiring diagnosis: edges always 0 with a pin state that never changes means a wiring/power problem, not a code problem — check sensor VCC/GND and the actual signal wire before touching the sketch. `rpm_pin_test.ino` (same folder) is a minimal standalone sketch for exactly this check, independent of the interrupt/RPM logic.

### Boot sequence order matters
VEXT power rail is enabled and given time to stabilize *before* I2C init — keep that order; IMU/ADS1115 detection depends on the rail being up first.

### Code style
- `pollGps()` runs every `loop()` iteration to keep the UART buffer drained; heavier work (`updateSensors`, serial dump) is gated behind `SENSOR_INTERVAL_MS`. Keep that split.
- Keep the ASCII banner section headers (`════...`) — used throughout both `.ino` and `.h` files in this repo for visual navigation.

---

## Current State (V2 — breadboard rework)

- Sensor-only, serial-print build — no LoRa, no ESP-NOW yet
- Sensor loop @ 5 Hz (`SENSOR_INTERVAL_MS = 200`), RPM recalculated @ 2 Hz
- ESC/UART telemetry removed (was present in V1, not part of this breadboard build)
- Voltage and current sensing moved entirely to the ADS1115; the ESP32's internal ADC is now only used for the temp probe

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Hardware** table and **Current State** section — and if a `SYSTEM_INFO.md` is added later, keep that in sync too.

"Significant change" means: `telemetry_packet_t` layout change, pin reassignment, calibration constant updated from a real bench measurement, or a sensor added/removed. Comment edits do not require a doc update.
