# CLAUDE.md — Telemetry Web

**This file is a briefing for an AI assistant.** Read it before touching any code in this folder.

---

## What This Project Is

The always-on public dashboard for Greenpower telemetry. A small Node/Express app meant to run on Railway 24/7, reachable at whatever domain is pointed at it (see the GoDaddy DNS notes below). It does **not** talk to the ESP32 receiver directly — [`receiver_agent`](../receiver_agent/CLAUDE.md) does that, and POSTs live packets here whenever it's actively forwarding. Live data still flows through an in-memory value the same as before; as of this pass, every packet is **also** persisted to Postgres and grouped into automatically-bounded "sessions" so past drives/tests can be browsed and exported later, not just watched live.

The three-part system: `greenpower_receiver` (ESP32, LoRa → USB serial) → `receiver_agent` (runs on whatever computer the receiver is plugged into, watches for it, asks the user before forwarding) → `telemetry_web` (this folder, always-on public site).

---

## Files in This Folder

| File | Role | May be edited? |
|------|------|----------------|
| `server.js` | Express app — `/api/telemetry` (POST, authenticated), `/api/latest` (GET, public), session endpoints (GET, public), serves `public/` | **Yes — primary target** |
| `public/index.html` | Dashboard — polls `/api/latest` 5x/sec for live data, fetches session data on demand (not polled), single self-contained file (no build step) | **Yes** |
| `package.json` | Dependencies (`express`, `pg`) | **Yes, if adding a real dependency** |
| `CLAUDE.md` | This file | **Yes — update after significant changes** |

---

## Rules and Constraints

### Live status is still in-memory only — persistence is a separate, parallel path
`latest`/`lastUpdateMs` in `server.js` are still plain in-memory variables, cleared on every restart/redeploy, and that's still correct for "is the car live right now." Don't try to derive `/api/latest` from the database — it's deliberately a separate, cheap, always-current path that doesn't depend on Postgres being configured or reachable.

