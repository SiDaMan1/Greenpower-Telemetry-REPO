# CLAUDE.md — Receiver Agent

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

A local background script (Node.js) that runs on whatever computer the [`greenpower_receiver`](../greenpower_receiver/CLAUDE.md) ESP32 gets plugged into over USB. It watches for new serial ports, positively identifies whether a newly connected device is actually the Greenpower receiver (not some other USB device), and — critically — **asks the user before forwarding anything**, via an OS notification. Only on acceptance does it start relaying parsed telemetry to [`telemetry_web`](../telemetry_web/CLAUDE.md)'s `/api/telemetry` endpoint.

The three-part system: `greenpower_receiver` (ESP32, LoRa → USB serial) → **this folder** (watches USB, prompts, forwards) → `telemetry_web` (always-on public site).

This is meant to run continuously in the background (e.g. started at login), not launched manually each time — see `README.md` for the Windows auto-start setup.

---

## Files in This Folder

| File | Role | May be edited? |
|------|------|----------------|
| `agent.js` | Entire agent — port scanning, device identification, prompt, forwarding | **Yes — primary target** |
| `config.example.json` | Template for local config — copy to `config.json` and fill in real values | **Yes, as a template** |
| `config.json` | Real config (gitignored — contains the API key) | **User-created, not checked in** |
| `package.json` | Dependencies (`serialport`, `node-notifier`) | **Yes, if adding a real dependency** |
| `CLAUDE.md` | This file | **Yes — update after significant changes** |

---

## Rules and Constraints

### Never forward without asking — this is the whole point of this agent existing
`promptToForward()` is not optional and not a "nice to have" — the user explicitly asked for consent-based forwarding rather than silent auto-start, specifically because a website that's always live but only shows real data when a human opted in on that specific machine was the desired behavior. Don't add a "remember my choice, don't ask again" auto-accept mode without the user asking for it — that would defeat the reason this agent exists rather than the website talking to the ESP32 directly.

### Device identification is handshake-based, not just "any serial port"
`identifyPort()` doesn't assume a new serial port is the receiver just because it appeared — it opens the port, sends `ID?\n`, and waits for the `DEVICE_ID` string (`GREENPOWER_RX_V1`) to show up, either from the firmware's boot beacon (if the agent was already watching when the device powered up) or the on-demand reply. Ports that don't answer within `IDENTIFY_TIMEOUT_MS` are marked `'not-ours'` and left alone. This must match `DEVICE_ID` in `../greenpower_receiver/greenpower_receiver.ino` exactly — if that firmware constant changes, update it here too.

### `knownPorts` forgets a port when it disappears, on purpose
So unplugging and replugging the same physical receiver re-triggers the full identify → prompt flow rather than silently doing nothing (or silently resuming forwarding without asking again). Don't "fix" this into a persistent allow-list unless the user specifically asks for that — it would also mean silent forwarding after the first plug-in, which contradicts the consent requirement above.

### `node-notifier`'s cross-platform behavior is inconsistent — Windows path is what's tested
The response string that means "user clicked the notification" varies by OS/backend; this code only treats the literal `'activate'` response as acceptance. If notifications aren't appearing or clicks aren't registering as acceptance on whatever OS this actually runs on, check what `node-notifier` actually returns there before assuming the accept/decline logic itself is broken — log the raw `response` value first.

### Fetch requires Node 18+
`forwardLine()` uses the global `fetch` built into Node 18+, no `node-fetch` dependency. If this ever needs to run on an older Node version, that's a real added dependency, not just a version bump.

---

## Current State (V1)

- Windows-focused (development/testing done on Windows); other platforms untested
- No persistent "remember this device" — every unplug/replug re-prompts
- No retry/queue on forward failure — a failed POST to `telemetry_web` is logged and dropped, not retried
- Not yet wired up to auto-start at login — see `README.md` for manual setup steps; this is a documented gap, not an oversight

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Current State** section.

"Significant change" means: `DEVICE_ID` changes, the JSON schema forwarded changes, the consent/prompt flow changes, or auto-start gets actually implemented. Comment-only edits do not require a doc update.
