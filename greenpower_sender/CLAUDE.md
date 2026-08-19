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
| Temp probe (1-Wire digital) | GPIO6 (needs 4.7kΩ pull-up to VCC) | DS18B20 |
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
- `CURRENT_SENS` (V/A for the YHDC HSTS016L) — still a placeholder pending bench calibration against a known load, don't treat it as verified.
- `VDIV_RATIO_MOTOR = 0.19893`, `VDIV_RATIO_BATT = 0.19954` — **calibrated against a multimeter**, no longer the nominal `1/5`. Motor: sketch read 12.97V vs multimeter 13.04V. Battery: sketch read 13.02V vs multimeter 13.05V. Re-run the procedure below if the resistors are ever swapped or the divider circuits rebuilt.

### Voltage divider calibration procedure
With the sketch running and printing `Motor Volt`/`Batt Volt` to serial:
1. Measure the true voltage at that divider's input node with a multimeter, at the same time the serial dump shows a reading.
2. Compute the corrected ratio: `new_ratio = old_ratio * (sketch_reading / multimeter_reading)`.
   - Example: divider reports 24.60V, multimeter reads 24.35V, old ratio is `1/5` (0.2) → `new_ratio = 0.2 * (24.60/24.35) = 0.2021`.
3. Put that value into `VDIV_RATIO_MOTOR` or `VDIV_RATIO_BATT` (whichever channel you measured) and re-flash.
4. Re-check against the multimeter — one pass is normally enough since the relationship is linear, but repeat if the source voltage was unstable during the first measurement.

Do this once per divider (motor and battery separately) — don't assume one calibrated value applies to both.

### Temp probe is a DS18B20, not analog — don't re-add ADC/NTC code here
Early in the V2 rework this was built as an analog NTC thermistor circuit (divider math, Beta equation, ADC oversampling/calibration) because the sensor was initially described as an "analog output" probe. It's actually a **DS18B20**, a digital 1-Wire sensor — none of that analog machinery applies, and it was the actual cause of the wild/inverted readings seen at the time (a 1-Wire digital pulse train read through an ADC as if it were a steady analog voltage produces meaningless numbers). The sketch now uses `OneWire`/`DallasTemperature` on GPIO6, same as V1 used on GPIO45. If temp readings ever look wrong again, don't reach for divider/thermistor math — check the 4.7kΩ pull-up and the data-line wiring first, and check `tempSensor.getDeviceCount()` in the boot log (0 means the sensor didn't respond).
- 9-bit resolution is set intentionally (`tempSensor.setResolution(9)`) to keep the blocking `requestTemperatures()` call (~94 ms) well under the sketch's 200 ms sensor-poll cadence — 12-bit default (~750 ms) would stall the loop.
- `readTempF()` returns `NAN` on `DEVICE_DISCONNECTED_C` rather than a bogus number — the print block checks `isnan()` and prints a clear DISCONNECTED line instead of a fake temperature.

### GPIO45 is not usable for analog input
This was a real wiring mistake caught early in the V2 rework, before the sensor was correctly identified as the digital DS18B20 above: GPIO45/46 on the ESP32-S3 are dedicated strapping pins with no ADC channel. Kept here as a standing rule for any *future* analog sensor — don't route one to GPIO45 or 46.

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
- Voltage and current sensing moved entirely to the ADS1115; the ESP32's internal ADC is no longer used for anything in this sketch (temp probe is 1-Wire digital, not analog)

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Hardware** table and **Current State** section — and if a `SYSTEM_INFO.md` is added later, keep that in sync too.

"Significant change" means: `telemetry_packet_t` layout change, pin reassignment, calibration constant updated from a real bench measurement, or a sensor added/removed. Comment edits do not require a doc update.