### Database is optional — `pool` being `null` must never crash a request
`server.js` only creates the `pg` `Pool` if `DATABASE_URL` is set (Railway injects this automatically once a Postgres plugin is attached — see README.md's setup section). Every DB-touching route (`recordPoint()`, `/api/sessions`, `/api/sessions/:id/points`, `/api/sessions/:id/export.csv`) checks `if (!pool)` first and degrades gracefully (silent skip for recording, `503` for the read endpoints) rather than throwing. Keep that guard on any new DB-touching code — local dev without a configured Postgres, and any deploy where the plugin hasn't been added yet, both depend on this not crashing.

### Telemetry is stored as JSONB, not one column per field — this is deliberate
`telemetry_points.data` is a single `JSONB` column holding the whole packet, not `speed_mph FLOAT, batt_volt FLOAT, ...`. `telemetry_packet_t` has changed shape twice already in this project's history (ESC fields added, RPM fields reworked) — a fixed relational schema would need a migration every time that happens again. JSONB means the DB layer doesn't care what fields exist. The one place a fixed field list *does* exist is `CSV_COLUMNS` in `server.js`, and that's intentional — see below.

### `CSV_COLUMNS` is a fixed, hand-maintained list — keep it in sync with the packet shape
CSV export doesn't derive its columns from whatever keys happen to be in any one row (which would make column order unstable, or silently drop columns missing from row 1). It uses the explicit `CSV_COLUMNS` array in `server.js`. If a field is added to `telemetry_packet_t` (and to `METRICS` in `index.html` per the rule below), add it to `CSV_COLUMNS` too, in the same change — otherwise it'll flow into the database fine but silently never appear in an exported CSV.

### Sessions are bounded by a time gap, not an explicit start/stop signal
`SESSION_GAP_MS` (60000) in `server.js`: a new session starts whenever a packet arrives and either no session is active yet, or the gap since the last recorded packet exceeds this. Nothing in the firmware, `receiver_agent`, or the dashboard explicitly signals "session started" or "session ended" — this is purely inferred from the data flow itself. A receiver briefly losing LoRa sync for under a minute stays in the same session; walking away for an hour and coming back starts a new one. If a different session-boundary policy is ever wanted (explicit start/stop button, vehicle-ignition signal, etc.), that's a real design change to discuss, not a constant to tweak blindly.

### Session views are one-shot renders, not live — don't wire them into the live poll loop
`viewSession()` in `index.html` fetches a session's full point history once and builds Chart.js charts from the complete static array. This is architecturally different from the live Overview/dedicated-tab charts, which grow incrementally via `pushHistory()` on every live poll tick. Don't merge these two code paths — a past session's data doesn't change, and re-polling it repeatedly would be pure waste.

### `POST /api/telemetry` requires the API key; `GET /api/latest` does not
The write endpoint is authenticated (`Authorization: Bearer <TELEMETRY_API_KEY>`) so random internet traffic can't inject fake telemetry. The read endpoint is deliberately public with no auth — it's what the dashboard itself polls, and there's nothing sensitive in a speed/RPM/voltage reading. Don't add auth to `/api/latest`, and don't remove it from `/api/telemetry`.

### `TELEMETRY_API_KEY` must be set as a real Railway environment variable
Without it, `server.js` generates a random key at boot and only prints it to the server log — useless for `receiver_agent` running on someone's laptop, since it has no way to read Railway's log. Set `TELEMETRY_API_KEY` in Railway's dashboard (Variables tab) to a real fixed value, and put the same value in `receiver_agent`'s config. If the key is ever rotated, both sides need updating together.

### `STALE_MS` (2000) defines "offline"
If no POST arrives within this window, `/api/latest` reports `online: false` and the dashboard shows the offline banner, even though `data` still holds the last-known values. The frontend intentionally still displays those stale numbers (not blanked out) alongside the offline indicator — don't change that without a reason, it's useful to see the last known state.

### Dashboard is a single static file, no build step
`public/index.html` is plain HTML/CSS/JS, served directly by `express.static`. Don't introduce a bundler/framework for this unless the UI genuinely outgrows a single file — it deploys as-is with zero build configuration, which matters for how simply this deploys to Railway.

### Live chart history (Overview/dedicated tabs/Multi) is still browser-only and short — that's separate from Sessions
The rolling `history`/`historyIndex` arrays (`MAX_POINTS = 150`, ~30s at 5 Hz) that feed the *live* charts are still client-side-only and still reset on page load — this didn't change. What changed is that there's now a **separate, independent, server-side** long-term record (Postgres) reachable through the Sessions tab, not by extending `MAX_POINTS` or feeding the live charts from the database. Don't conflate the two: the live charts are deliberately cheap and ephemeral; Sessions is deliberately durable and fetched on demand.

### History points are deduped by `seq`, not by poll tick
`poll()` only calls `pushHistory()`/`pushRawLine()` when `data.seq` differs from the last seen value. This matters because the dashboard polls at the same 5 Hz rate the sender transmits at — without the dedupe check, a single momentarily-slow poll response or a dropped LoRa packet would cause the same packet to be re-fetched and plotted as if it were a new sample, flattening the chart at that point instead of correctly showing a gap. Don't remove this check to "simplify" the polling loop.

### `METRICS` is the single source of truth for every telemetry field shown
Adding a new field to `telemetry_packet_t` that should appear on the dashboard means adding one entry to the `METRICS` object in `index.html` (label, unit, color, decimals, normalization range for the Multi tab, and whether it's on by default there) — the overview mini-charts, dedicated per-metric chart pages, stat cards, and Multi-tab toggle buttons are all generated from that one object, not hand-duplicated per field. Don't add a new metric's markup by copy-pasting an existing tab's HTML; add it to `METRICS` and (if it needs its own tab rather than just showing in Overview/Multi) add the tab markup referencing the same field key so the existing JS wiring picks it up automatically (`ov-<key>`, `big-<key>`, `chart-<key>`, `ovchart-<key>` id conventions). **Also add it to `CSV_COLUMNS` in `server.js`** (see the Sessions rules below) — `METRICS` and `CSV_COLUMNS` are two independent lists that both need updating for a new field to be fully wired end-to-end (dashboard display AND CSV export); a field only in one of them will display but not export, or export but not display.

### GPS/IMU/ESC validity comes from `flags`, not from checking for non-zero values
`gpsValid()`/`imuValid()`/`escValid()` read bit 0 / bit 1 / bit 3 of the packet's `flags` byte (matching `PKT_FLAG_GPS_VALID`/`PKT_FLAG_IMU_VALID`/`PKT_FLAG_ESC_VALID` in the firmware's `config.h` — note bit 2 / `0x04` is the current-sensor flag, not ESC, easy to mix up) rather than guessing validity from whether `latitude`/`roll_deg`/`esc_setpoint_pct` etc. happen to be zero — a real reading can legitimately be zero (e.g. sitting still at a roll of exactly 0°, or the ESC's setpoint genuinely at 0%), so a zero-check would misreport a valid reading as invalid. Only push a GPS point onto the raceline trail when `gpsValid()` is true.

### ESC mode/state are strings, not `METRICS` entries
`esc_mode`/`esc_state` are handled by hand in `applyState()` (the `esc-mode`/`esc-state` elements), not added to the `METRICS` registry — `METRICS` assumes every field is a chartable number with decimals and a normalization range, which doesn't apply to a string like `"NORMAL"` or `"RAMP"`. The three numeric ESC fields (`esc_setpoint_pct`/`esc_live_pct`/`esc_ramp_pct`) *are* in `METRICS` and get the full auto-wired treatment (chart, history, Multi-tab toggle) for free. If a future string-valued field needs display, follow the mode/state pattern, not the `METRICS` pattern.

### Colors are CSS custom properties, resolved at chart-creation time
Chart.js needs real color strings, not `var(--foo)` references — `resolveColor()` reads the computed CSS variable once when each chart is built. If the theme's CSS variables are ever changed dynamically (e.g. a light/dark toggle) rather than just at load time, charts built before the change won't pick up new colors without being recreated — this isn't wired up for live theme switching.

---

## Current State (V3 — persistent sessions added)

- Live path unchanged: in-memory `latest`/`lastUpdateMs`, `STALE_MS = 2000` (tuned for the 5 Hz cadence — ~10 missed packets before flagging offline)
- **Persistent path added**: optional Postgres (`DATABASE_URL`, a Railway plugin — not configured by default, degrades gracefully without it) storing every packet as JSONB, auto-grouped into sessions on a 60s gap (`SESSION_GAP_MS`)
- New endpoints: `GET /api/sessions`, `GET /api/sessions/:id/points`, `GET /api/sessions/:id/export.csv` — all public reads, same reasoning as `/api/latest` (nothing sensitive in a telemetry reading)
- New **Sessions tab**: lists past sessions with duration/packet count, "VIEW" builds a one-shot set of charts from that session's full history (not live-polled), "CSV" downloads the full session via a direct link
- `CSV_COLUMNS` in `server.js` is a fixed, hand-maintained export column list — must be kept in sync with `METRICS` in `index.html` whenever a packet field is added (see rule above)
- Full tabbed dashboard (Overview, Speed, Power, Temp, RPM, ESC, IMU, Raceline, Link, Multi, Sessions, Raw) — adapted from an earlier project's dashboard design, rewired for this project's actual packet fields
- Live chart history (Overview/dedicated tabs/Multi) is still client-side-only, capped at `MAX_POINTS = 150` (~30s at 5 Hz), separate from the new persistent Sessions path — see rules above
- `TELEMETRY_API_KEY` — must be set in Railway's environment variables for production; falls back to a random per-boot key otherwise (dev-only)

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Current State** section.

"Significant change" means: API contract change (`/api/telemetry` payload shape, auth scheme), new persistent storage added, or a new endpoint. Styling-only edits to `public/index.html` do not require a doc update.
