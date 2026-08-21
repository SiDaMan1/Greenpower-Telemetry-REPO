# Greenpower Telemetry

A live telemetry system built for the Greenpower race vehicle. Sensors on the car — speed, battery/motor voltage, current, temperature, motor & wheel RPM, IMU orientation and G-forces, GPS, ESC state, LoRa link quality — stream off the car in real time, get picked up by a base-station receiver, and show up on a public web dashboard within a fraction of a second. Every packet is also saved, so past runs and test sessions can be reviewed and exported later, not just watched live.

```
Vehicle (greenpower_sender) ──LoRa──▶ greenpower_receiver ──USB──▶ receiver_agent ──▶ telemetry_web (dashboard)
                             └─ESP-NOW──▶ steering_wheel_display
```

- **`greenpower_sender`** — firmware on the car itself, reads every sensor and transmits over both LoRa (long-range, to the pits) and ESP-NOW (short-range, straight to the steering wheel).
- **`greenpower_receiver`** — a base-station ESP32 that catches the LoRa packets and relays them over USB.
- **`receiver_agent`** — a small background app on a laptop plugged into the receiver. It watches for the device, asks before forwarding (nothing goes live without a person clicking "Yes"), and pushes packets to the dashboard.
- **`telemetry_web`** — the dashboard itself: live numbers and charts, GPS raceline on a real map, session history with CSV export, installable as a mobile app.
- **`steering_wheel_display`** — the driver's own live readout, wired straight into the car's ESP-NOW link, no dependency on the receiver/agent/internet chain at all.

---

## 🏁 Live Dashboard

**[ascteracingmonitor.com →](https://ascteracingmonitor.com/)**

Open it anytime to see whatever the car is currently transmitting — speed, power, temperatures, RPM, ESC status, IMU data, live GPS raceline on satellite imagery, and LoRa link quality, all updating several times a second. When nothing is actively forwarding it just shows offline, not stale numbers pretending to be current.

A few things worth knowing if you're using it, not just building it:
- **Time range toggle** (top of the page) — switch the live charts between 1/5/15/30 minutes or the whole session, not just a fixed short window.
- **Sessions tab** — every run gets grouped automatically and stays browsable afterward, with a one-click CSV export of the full packet history.
- **Multi tab** — overlay several metrics normalized onto one chart, useful for spotting correlations (e.g. current spikes vs. RPM dips).
- **Install it as an app** — on a phone, use your browser's "Add to Home Screen"/"Install" option for a full-screen, no-browser-chrome experience with its own icon.

## 📥 Get telemetry flowing to the dashboard

If you just need the car's data reaching the dashboard, you don't need this whole repo — only the **receiver agent**, running on whatever laptop the base-station receiver is plugged into.

[![Download Receiver Agent](https://img.shields.io/badge/⬇️_Download-Receiver_Agent-22c55e?style=for-the-badge)](https://ascteracingmonitor.com/GreenpowerAgentSetup.exe)

1. Click the button above → downloads `GreenpowerAgentSetup.exe` (a real Windows installer, ~2 MB)
2. Run it and click through the setup wizard

No unzipping, no `.bat` files — it's a proper installer now: Start Menu shortcut, uninstaller in Add/Remove Programs, no admin rights needed (installs to your own user folder). It still installs its own dependencies, config is already baked in, and it registers itself to auto-start at login (plus a small tray icon so you can tell it's running). Requires [Node.js](https://nodejs.org) (LTS) already installed — the installer checks and tells you if it's missing. After setup, day-to-day use is just: plug in the receiver, click **Yes** on the notification that pops up asking permission to forward. Unplug/replug always re-asks — it never silently starts forwarding on its own.

The same download button also lives on the dashboard itself (top-right, desktop) for anyone who lands there first — it opens a short confirmation dialog first, since this is now a real installer someone's about to run, not an inert zip.

More detail, including what the consent prompt is actually protecting against: [`receiver_agent/README.md`](receiver_agent/README.md)

---

## Project map

| Folder | What it is |
|---|---|
| [`greenpower_sender`](greenpower_sender) | Firmware on the vehicle (ESP32-S3) — reads sensors, transmits over LoRa + ESP-NOW |
| [`greenpower_receiver`](greenpower_receiver) | Firmware on a base-station ESP32 — receives LoRa, relays JSON over USB serial |
| [`receiver_agent`](receiver_agent) | Node.js background agent (runs on a laptop) — watches for the receiver over USB, asks before forwarding, relays to the dashboard, tray icon |
| [`telemetry_web`](telemetry_web) | The dashboard itself (Node/Express + Postgres), deployed on Railway — live view, session history, mobile PWA |
| [`steering_wheel_display`](steering_wheel_display) | Firmware for the steering wheel's own live display (ESP-NOW receiver) |
| [`mock_sender`](mock_sender) | Fake sender for testing the display/dashboard without the real car |
| [`esc controller`](esc%20controller) | Firmware for the motor controller (ESP32 WROOM-32), talks to the sender over UART |

Each folder has its own `CLAUDE.md` with the real technical detail — hardware wiring, pinouts, protocol formats, known gotchas. This README is the map for a human reader; those files are the AI-assistant-facing equivalent, kept in sync with whatever's actually true in the code.

## Running your own copy

Every piece here is meant to work as a drop-in replacement for testing without the real car: `mock_sender` speaks the exact same LoRa/ESP-NOW packet formats as `greenpower_sender`, so a receiver or steering-wheel display can't tell the difference — and `telemetry_web` itself has a built-in **Test Mode** toggle (top of the dashboard) that feeds realistic sample data through the live charts with no hardware at all, for trying out the UI. See each folder's own README/CLAUDE.md for the specifics of setting that piece up.
