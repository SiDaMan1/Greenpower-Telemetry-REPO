# CLAUDE.md — Telemetry Web

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

The always-on public dashboard for Greenpower telemetry. A small Node/Express app meant to run on Railway 24/7, reachable at whatever domain is pointed at it (see the GoDaddy DNS notes below). It does **not** talk to the ESP32 receiver directly — [`receiver_agent`](../receiver_agent/CLAUDE.md) does that, and POSTs live packets here whenever it's actively forwarding. This site just holds the latest packet in memory and serves it to the browser dashboard, showing "offline" once data goes stale.

The three-part system: `greenpower_receiver` (ESP32, LoRa → USB serial) → `receiver_agent` (runs on whatever computer the receiver is plugged into, watches for it, asks the user before forwarding) → `telemetry_web` (this folder, always-on public site).

---

## Files in This Folder

| File | Role | May be edited? |
|------|------|----------------|
| `server.js` | Express app — `/api/telemetry` (POST, authenticated) and `/api/latest` (GET, public), serves `public/` | **Yes — primary target** |
| `public/index.html` | Dashboard — polls `/api/latest` every second, single self-contained file (no build step) | **Yes** |
| `package.json` | Dependencies (just `express`) | **Yes, if adding a real dependency** |
| `CLAUDE.md` | This file | **Yes — update after significant changes** |

---

## Rules and Constraints

### No persistence — this is a live-status dashboard, not a logger
`latest`/`lastUpdateMs` in `server.js` are plain in-memory variables. A Railway restart or redeploy clears them, and there's no history of past sessions anywhere. This is intentional for "is the car live right now" — if historical logging/analytics is ever wanted, that's a different feature (a real datastore), not a tweak to this file.

### `POST /api/telemetry` requires the API key; `GET /api/latest` does not
The write endpoint is authenticated (`Authorization: Bearer <TELEMETRY_API_KEY>`) so random internet traffic can't inject fake telemetry. The read endpoint is deliberately public with no auth — it's what the dashboard itself polls, and there's nothing sensitive in a speed/RPM/voltage reading. Don't add auth to `/api/latest`, and don't remove it from `/api/telemetry`.

### `TELEMETRY_API_KEY` must be set as a real Railway environment variable
Without it, `server.js` generates a random key at boot and only prints it to the server log — useless for `receiver_agent` running on someone's laptop, since it has no way to read Railway's log. Set `TELEMETRY_API_KEY` in Railway's dashboard (Variables tab) to a real fixed value, and put the same value in `receiver_agent`'s config. If the key is ever rotated, both sides need updating together.

### `STALE_MS` (10000) defines "offline"
If no POST arrives within this window, `/api/latest` reports `online: false` and the dashboard shows the offline banner, even though `data` still holds the last-known values. The frontend intentionally still displays those stale numbers (not blanked out) alongside the offline indicator — don't change that without a reason, it's useful to see the last known state.

### Dashboard is a single static file, no build step
`public/index.html` is plain HTML/CSS/JS, served directly by `express.static`. Don't introduce a bundler/framework for this unless the UI genuinely outgrows a single file — it deploys as-is with zero build configuration, which matters for how simply this deploys to Railway.

---

## Current State (V1)

- In-memory only, no database
- Single dashboard page, polling-based (1s interval), not WebSocket
- `TELEMETRY_API_KEY` — must be set in Railway's environment variables for production; falls back to a random per-boot key otherwise (dev-only)
- No historical logging — only ever shows the most recent packet

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Current State** section.

"Significant change" means: API contract change (`/api/telemetry` payload shape, auth scheme), new persistent storage added, or a new endpoint. Styling-only edits to `public/index.html` do not require a doc update.
