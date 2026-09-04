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
**Caught exactly this trap once already, worth remembering**: the spreading factor isn't actually in `config.h` on either side — it's a literal argument to each sketch's own `radio.begin(...)` call in the `.ino` file, so "keep `config.h` in sync" doesn't automatically cover it. When the sender moved from SF7 to SF12 for real-world range (see `greenpower_sender/CLAUDE.md`), the receiver's own `radio.begin()` spreading-factor argument had to be changed by hand in the same pass. **That whole SF12 attempt has since been reverted — see the ⚠️ rule below.**

### ⚠️ SF12 was tried, confirmed to hang THIS device (not just the sender), and has been fully reverted to SF7
Real, hardware-confirmed finding, and a useful correction to a wrong assumption made at the time: the original reasoning was "this device only listens (interrupt-driven RX, no TX), so SF12's long airtime can't create the blocking-call problem it does on the sender's TX path" — that assumption turned out to be irrelevant to what actually broke. The hang isn't a runtime TX/RX blocking issue at all — it happens during `radio.begin()` itself, during `setup()`, before any actual transmit or receive ever occurs. Since `radio.begin()` with an SF12 parameter is called identically on both sender and receiver, **both devices hang the same way, for the same reason, regardless of which side transmits and which side only listens.** Confirmed directly: after flashing this firmware, the board printed NOTHING at all to Serial — not even the `[BOOT] Greenpower Receiver V1`/`DEVICE_ID` lines that execute before `radio.begin()` is ever called — meaning `setup()` itself never returned, on more than one physical board. Reverted in full at the time; `radio.begin()`'s spreading-factor argument went back to `7`. See `greenpower_sender/CLAUDE.md`'s matching ⚠️ rule for the fuller investigation.

**SF10 was the actual second attempt, per an explicit follow-up request to re-try max range "but don't break anything this time."** `radio.begin()`'s spreading-factor argument was set to `10`, matching the sender — NOT `12` again; SF12's root cause was never actually confirmed, only reverted, so going straight back to it would have been the same unverified gamble that just cost multiple bricked boards. SF10 stays below the symbol-duration threshold that requires RadioLib to enable low-data-rate-optimization for SX126x, the leading (unconfirmed at the time) suspect for the SF12 hang — a smaller, better-reasoned step, not a proven-safe one. See `greenpower_sender/CLAUDE.md`'s matching SF10 rule for the full reasoning, including why the sender also needed its async-TX rework reintroduced (this device didn't — same reasoning as before, listening-only RX has no blocking-call exposure regardless of spreading factor).

### ⚠️ SF10 ALSO confirmed to hang this board on real hardware — reverted to SF7 a second time
**Confirmed, not theoretical**: after the board was reflashed with the SF10 firmware (plus the `epoch_time`/`hdop_x10`/`esc_mode_code` packet changes from the same pass), it produced **zero serial output at all** when connected — not even the `[BOOT] Greenpower Receiver V1`/`DEVICE_ID` boot beacon, which prints *before* `radio.begin()` is ever called. Confirmed directly, byte-level: a raw serial sniff (opening the port and reading for 5 full seconds, independent of `receiver_agent`) received exactly 0 bytes. Since the packet-format changes only run *after* `radio.begin()` succeeds, and the boot beacon itself never appeared, this points at `radio.begin()` with the SF10 parameter hanging the board the same way SF12 did — the "SF10 stays below the LDRO threshold, so it should be safe" reasoning above was a theory, and this is now a second real, hardware-confirmed data point that it wasn't sufficient. The LDRO threshold specifically is no longer a reliable predictor of what does/doesn't hang this board's `radio.begin()` call.
**Reverted to SF7 again** — `radio.begin()`'s spreading-factor argument is back to `7`. The `epoch_time`/`hdop_x10`/`esc_mode_code` packet-format changes were NOT reverted (they run after `radio.begin()` and aren't implicated) — only the spreading factor. **The sender's own copy also needs reverting to match** (see `greenpower_sender/CLAUDE.md`'s matching entry) — SF7 here and SF10 there means packets simply won't decode, not a working link.
**The underlying range problem is unsolved again** — two independent attempts (SF12, SF10) at increasing spreading factor for range have now both hung this exact board's `radio.begin()`. Don't try SF8/SF9 (or SF10 again) without new information about why `radio.begin()` itself hangs at some spreading factors and not others on this specific board — this looks like it may not be a LoRa-parameter-tuning problem at all, but something more fundamental about this board's SX1262 init sequence at anything above SF7. Worth investigating RadioLib's GitHub issues for this exact chip/board combo, or testing spreading factors one step at a time (SF8, then SF9) with a Serial Monitor attached, before assuming any specific value is safe.

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

