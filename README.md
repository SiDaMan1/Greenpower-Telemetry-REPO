# Greenpower Telemetry

Live telemetry system for the Greenpower race vehicle — sensors on the car talk over LoRa and ESP-NOW to a receiver, a small background agent on a laptop forwards that to the web, and a dashboard shows it live (plus history).

```
Vehicle (greenpower_sender) ──LoRa──▶ greenpower_receiver ──USB──▶ receiver_agent ──▶ telemetry_web (dashboard)
                             └─ESP-NOW──▶ steering_wheel_display
```

## Get the live dashboard forwarding data

If you just want telemetry from the receiver flowing to the dashboard, you don't need this whole repo — only the **receiver agent**. Grab it, unzip it, and run the setup script:

[![Download Receiver Agent](https://img.shields.io/badge/⬇️_Download-Receiver_Agent-22c55e?style=for-the-badge)](https://download-directory.github.io/?url=https://github.com/SiDaMan1/Greenpower-Telemetry-REPO/tree/main/receiver_agent)

1. Click the button above → downloads `receiver_agent.zip`
2. Unzip it anywhere
3. Double-click **`setup.bat`** inside the unzipped folder

That's it — it installs its own dependencies, config is already baked in, and it registers itself to start automatically at login. After this one-time setup, using it day to day is just: plug in the receiver, click **Yes** on the notification that pops up.

More detail: [`receiver_agent/README.md`](receiver_agent/README.md)

## Project map

| Folder | What it is |
|---|---|
| [`greenpower_sender`](greenpower_sender) | Firmware on the vehicle (ESP32-S3) — reads sensors, transmits over LoRa + ESP-NOW |
| [`greenpower_receiver`](greenpower_receiver) | Firmware on a base-station ESP32 — receives LoRa, relays JSON over USB serial |
| [`receiver_agent`](receiver_agent) | Node.js background agent (runs on a laptop) — watches for the receiver over USB, forwards data to the dashboard |
| [`telemetry_web`](telemetry_web) | The dashboard itself (Node/Express + Postgres), deployed on Railway |
| [`steering_wheel_display`](steering_wheel_display) | Firmware for the steering wheel's own live display (ESP-NOW receiver) |
| [`mock_sender`](mock_sender) | Fake sender for testing the display/dashboard without the real car |
| [`esc controller`](esc%20controller) | Firmware for the motor controller (ESP32), talks to the sender over UART |

Each folder has its own `CLAUDE.md` with the real technical detail — hardware wiring, pinouts, protocol formats, known gotchas. This README is just the map.
