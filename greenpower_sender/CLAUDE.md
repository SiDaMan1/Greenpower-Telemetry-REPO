# CLAUDE.md — Greenpower Sender

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

The **real** telemetry sender for the Greenpower vehicle, currently built on a **breadboard** around a Heltec ESP32-S3 LoRa WiFi V4. It reads live sensors, prints the results to USB serial, and transmits on two radio links at once: **LoRa** (on-board SX1262, binary `telemetry_packet_t`) for a future long-range base-station receiver, and **ESP-NOW** to the steering wheel [`display_receiver`](../steering_wheel_display/CLAUDE.md) (same CSV format as [`mock_sender`](../mock_sender/CLAUDE.md), so the receiver doesn't care which sender is live).

The ESC/UART link (dropped in the V2 rework, since the breadboard build didn't have the ESC controller connected at the time) is **back as of V3.1** — see [`esc controller`](../esc%20controller/CLAUDE.md), now running on an ESP32 WROOM-32. Both LoRa and ESP-NOW carry real ESC data; the ESP-NOW placeholder fallback (`"---"`, `0.0`) only kicks in if no ESC UART line has ever been parsed.

---

## Files in This Folder

| File | Role | May be edited? |
|------|------|----------------|
| `greenpower_sender.ino` | Entire sketch — sensor reads, RPM ISRs, LoRa TX, ESP-NOW TX, serial debug dump | **Yes — primary target** |
| `config.h` | LoRa radio settings, ESP-NOW peer MAC, `telemetry_packet_t` — all shared with a receiver | **Yes — but see "config.h is shared" below** |
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
| ESC RX (from ESC TX) | GPIO44, Serial2 | [`esc controller`](../esc%20controller/CLAUDE.md), 115200 baud |
| ESC TX (to ESC RX) | GPIO43, Serial2 | wired for symmetry — the ESC's firmware doesn't currently read anything back |

**LoRa (on-board SX1262):** NSS=8 RST=12 DIO1=14 BUSY=13, SPI SCK=9 MISO=11 MOSI=10. 915 MHz, **SF12** (was SF7 — bumped for real-world range after the sender/receiver were found to disconnect at walking distance; see the dedicated rule below), BW125, sync word 0xF3, 22 dBm — see `config.h`. SF12's ~3.8s time-on-air means LoRa now transmits roughly once every ~4.5s (`LORA_TX_INTERVAL_MS`), not 5x/sec — ESP-NOW to the steering wheel is unaffected, still 5Hz on its own separate interval.

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

### `config.h` is shared — coordinate before changing it
`telemetry_packet_t` (94 bytes, packed), the LoRa RF settings, and `ESPNOW_PEER_MAC` are meant to be identical on both ends of each link. There **is** a LoRa receiver in this repo now ([`greenpower_receiver`](../greenpower_receiver/CLAUDE.md)) and its `config.h` must be updated in the same commit as this one — they're two independently-maintained copies, not a shared include, so nothing enforces this automatically. Update the size comment whenever a packet field is added, removed, or reordered.

### ESC UART link — real data as of V3.1, not placeholders
`pollEsc()` runs every `loop()` iteration (same pattern as `pollGps()`) reading CSV lines off Serial2 from [`esc controller`](../esc%20controller/CLAUDE.md)'s `throttle_controller.ino`: `mode,state,setpointPct,livePct,rampPct`. Parsed values land in the module-level `esc` struct; `updateSensors()` copies them into `pkt.esc_*` and sets `PKT_FLAG_ESC_VALID` only once a line has actually been parsed (`esc.valid`) — don't assume the ESC fields are populated from boot, a disconnected/not-yet-booted ESC controller means the flag stays clear and `pkt.esc_*` stays zeroed.
- `esc.mode`/`esc.state` are 8-byte buffers (`char[8]`), matching `telemetry_packet_t.esc_mode`/`esc_state` exactly — this isn't arbitrary, the ESC's own `snprintf` truncates those fields to 7 chars (`%.7s`) before sending, so 8 bytes (7 + null) is exactly enough and intentionally not more.
- `parseEscLine()` bails out early (via early `return`) on any missing comma-separated field, leaving whatever was already in `esc.*` from the previous good line untouched — a single malformed/truncated UART line doesn't corrupt or blank out the last known good ESC state.

### ESP-NOW packet format — must match `mock_sender` and `display_receiver`
```
speed_mph,batV,rpm,amps,mode,state,setpoint%,live%,ramp%
```
Produced by `espNowSend()`, must stay field-for-field identical to what `mock_sender.ino` sends (see [`mock_sender/CLAUDE.md`](../mock_sender/CLAUDE.md)) and what `display_receiver`'s `parsePacket()` expects. `mode`/`state`/the three percent fields now come from the real ESC link (`pkt.esc_*`, gated on `PKT_FLAG_ESC_VALID`) — the `"---"`/`0.0` placeholders only fire if no ESC UART line has ever been successfully parsed since boot, not as a permanent stand-in.

### ESP-NOW peer MAC — critical
`ESPNOW_PEER_MAC` in `config.h` must match the MAC address `display_receiver` prints on boot. Currently `{0x44, 0x1B, 0xF6, 0xCA, 0x38, 0xE4}` — same value used by `mock_sender`, so the two senders are interchangeable from the receiver's point of view.

### LoRa and ESP-NOW are independent, and now run at genuinely different rates — `LORA_TX_INTERVAL_MS` vs `ESPNOW_TX_INTERVAL_MS`
Used to share one constant (`LORA_TX_INTERVAL_MS`, 200ms/5Hz, matching `SENSOR_INTERVAL_MS`) — that stopped being correct once LoRa moved to SF12 (see the dedicated rule below): ESP-NOW is a completely different radio (2.4GHz WiFi-based) with no LoRa-style airtime constraint, and the steering wheel display it feeds has no reason to slow down just because the LoRa base-station link did. `ESPNOW_TX_INTERVAL_MS` (200ms, unchanged) and `LORA_TX_INTERVAL_MS` (now ~4.5s) are two separate constants, each with its own `lastEspNowMs`/`lastLoraTxMs` timestamp — don't reunify them without a reason; they're deliberately decoupled now, not just historically two variables that happened to read the same constant.

### LoRa TX is async now (`startTransmit()`/`setPacketSentAction()`), not blocking `transmit()` — required by SF12, not a style preference
At SF7 (the original setting), a single `radio.transmit()` call blocked for roughly 50ms — imperceptible against a 200ms cadence, so blocking was never a problem. SF12's real time-on-air for this packet's 94-byte payload is **~3.8 seconds** (Semtech's LoRa time-on-air formula: ~401ms preamble + ~3375ms payload at BW125/CR4:5/explicit header/CRC on/low-data-rate-optimize on — RadioLib enables LDRO automatically for SX126x once symbol duration exceeds 16ms, true for SF11/SF12 at this bandwidth). Calling the blocking `transmit()` at that airtime would freeze the ENTIRE `loop()` — GPS parsing, ESC UART polling, RPM period calc, and critically ESP-NOW TX to the steering wheel — for 3.8 out of every ~4.5 seconds, silently defeating the point of giving ESP-NOW its own separate fast interval (see the rule above). Fixed by mirroring the exact interrupt-driven pattern `greenpower_receiver` already uses on its RX side (`setPacketReceivedAction`), just for TX instead of RX:
- `radio.setPacketSentAction(setLoraTxFlag)` (wired in `setup()`, right alongside `setDio2AsRfSwitch`) sets `volatile bool loraTxDoneFlag` from the radio's own IRQ — the ISR touches nothing else, same "keep the ISR trivial" rule the receiver's `setPacketFlag()` already follows.
- `loRaTx()` now calls `radio.startTransmit()` (returns immediately) instead of `radio.transmit()`, and guards against starting a second transmit while `loraTxInFlight` is still true from an unfinished previous one (skips + logs rather than corrupting radio state or blocking) — this should be rare given `LORA_TX_INTERVAL_MS`'s real headroom above SF12's actual airtime, not a normal steady-state occurrence.
- `checkLoraTxComplete()` (called every `loop()` iteration, alongside `pollGps()`/`pollEsc()`, NOT gated behind `SENSOR_INTERVAL_MS`) polls `loraTxDoneFlag` and calls `radio.finishTransmit()` — the required cleanup pairing for an async `startTransmit()`, same idea as the receiver always re-arming `startReceive()` after every `readData()` regardless of success. Checking every iteration (not just once per sensor tick) matters because TX completion can land anywhere within that ~3.8s window, not neatly aligned to the 200ms sensor cadence.
**If LoRa's spreading factor ever changes again, re-derive the real time-on-air** (the formula/assumptions are spelled out in `LORA_TX_INTERVAL_MS`'s own comment) rather than guessing — going back down to SF7-SF9 territory might make blocking `transmit()` viable again, but there's no reason to revert the async pattern just because it's no longer strictly required; it's strictly safer than blocking regardless of spreading factor.

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

## Current State (V3.2 — LoRa SF12 for real-world range, async TX)

- **LoRa moved from SF7 to SF12** (max range for this radio) after real-world testing found the sender/receiver disconnecting at ordinary walking distance — see the dedicated rule above for the full reasoning and the async-TX rework it required.
- **LoRa TX is now async** (`startTransmit()`/`setPacketSentAction()`/`finishTransmit()`, mirroring the receiver's existing interrupt-driven RX pattern) instead of a blocking `transmit()` call — required once SF12's ~3.8s airtime made blocking the whole `loop()` for that long per transmission unacceptable. See the dedicated rule above.
- **LoRa and ESP-NOW now run on genuinely separate intervals** — `LORA_TX_INTERVAL_MS` (~4.5s, SF12 airtime + headroom) and `ESPNOW_TX_INTERVAL_MS` (200ms/5Hz, unchanged) — previously one shared constant paced both. The steering wheel display (ESP-NOW) is unaffected by the LoRa range change.
- Dual-radio: LoRa (SX1262, 915 MHz, SF12/BW125, 22 dBm) + ESP-NOW, each on its own interval (see above)
- Sensor loop @ 5 Hz (`SENSOR_INTERVAL_MS = 200`), RPM recalculated @ 5 Hz (`RPM_CALC_INTERVAL_MS = 200`), ESP-NOW TX @ 5 Hz — **LoRa TX is the one exception now**, at ~4.5s per the SF12 change above; everything else in the pipeline still shares the same 5 Hz cadence, on purpose
- ESP-NOW peer: `{0x44, 0x1B, 0xF6, 0xCA, 0x38, 0xE4}` (steering wheel `display_receiver`)
- **ESC UART telemetry restored** (Serial2, GPIO44 RX/GPIO43 TX, 115200 baud) — real mode/state/setpoint%/live%/ramp% from `esc controller`'s `throttle_controller.ino` (now on ESP32 WROOM-32), flowing through both LoRa (`telemetry_packet_t.esc_*`, `PKT_FLAG_ESC_VALID`) and ESP-NOW
- Voltage and current sensing moved entirely to the ADS1115; the ESP32's internal ADC is no longer used for anything in this sketch (temp probe is 1-Wire digital, not analog)
- `greenpower_receiver` exists and decodes this sender's LoRa packets — `config.h` must be kept in sync with it manually (see above)

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Hardware** table and **Current State** section — and if a `SYSTEM_INFO.md` is added later, keep that in sync too.

"Significant change" means: `telemetry_packet_t` layout change, pin reassignment, calibration constant updated from a real bench measurement, or a sensor added/removed. Comment edits do not require a doc update.
