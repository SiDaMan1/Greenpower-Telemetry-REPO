# CLAUDE.md — Greenpower Receiver

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

A pure **LoRa → USB serial relay**. It listens for `telemetry_packet_t` frames transmitted by [`greenpower_sender`](../greenpower_sender/CLAUDE.md) over the on-board SX1262 and prints each one to USB serial as it arrives — as of V1.1, that includes real ESC data (mode/state/setpoint%/live%/ramp%), not just the sensor fields. No ESP-NOW, no display, no onward transmission of any kind — this is meant to sit on a laptop/base-station desk, plugged in over USB, either read directly in a serial monitor or parsed by another program watching that port.

This is a **different device** from [`steering_wheel_display`](../steering_wheel_display/CLAUDE.md), which receives over ESP-NOW (not LoRa) and drives a physical dashboard. The two receivers serve different purposes and don't share code.

---

## Files in This Folder

| File | Role | May be edited? |
|------|------|----------------|
| `greenpower_receiver.ino` | Entire sketch — LoRa RX, serial dump | **Yes — primary target** |
| `config.h` | LoRa radio settings + `telemetry_packet_t` — must match `greenpower_sender`'s copy exactly | **Yes — but see "config.h must stay in sync" below** |
| `CLAUDE.md` | This file | **Yes — update after significant changes** |

There is no `SYSTEM_INFO.md` in this folder yet.

---

## Hardware

Assumed to be another **Heltec ESP32-S3 LoRa WiFi V4** (same board as the sender), using the same on-board SX1262 pins: NSS=8, RST=12, DIO1=14, BUSY=13, SPI SCK=9/MISO=11/MOSI=10. This assumption was made without confirming the actual receiver hardware — if it turns out to be a different board, the pin `#define`s in both `greenpower_receiver.ino` (SPI pins) and `config.h` (NSS/RST/DIO1/BUSY) need to be updated to match, and this section corrected.

No other peripherals — no sensors, no GPS, no display. Just the radio and USB.

---

## Rules and Constraints

### `config.h` must stay in sync with `greenpower_sender/config.h`
This is two independently-maintained copies of the same file, not a shared include — Arduino sketches in separate folders can't easily share a header across sketch boundaries, so both sides keep their own copy by convention (same pattern used by `mock_sender`/`display_receiver` elsewhere in this repo). If `telemetry_packet_t` or the LoRa RF settings (frequency, bandwidth, spreading factor, coding rate, sync word) change on the sender side, copy the change here too, in the same commit — a mismatch means packets either won't decode or won't be heard at all, usually with no obvious error message pointing at the real cause.
**Caught exactly this trap once already, worth remembering**: the spreading factor isn't actually in `config.h` on either side — it's a literal argument to each sketch's own `radio.begin(...)` call in the `.ino` file, so "keep `config.h` in sync" doesn't automatically cover it. When the sender moved from SF7 to SF12 for real-world range (see `greenpower_sender/CLAUDE.md`), the receiver's own `radio.begin()` spreading-factor argument had to be changed by hand in the SAME pass — it's not something a `config.h` diff would ever surface on its own.

### Interrupt-driven receive — keep the ISR trivial
`setPacketFlag()` only sets a `volatile bool`; all real work (`radio.readData()`, parsing, printing) happens in `loop()`. Don't add radio calls or `Serial.print` inside the ISR itself — RadioLib's SPI transactions aren't ISR-safe, and blocking work in an interrupt handler risks missing the next packet or crashing.

### Always call `radio.startReceive()` again after handling a packet
This happens unconditionally at the bottom of `loop()`, including after CRC errors and other read failures — skipping it on the error paths would leave the radio in a finished-RX state that never hears another packet. Don't add an early `return` on an error branch without restarting receive first.

### Serial protocol is a contract with `receiver_agent` — don't change it silently
Added in the V1.1 pass to support the local forwarder agent (`../receiver_agent`): a `DEVICE_ID` beacon (`GREENPOWER_RX_V1`) printed once at boot and on-demand in response to `ID?\n`, plus a `JSON:`-prefixed machine-readable line after every successfully decoded packet (exact behavior is documented in the header comment block at the top of the .ino).

`receiver_agent` only parses the `JSON:` line generically (`JSON.parse()` + forward the whole object, no per-field schema) — it does NOT hardcode field names, so **purely additive** JSON changes (new fields alongside all existing ones, like the ESC fields added in this same pass) don't need a `DEVICE_ID` bump. Only bump the version suffix — and update the agent's matching `DEVICE_ID` constant in the same change — for a change the agent's *handshake* itself would care about, i.e. anything that isn't purely additive to the JSON payload.

### RSSI/SNR are receiver-only diagnostics
`radio.getRSSI()`/`radio.getSNR()` reflect this device's radio, not anything in `telemetry_packet_t` — they're printed per-packet as link-quality info, not part of the sender's data. Don't confuse the two when adding new printed fields.

### Code style
- Keep the ASCII banner section headers (`════...`) and print-dump field ordering, matching `greenpower_sender.ino`'s serial dump — makes the two easy to compare side-by-side when debugging a link issue.

---

## Current State (V1.2 — SF12, matching the sender's range change)

- **LoRa spreading factor bumped from SF7 to SF12** to match `greenpower_sender`'s move to max range (real-world disconnects at walking distance on SF7) — `radio.begin()`'s spreading-factor argument updated to `12`, same as the sender. See that project's CLAUDE.md for the full reasoning (time-on-air math, the sender-side async-TX rework this required). Nothing else on the RX side needed to change — this device only listens, so SF12's long airtime doesn't create a blocking-call problem here the way it did on the sender's TX path; packets now just arrive roughly once every ~4.5s instead of 5x/sec.
- LoRa RX only — no ESP-NOW, no display, no packet forwarding beyond USB serial
- Interrupt-driven receive via RadioLib (`setPacketReceivedAction` + `startReceive()`), not blocking/polled
- Prints full packet contents plus RSSI/SNR/packet count on every successful receive; CRC mismatches and other read errors are counted and logged, not silently dropped
- `telemetry_packet_t` is now 94 bytes (was 66) — ESC fields (`esc_mode`, `esc_state`, `esc_setpoint_pct`, `esc_live_pct`, `esc_ramp_pct`, `PKT_FLAG_ESC_VALID`) added to match `greenpower_sender`'s restored ESC UART link; both the pretty dump and the `JSON:` line include them
- Assumes a second Heltec ESP32-S3 LoRa WiFi V4 as the host board — **unconfirmed**, see Hardware section
- Speaks a small serial protocol to `../receiver_agent`: `DEVICE_ID` beacon/handshake (`GREENPOWER_RX_V1`) + a `JSON:` line per packet

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Hardware** section and **Current State** section.

"Significant change" means: `telemetry_packet_t` layout change, LoRa RF setting change, pin reassignment, or actual host-hardware confirmation/correction. Comment edits do not require a doc update.
