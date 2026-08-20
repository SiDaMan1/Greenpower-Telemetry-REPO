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
| `public/index.html` | Dashboard — polls `/api/latest` 5x/sec, single self-contained file (no build step) | **Yes** |
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

### `STALE_MS` (2000) defines "offline"
If no POST arrives within this window, `/api/latest` reports `online: false` and the dashboard shows the offline banner, even though `data` still holds the last-known values. The frontend intentionally still displays those stale numbers (not blanked out) alongside the offline indicator — don't change that without a reason, it's useful to see the last known state.

### Dashboard is a single static file, no build step
`public/index.html` is plain HTML/CSS/JS, served directly by `express.static`. Don't introduce a bundler/framework for this unless the UI genuinely outgrows a single file — it deploys as-is with zero build configuration, which matters for how simply this deploys to Railway.

### Chart history lives entirely in the browser, not the server
Since `server.js` only ever holds the single latest packet (see "No persistence" above), every chart's rolling history (`history`/`historyIndex` in `index.html`, capped at `MAX_POINTS = 150`, ~30s at 5 Hz) is built client-side from repeated polls, not fetched from an API. This means **every open browser tab has its own independent history that starts empty on page load** — there's no shared/server-side timeline. If a "session replay" or multi-viewer-consistent history feature is ever wanted, that requires actual server-side storage, not a tweak to the frontend.

### History points are deduped by `seq`, not by poll tick
`poll()` only calls `pushHistory()`/`pushRawLine()` when `data.seq` differs from the last seen value. This matters because the dashboard polls at the same 5 Hz rate the sender transmits at — without the dedupe check, a single momentarily-slow poll response or a dropped LoRa packet would cause the same packet to be re-fetched and plotted as if it were a new sample, flattening the chart at that point instead of correctly showing a gap. Don't remove this check to "simplify" the polling loop.

### `METRICS` is the single source of truth for every telemetry field shown
Adding a new field to `telemetry_packet_t` that should appear on the dashboard means adding one entry to the `METRICS` object in `index.html` (label, unit, color, decimals, normalization range for the Multi tab, and whether it's on by default there) — the overview mini-charts, dedicated per-metric chart pages, stat cards, and Multi-tab toggle buttons are all generated from that one object, not hand-duplicated per field. Don't add a new metric's markup by copy-pasting an existing tab's HTML; add it to `METRICS` and (if it needs its own tab rather than just showing in Overview/Multi) add the tab markup referencing the same field key so the existing JS wiring picks it up automatically (`ov-<key>`, `big-<key>`, `chart-<key>`, `ovchart-<key>` id conventions).

### GPS/IMU validity comes from `flags`, not from checking for non-zero values
`gpsValid()`/`imuValid()` read bit 0 / bit 1 of the packet's `flags` byte (matching `PKT_FLAG_GPS_VALID`/`PKT_FLAG_IMU_VALID` in the firmware's `config.h`) rather than guessing validity from whether `latitude`/`roll_deg` etc. happen to be zero — a real reading can legitimately be zero (e.g. sitting still at a roll of exactly 0°), so a zero-check would misreport a valid reading as invalid. Only push a GPS point onto the raceline trail when `gpsValid()` is true.

### Colors are CSS custom properties, resolved at chart-creation time
Chart.js needs real color strings, not `var(--foo)` references — `resolveColor()` reads the computed CSS variable once when each chart is built. If the theme's CSS variables are ever changed dynamically (e.g. a light/dark toggle) rather than just at load time, charts built before the change won't pick up new colors without being recreated — this isn't wired up for live theme switching.

---

## Current State (V2 — adapted from an earlier standalone dashboard)

- In-memory only, no database, on the server side
- Full tabbed dashboard (Overview, Speed, Power, Temp, RPM, IMU, Raceline, Link, Multi, Raw) — adapted from an earlier project's dashboard design, rewired for this project's actual packet fields (`speed_mph`, `batt_volt`, `motor_volt`, `current_a`, `temp_f`, `motor_rpm`, `wheel_rpm`, `roll_deg`/`pitch_deg`/`yaw_deg`, `accel_g`/`lateral_g`/`vertical_g`, `rssi`/`snr`, GPS fields) — no ESC/throttle tab, since that hardware isn't part of this build (see `greenpower_sender/CLAUDE.md`)
- Single dashboard page, polling-based (200ms / 5 Hz interval, matching the sender's transmit rate), not WebSocket
- All chart history is client-side only (see above) — no server-side history endpoint exists
- `STALE_MS = 2000` — tuned for the 5 Hz cadence; that's already ~10 missed packets before flagging offline
- `TELEMETRY_API_KEY` — must be set in Railway's environment variables for production; falls back to a random per-boot key otherwise (dev-only)
- No historical logging server-side — only ever transmits/stores the most recent packet; the dashboard's own charts are the only "history," and only for as long as that browser tab stays open

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Current State** section.

"Significant change" means: API contract change (`/api/telemetry` payload shape, auth scheme), new persistent storage added, or a new endpoint. Styling-only edits to `public/index.html` do not require a doc update.
