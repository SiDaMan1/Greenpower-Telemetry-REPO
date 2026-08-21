# CLAUDE.md — Greenpower Telemetry (repo root)

**This file is a briefing for an AI assistant.** Read it before touching anything in this repo — then read the specific subfolder's own `CLAUDE.md` before actually editing code there. This file is the map; the subfolder files are the real technical detail (hardware wiring, pinouts, protocol formats, known gotchas, calibration constants).

---

## What This Repo Is

A live telemetry system for the Greenpower race vehicle. Sensors on the car transmit over LoRa (long-range, to a base-station receiver) and ESP-NOW (short-range, to the steering wheel's own display) simultaneously. A small background agent on a laptop watches for the receiver over USB and forwards live packets to a public web dashboard, which also persists history to a database.

```
Vehicle (greenpower_sender) ──LoRa──▶ greenpower_receiver ──USB──▶ receiver_agent ──▶ telemetry_web (dashboard)
                             └─ESP-NOW──▶ steering_wheel_display
```

`esc controller` is a separate ESP32 (the motor/throttle controller) that talks to `greenpower_sender` over UART — its data flows through the sender, not directly to the receiver.

`mock_sender` exists purely for testing `steering_wheel_display`/`telemetry_web` without the real car — it sends the same ESP-NOW/packet formats as `greenpower_sender` so either one is a drop-in replacement for the other from a receiver's point of view.

---

## Project Map

| Folder | What it is | Own CLAUDE.md |
|---|---|---|
| [`greenpower_sender`](greenpower_sender) | Firmware on the vehicle (ESP32-S3) — reads sensors, transmits over LoRa + ESP-NOW | [Yes](greenpower_sender/CLAUDE.md) |
| [`greenpower_receiver`](greenpower_receiver) | Firmware on a base-station ESP32 — receives LoRa, relays JSON over USB serial | [Yes](greenpower_receiver/CLAUDE.md) |
| [`receiver_agent`](receiver_agent) | Node.js background agent (runs on a laptop) — watches for the receiver over USB, asks before forwarding, relays to the dashboard, tray icon | [Yes](receiver_agent/CLAUDE.md) |
| [`telemetry_web`](telemetry_web) | The dashboard itself (Node/Express + Postgres), deployed on Railway — live view, session history, mobile PWA | [Yes](telemetry_web/CLAUDE.md) |
| [`steering_wheel_display`](steering_wheel_display) | Firmware for the steering wheel's own live display (ESP-NOW receiver) | [Yes](steering_wheel_display/CLAUDE.md) |
| [`mock_sender`](mock_sender) | Fake sender for testing the display/dashboard without the real car | [Yes](mock_sender/CLAUDE.md) |
| [`esc controller`](esc%20controller) | Firmware for the motor controller (ESP32 WROOM-32), talks to the sender over UART | [Yes](esc%20controller/CLAUDE.md) |

`README.md` (repo root) covers the same map for a human reader, plus the one-click receiver-agent download button — this file is the AI-facing equivalent, and the two should stay roughly in sync when the map itself changes (a new folder, a folder's role changing), though the README doesn't need every rule below.

**Also read [`SESSION_NOTES.md`](SESSION_NOTES.md)** (repo root) before starting work — it's not auto-loaded the way this file is, so it's easy to miss. It summarizes the most recent session's narrative (what was discussed/decided/why) in a way the per-folder `CLAUDE.md` files deliberately don't — those hold durable facts about the code, not conversation history. It's overwritten each session, not appended to.

---

## Rules and Constraints

### Pick the narrowest CLAUDE.md that actually covers the file you're editing
Don't try to hold every subfolder's hardware/protocol details in your head from this file alone — it's deliberately a map, not a merged briefing. Before editing anything inside `telemetry_web/`, read `telemetry_web/CLAUDE.md`; before touching sender firmware, read `greenpower_sender/CLAUDE.md`; etc. Claude Code reads CLAUDE.md files up the directory tree automatically, so a session started inside a subfolder already has that folder's context — this file mainly matters for a session started at the repo root, or for understanding how the pieces fit together before diving into one of them.

### `config.h`/packet-format changes are cross-folder and easy to half-do
`telemetry_packet_t` (defined identically in both `greenpower_sender/config.h` and `greenpower_receiver/config.h` — two independently-maintained copies, not a shared include) and the ESP-NOW CSV format (produced by `greenpower_sender`/`mock_sender`, consumed by `steering_wheel_display`) are the two places a field-list change has to land in more than one folder in the same commit, or one side silently breaks against the other. Each side's own CLAUDE.md flags this locally too — it's called out here because it's the one kind of change that isn't obviously "cross-folder" just from looking at whichever single file you started editing.

### This repo is public — treat anything committed as public
`receiver_agent/config.json` (real API key + dashboard URL) is intentionally committed despite that, at the repo owner's explicit request after being told the repo's visibility — see git history around that commit for the reasoning. Don't assume the same is fine for a *different* secret without asking; that was a specific, informed exception, not a general policy that "secrets are fine here now."

### Every subfolder's CLAUDE.md ends with its own "Self-Maintenance Requirement"
Follow it — update the relevant subfolder's Current State section after a significant change there (each file defines what counts as "significant" for that folder). This file's own map (the table above) only needs updating when a folder is added/removed or its one-line role actually changes, not for changes inside an existing folder.

---

## Self-Maintenance Requirement

Update the **Project Map** table above when a folder is added, removed, or its role changes. Everything else (hardware detail, protocol formats, current state per component) lives in the subfolder's own CLAUDE.md — don't duplicate it here, it'll drift.