### Packet shrink — `hdop_x10`/`esc_mode_code`/`esc_state_code` decoded back to real values here
`telemetry_packet_t` shrank from 98 → 81 bytes on the sender side (see `greenpower_sender/CLAUDE.md`'s matching rule) to cut LoRa airtime further, on top of `epoch_time`'s own addition:
- **`hdop_x10`** (`uint8_t`, was `hdop` as `float`) — divide by 10.0 to get real HDOP; `PKT_HDOP_NO_FIX` (255) means "no fix" (replaces the old `99.9f` placeholder, which no longer fits in a byte).
- **`esc_mode_code`/`esc_state_code`** (`uint8_t` each, were `esc_mode[8]`/`esc_state[8]` as ASCII strings) — `escModeToStr()`/`escStateToStr()` (this file) convert the codes back to the same strings (`"ECO"/"NORMAL"/"SPORT"`, `"IDLE"/"REENG"/"RAMP"/"HOLD"`) for the pretty dump and the `JSON:` line's `esc_mode`/`esc_state` fields — from `receiver_agent`'s point of view the JSON shape is unchanged, still real strings, just decoded here instead of carried as strings over the air. `PKT_ESC_MODE_*`/`PKT_ESC_STATE_*` (`config.h`) must match `../esc%20controller/throttle_controller.ino`'s `modeName()`/`stateName()` exactly.

This is purely a wire-format change — the `JSON:` line's own shape didn't change (still real `esc_mode`/`esc_state`/`hdop` strings/numbers), so this did **not** need a `DEVICE_ID` bump per the "Serial protocol is a contract" rule below.

### `epoch_time` — decoded and formatted receiver-side, not on the wire
`telemetry_packet_t.epoch_time` (added in the same pass that added the sender's DS1307 RTC — see `greenpower_sender/CLAUDE.md`) is a raw `uint32_t` Unix-seconds value, `0` meaning "sender has no RTC." This device decodes it into a human `"YYYY-MM-DD HH:MM:SS"` string with `gmtime_r()`/`strftime()` (both standard C, no extra library) purely for the pretty serial dump and the `JSON:` line's `timestamp` field — that formatting happens entirely after the packet has already arrived over LoRa, so it has zero effect on airtime/latency. The `JSON:` line carries both `epoch_time` (raw int) and `timestamp` (formatted string) — raw for anything downstream that wants to do its own date math (e.g. `new Date(epoch_time * 1000)` in JS), formatted for anything that just wants to display it. `0`/`"NO_RTC"` means the sender's RTC wasn't detected at boot — not a real 1970-01-01 timestamp.

## Current State (V1.5 — SF10 CONFIRMED to hang this board too, reverted to SF7 a second time)

- **SF10 confirmed to hang this board on real hardware** (zero serial output at all, including the pre-`radio.begin()` boot beacon) — reverted `radio.begin()`'s spreading factor back to `7`. Packet-format changes (`epoch_time`/`hdop_x10`/`esc_mode_code`) were kept, only the spreading factor reverted. **The range problem is unsolved again** — see the dedicated ⚠️ rule above. **The sender must also be reverted to SF7 to match**, or the two boards won't decode each other's packets at all.
- **Packet shrunk: `hdop_x10`/`esc_mode_code`/`esc_state_code` replace `hdop`(float)/`esc_mode[8]`/`esc_state[8]`** — 98 → **81 bytes**. Decoded back to real values/strings here via `escModeToStr()`/`escStateToStr()` and a `/10.0` divide, purely receiver-side. See the dedicated rule above.
- **`telemetry_packet_t.epoch_time` added** (`uint32_t` Unix seconds, `0` = sender has no RTC). Decoded into a human date string here via `gmtime_r()`/`strftime()`, purely receiver-side; both the pretty dump and the `JSON:` line (`epoch_time` + `timestamp`) include it. See the dedicated rule above.
- **A move to SF12 was tried, confirmed to hang this device (no serial output at all, on multiple boards), and fully reverted** — see the ⚠️ rule above for the full incident.
- **SF10 is the second attempt** — `radio.begin()`'s spreading-factor argument is now `10`, matching the sender. Chosen specifically because it avoids the low-data-rate-optimization mechanism suspected (not confirmed) to have caused the SF12 hang. **Not yet confirmed working on real hardware** — test on one board with a Serial Monitor attached before flashing others. No other change needed on this side — still plain blocking interrupt-driven RX, no async-TX rework (that's sender-only, see its CLAUDE.md).
- LoRa RX only — no ESP-NOW, no display, no packet forwarding beyond USB serial
- Interrupt-driven receive via RadioLib (`setPacketReceivedAction` + `startReceive()`), not blocking/polled
- Prints full packet contents plus RSSI/SNR/packet count on every successful receive; CRC mismatches and other read errors are counted and logged, not silently dropped
- `telemetry_packet_t` is now 81 bytes (was 66, then 94, then 98) — ESC fields (`esc_mode`, `esc_state`, `esc_setpoint_pct`, `esc_live_pct`, `esc_ramp_pct`, `PKT_FLAG_ESC_VALID`) added to match `greenpower_sender`'s restored ESC UART link; both the pretty dump and the `JSON:` line include them
- Assumes a second Heltec ESP32-S3 LoRa WiFi V4 as the host board — **unconfirmed**, see Hardware section
- Speaks a small serial protocol to `../receiver_agent`: `DEVICE_ID` beacon/handshake (`GREENPOWER_RX_V1`) + a `JSON:` line per packet

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Hardware** section and **Current State** section.

"Significant change" means: `telemetry_packet_t` layout change, LoRa RF setting change, pin reassignment, or actual host-hardware confirmation/correction. Comment edits do not require a doc update.
