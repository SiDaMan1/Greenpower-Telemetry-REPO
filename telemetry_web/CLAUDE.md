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
| `public/index.html` | Dashboard — polls `/api/latest` 5x/sec for live data, fetches session data on demand (not polled), single self-contained file (no build step). Same file serves both desktop and mobile layouts via CSS media queries + a mobile-only sidebar, not a separate page — see PWA/mobile rules below | **Yes** |
| `public/manifest.json` | PWA manifest — name, icons, `display:standalone`, theme colors | **Yes** |
| `public/sw.js` | Minimal service worker — installability only, no caching (see rule below) | **Yes, but keep it caching-free unless offline support is a deliberate feature decision** |
| `public/icon.svg`, `public/icon-maskable.svg` | App icons referenced by `manifest.json` — `icon-maskable.svg` has the bolt shrunk into the safe zone with a full-bleed background | **Yes** |
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
The rolling `history`/`historyTimes` arrays (`MAX_POINTS = 150`, ~30s at 5 Hz) that feed the *live* charts are still client-side-only and still reset on page load — this didn't change. What changed is that there's now a **separate, independent, server-side** long-term record (Postgres) reachable through the Sessions tab, not by extending `MAX_POINTS` or feeding the live charts from the database. Don't conflate the two: the live charts are deliberately cheap and ephemeral; Sessions is deliberately durable and fetched on demand.

### Every chart's x-axis is real wall-clock time — server-stamped, not client-guessed
This was a real bug, fixed in this pass: the live charts used to plot against `historyIndex`, a plain incrementing integer (`pointCount++`) with the x-axis hidden entirely on the mini-charts — not a timestamp at all, on any of them. Two things now make it real time:
1. `server.js` stamps `received_at` (its own `Date.now()`, server receive time) onto the `latest` object in the `POST /api/telemetry` handler — this is intentionally NOT the same value passed to `recordPoint()`/stored in the DB, which gets its own `received_at` from Postgres's `now()` at insert time. Two independent timestamps, two independent purposes (live display vs. durable record) — don't try to unify them into one.
2. The frontend's `historyTimes` array stores that real `d.received_at` (epoch ms), and `applyHistory()` formats it to a locale time string once per update for every chart's labels — mini-charts included, via `BASE_OPTS.scales.x` now defaulting to `display:true` instead of `false`.

If a chart's x-axis ever looks like it's showing sequence numbers or is missing again, check that the data source is actually populating `received_at` — don't quietly reintroduce a counter as a stand-in.

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

### Mobile is a layout mode of the same page, not a separate build
Below `860px` wide, a `@media` block hides the desktop `#tabs` bar and shows `.mobile-sidebar` instead (fixed-position icon rail, `56px` collapsed / `210px` expanded via a `.expanded` class toggle on click), and hides `#overview-right-wrap` (the Overview graph grid) — mobile Overview shows only the stats-bar numbers, never the charts, per an explicit design choice to keep the mobile Overview lightweight. All 12 tabs (`overview`/`speed`/`power`/`temperature`/`rpm`/`esc`/`imu`/`raceline`/`link`/`multi`/`sessions`/`raw`) are duplicated as `.mobile-tab` elements with the same `data-page` values as the desktop `.tab` elements, and the shared click handler keeps both sets' `.active` class in sync — don't add a new tab to only one of the two tab lists, or that page becomes unreachable from whichever UI mode was skipped. Selecting a mobile tab also calls `collapseMobileSidebar()` so the rail snaps back to icon-only after navigating.

### No CSS `transition` on the mobile sidebar's width/label — same reason as the stats-bar collapse
`.mobile-sidebar.expanded` and `.mobile-tab-label` change `width`/`display` instantly, with **no** `transition` property. This mirrors the earlier, deliberate fix to the Overview stats-bar collapse (`.stats-row{display:none}` instead of an animated `max-height`/`grid-template-rows`): CSS-transitioned state changes on this page have repeatedly failed to reliably reach their declared end-state, while instant class-driven toggles have not. If a future edit wants to animate the sidebar, verify the end-state actually applies (computed width, not just class presence) before trusting it — don't re-add a bare `transition:` and assume it works.

### PWA installability — manifest + service worker, deliberately no offline caching
`public/manifest.json`, `public/sw.js`, and the icon SVGs exist solely to satisfy the browser's install criteria (Chrome/Android requires a registered service worker with a `fetch` listener before firing `beforeinstallprompt`). `sw.js` intentionally never calls `event.respondWith()` — this is a live-telemetry dashboard, and caching API responses or the page shell risks silently serving stale readings, which would undermine the entire point of the tool. Install is offered via a small floating button (`#install-fab`), not a banner — see the icon/install rule below. If real offline behavior is ever wanted, that's a deliberate feature to design (e.g. an explicit "stale/offline" state), not something to fall into by adding a cache to `sw.js`.

### Install prompt is a floating icon button, not a banner
`#install-fab` (bottom-right, circular, hidden unless `.show`) replaced an earlier full-width banner — a banner permanently claims header space most visits don't need; the FAB costs nothing until tapped, then opens `#install-popover` (small card, `.show` to display) with the actual "Install" action (Chrome/Android, via the captured `beforeinstallprompt`) or manual instructions (iOS, no such API exists there). Dismissal still persists via the `gp_install_dismissed` localStorage key. **Caught once in testing, worth remembering**: `.install-fab`'s own base rule must never set `display:` itself — the page-wide default-hidden rule (`.mobile-sidebar, .mobile-sidebar-backdrop, .install-fab, .install-popover { display:none; }`) and `.install-fab`'s own base rule are equal-specificity single-class selectors, so whichever is declared later in the stylesheet wins the cascade regardless of the `.show` class; `display:flex` must live only on the `.install-fab.show` rule.

### Icons are inline SVG from one JS map, not emoji
Every static UI icon (nav tabs, header logo, sidebar toggle, hide/customize/refresh/export/close/clear/install buttons) is an empty `<span data-icon="name">` in the HTML, filled in once at load by `applyIcons()` from the `ICONS` map near the top of `<script>` — add a new icon by adding one entry to `ICONS` and one `data-icon="..."` span, not by pasting SVG markup inline. Deliberately not emoji: emoji render inconsistently across platforms/fonts/OSes and read as informal; one hand-authored Feather-style icon set (stroke=`currentColor`, no fill) reads as a coherent design system and automatically inherits whatever text color its container already has. Dynamic runtime status glyphs (e.g. the GPS fix ✅/❌ strings written by `applyState()`) were deliberately left as-is — converting those would mean restructuring the DOM nodes they get written into, and they're data indicators rather than navigation/action iconography.

### Chart axes are pre-filled and range-seeded so a fresh chart doesn't visibly lurch on its first real data points
`history[k]` starts as `MAX_POINTS` nulls (not an empty, growing array) and `historyTimes` is seeded with `MAX_POINTS` backdated placeholder timestamps spaced `SENSOR_INTERVAL_MS_GUESS` (200ms) apart, both filled in before the first `applyHistory()` call (which now also runs once immediately on load, not only after the first live packet). `safeChart()` additionally seeds `scales.y.suggestedMin/suggestedMax` from each metric's `METRICS[k].range` when a `metricKey` is passed. Together this means every chart already shows its full time window and a sane y-range from the very first paint — without it, a brand-new chart auto-fits tightly to whatever 1-2 real points exist so far, and every subsequent point yanks the axes into a new shape until `MAX_POINTS` is finally reached (this was the "graphs expand rapidly when first receiving data" bug). If a new metric/chart is added, pass its `metricKey` through to `safeChart()` — omitting it just means that one chart's y-axis won't get the stabilizing hint, not a hard failure.

### Tooltips work on both hover (desktop) and tap (mobile) — this requires `intersect:false`
Every chart's line has `elements.point.radius:0` (no visible dots — a deliberate look), which means Chart.js's *default* tooltip trigger (`intersect:true`, i.e. "the pointer must land exactly on a rendered point") can never fire — there's no point to land on. `BASE_OPTS` sets `interaction:{mode:'index', intersect:false}` (and each per-chart `options` object needs the same, done in the multi-chart and session-chart config too, since they don't inherit from `BASE_OPTS` directly) so the tooltip instead fires from proximity to an x-position — which is what makes it work as a touch/tap gesture on mobile as well as a mouse hover on desktop, since Chart.js's default `events` list already includes `touchstart`/`touchmove` needing no separate mobile-specific code path. `TOOLTIP_OPTS` is a shared object so all chart types render tooltips with the same dark, theme-matched styling.

### The mobile sidebar uses a real, animated CSS `transition` on `width` — and how to verify that, since the obvious way is broken here
Earlier work concluded CSS transitions in this project "never reach their end state" and switched several UI pieces (the stats-bar collapse, once the mobile sidebar itself) to instant `display`/class-driven toggles instead. Re-investigated during the icon/animation pass: the real cause is specific to the automated browser-preview tool used to verify this page in this dev environment — its browser pane only composites frames (runs `requestAnimationFrame`, applies CSS transitions) while actually displayed/focused; when it isn't, `computer{action:"screenshot"}` fails outright with "the Browser pane is not displayed, so the page is not compositing frames", and under that same condition **any** CSS transition — even a trivial transform on a freshly-created throwaway element — silently never reaches its end state no matter how long you `await`, while forcing `el.style.transition = 'none'` on the same class toggle applies the end state instantly and correctly. That's a property of the verification tool, not of real browsers or of CSS transitions — a normal, visible/focused browser tab always composites and transitions animate normally for real users. **When verifying a CSS transition/animation change in this environment**: don't trust `getBoundingClientRect()`/`getComputedStyle()` read immediately or after an `await setTimeout` — instead temporarily set `el.style.transition = 'none'` before toggling the class, confirm the computed end-state is structurally correct, then remove the inline override; that isolates "is the class/selector/rule correct" (which the tool *can* verify) from "does it animate" (which only a real, visibly-displayed browser can show). Don't strip a `transition` from this file's CSS again based on a synchronous/async-timeout check failing to show the end state — check it this way first.

### Test Mode is a client-side-only fake-data generator, never touches the network
The `#test-mode-btn` toggle in the header runs `genTestPacket()`/`testModeTick()` (near the bottom of `<script>`), which pushes synthetic packets through the *exact same* `applyState()`/`pushHistory()`/`applyHistory()`/`pushRawLine()` functions a real `/api/latest` response would — this is deliberate, so testing the UI can't silently drift from how real data actually renders. It never calls `fetch`/`POST` anywhere, so it can't pollute a real session or write to the database. `poll()`'s own `setInterval` checks `testModeActive` and skips itself while test mode is on, handing back to the real feed the instant it's turned off (an immediate `poll()` call, not waiting for the next 200ms tick).

### `MAX_POINTS` is viewport-dependent, decided once at load — not the same on mobile and desktop
Checked once via `matchMedia('(max-width:860px)')` when the script first runs (60 on mobile vs. 150 on desktop, both still ~the same *time* window at the 5Hz cadence — just fewer points per pixel on a narrower chart). Not re-evaluated on resize/rotation — the `history`/`historyTimes` arrays are pre-sized to this value once (`initHistory`-equivalent code near `STATE / HISTORY`), and there's no live-resize path for them. If a metric/chart needs a different point-density rule later, this is the one place to change it.

### Live charts DO now animate on update — `chart.update()`, not `chart.update('none')`
This reverses an earlier explicit choice ("animation stays OFF for live data updates... looks laggy, not smooth"). `LIVE_ANIMATION` (`{duration:250, easing:'easeOutQuad'}`) is short enough to resolve before the next 200ms tick lands, which is what makes it read as a smooth "line growing" motion instead of the earlier hard per-tick cut, or (if the duration were left longer) visible tween-stacking lag. If update cadence or `LIVE_ANIMATION.duration` ever change together, keep duration comfortably under the tick interval.

### Haptics on chart scrub are attached post-construction, not baked into `BASE_OPTS`
`attachHaptics(opts)` sets `opts.onHover` after `safeChart()`/the multi-chart build their options via `JSON.parse(JSON.stringify(BASE_OPTS))` — functions don't survive a JSON round-trip, so putting `onHover` directly on `BASE_OPTS` would silently vanish on every chart built that way. `navigator.vibrate` is Android-only (silently absent, not an error, on iOS/desktop) and fires once per newly-crossed data-point index while scrubbing, not per pointer-move frame.

### Nothing is selectable/copyable except actual telemetry values
`body { user-select:none }` by default, with `user-select:text` re-enabled only on `.stat-value`/`.dedicated-value`/`.stat-unit`/`.dedicated-unit`/`#raw-log`/`#sessions-table td` (and `#esc-mode`/`#esc-state`, which are also `.dedicated-value`). Adding a new numeric/string readout element outside those classes means it won't be copyable unless it's given one of them (or added explicitly) — this was deliberate, not an oversight, if a new display element ever seems to need long-press-to-copy and doesn't have it.

### Mobile dedicated-value pages: even card count → all landscape-rectangle tiles (2/row), odd → last one is a full-width rectangle
Pure CSS, no JS/count-tracking: `.big-row .dedicated-big-card:nth-child(odd):last-child` only matches when the last card's position is itself odd, which is only possible when the row's total count is odd. Pages with a single card (Speed, Temperature) don't use `.big-row` at all and are untouched by this — already a full-width rectangle by default, same as before. Tiles are `aspect-ratio:8/5` (a slight landscape rectangle), not `1/1` — a first pass used true squares, but that took more vertical scroll height than needed; 8/5 keeps the 2-per-row grid while shrinking each tile's height.

### `chart.resize()` silently no-ops while an animation is in flight — always call `forceChartResize()`, never `chart.resize()` directly
This was the *actual* root cause of "Test Mode graphs don't show up until you turn Test Mode off" (a follow-up animation change made it much worse, but the underlying trap already existed for real live data too). Chart.js's `resize(w,h)` checks its internal Animator's `running(chart)` state: if true, it just stashes the new size to apply on that animation's next draw instead of resizing synchronously. Once live charts started animating on every `update()` (see `LIVE_ANIMATION` above) with updates arriving every ~200ms, a chart fed a steady stream of updates can have "an animation running" almost continuously — so a plain `chart.resize()` call (the tab-switch fix, the stats-bar-collapse fix) gets silently deferred, and nothing ever flushes that deferred resize while updates keep re-arming a fresh animation before the old one finishes. Confirmed directly: a chart's `canvas.width`/`canvas.height` stayed frozen at 0 (its size when created inside a `display:none` page) even after its tab became active and even after manually calling `chart.resize(w,h)` with explicit correct dimensions — while continuous updates kept flowing; calling `chart.stop()` immediately before `resize()` (cancels the in-flight animation, which flips the Animator check back to false) fixed it instantly. `forceChartResize(chart)` wraps exactly that `stop()`-then-`resize()` pair — both places that resize a chart in response to layout changes (`setStatsBarCollapsed()`, the tab-click handler) call it instead of `chart.resize()` directly; if a future resize call site is added, use `forceChartResize()` there too, not the raw method. `LIVE_ANIMATION.duration` (150ms) is also kept safely under the ~200ms update interval as defense-in-depth, so animations reliably finish between ticks on their own even without an explicit `stop()`.

### Only the currently-visible chart animates — `safeUpdateChart()` checks `canvas.offsetParent`
`applyHistory()` updates every chart (all 8 overview minis + all 16 dedicated bigs + Multi) on every tick regardless of which tab is active — that part is unchanged and cheap for a plain data-swap. But animating all ~25 of them at once (see `LIVE_ANIMATION` above) turned out to be enough main-thread/GPU work on a phone to starve paint entirely: the chart actually on screen appeared frozen/blank the whole time, only catching up once updates stopped. Confirmed worst under Test Mode specifically, because its guaranteed-every-200ms cadence (no packet loss to thin it out, unlike real LoRa data) maxes out the update rate. Fix: `chart.canvas.offsetParent === null` for any chart on a hidden tab, so only the visible chart(s) get the animated `update()`; everything else still gets fresh data via `update('none')`. If a chart ever needs to animate while "hidden" (e.g. a future preview thumbnail), this check will wrongly skip it — not a real scenario today.

### `color-scheme: dark` (meta tag + CSS) — separate from `theme-color`, needed for OS-drawn chrome (status/nav bar)
`theme-color` only tints the in-browser tab/address-bar strip. The OS-level chrome around an *installed* PWA (Android's status bar AND bottom gesture/nav bar) follows the page's declared `color-scheme` in current Chrome, not `theme-color` — without it, that chrome can default to the system light/dark setting regardless of how dark the page itself is. Declared both as `<meta name="color-scheme" content="dark">` and `:root{color-scheme:dark}` (belt-and-suspenders — different engines/contexts read one or the other). Still won't retroactively fix an *already-installed* copy of the app without an uninstall+reinstall (manifest/meta values are cached at install time) — see SESSION_NOTES.md.

### X-axis tick count is mobile-aware too (`IS_MOBILE`, shared with `MAX_POINTS`)
Chart labels are hour:minute only (`fmtTimeNoSeconds`, no seconds) — fine on desktop's ~30s window, but mobile's ~12s window (`MAX_POINTS=60`) is short enough that most/all ticks land in the same minute and print the identical label repeated. `maxTicksLimit` drops to 2 on mobile (was 4/8) everywhere it's set (mini charts, dedicated "big" charts, Multi) — all three sites read the same `IS_MOBILE` constant, keep them in sync if this changes again.

---

## Current State (V3.3 — mobile UI polish round 2: test mode, haptics, animated/smoother charts, symmetric nav, select-none)

- **App name on install** is now "GP Telemetry" (`manifest.json`'s `short_name`, and the `apple-mobile-web-app-title` meta tag) — was "Greenpower"/"Greenpower Telemetry". An *already-installed* PWA on a device won't pick this up (or the theme-color/status-bar-color fixes noted in SESSION_NOTES.md) until it's uninstalled and reinstalled — this is a platform manifest-caching behavior, not something fixable from this repo's code.
- **Test Mode** (`#test-mode-btn` in the header) — see rule above.
- **Mobile bottom nav bar squares are now symmetric**: `.mobile-navbar` itself is the single flex row (`justify-content:space-between`) with `.mobile-navbar-scroll` reduced to `display:contents` — previously the 5 tabs were evenly spaced only *within their own sub-box*, while the divider+expand button sat outside it with mismatched edge margins, reading as off-center.
- **IMU page's two stacked `.chart-row` sections no longer overlap on mobile** — `.big-row`/`.chart-row` are `flex:none` (was `flex:1`) at the mobile breakpoint, so each section's box sizes to its actual grid content instead of being compressed to a flex-share that overflowed into the next section.
- **Live charts animate on update, use fewer points on mobile, and render smoother lines** — see the three rules above (`LIVE_ANIMATION`, mobile `MAX_POINTS`, `borderJoinStyle`/`borderCapStyle:'round'`).
- **Chart scrubbing triggers light haptic feedback on Android** — see `attachHaptics()` rule above.
- **Multi tab's legend markers are short colored lines, not boxes** (`usePointStyle:true, pointStyle:'line'`) — less horizontal space per entry across up to 18 toggleable metrics.
- **Only telemetry values are selectable/copyable** — see rule above.
- **Mobile dedicated-value pages (Power/RPM/ESC/IMU/Link) lay out as 2-per-row landscape-rectangle tiles, with an odd one out spanning full-width** — see rule above.
- **`color-scheme:dark` declared** (meta + CSS) so Android's installed-PWA status/nav bar chrome follows the page's dark theme — see rule above; still needs an uninstall+reinstall to take effect on an already-installed copy.
- **Only the visible chart animates on update** — see `safeUpdateChart()`/`offsetParent` rule above; fixes a real bug where all charts animating at once (worst under Test Mode) starved paint on mobile.
- **X-axis tick count is mobile-aware** (`IS_MOBILE`, shared with `MAX_POINTS`) — 2 ticks on mobile instead of 4/8, since the shorter mobile time window was otherwise printing the same minute-resolution label repeated.
- **Download Agent button** no longer opens `target="_blank"` — the redirector page now loads in the same tab so only the native download prompt appears, not an extra tab left open.

---

## Current State (V3.2 — mobile UI polish: icons, stable chart axes, floating install button)

- **Mobile layout added**: below `860px` width, `.mobile-sidebar` (a compact floating rounded-rectangle-chip dock, tap to expand) replaces the desktop `#tabs` bar and the Overview page's graph grid (`#overview-right-wrap`) is hidden — larger stats-bar numbers only (see mobile-only `#page-overview` sizing rules). Desktop layout (≥860px) is unchanged. Same `index.html`, no separate build/route.
- **Installable as a PWA**: `public/manifest.json` + `public/sw.js` (no caching, installability only) + `public/icon.svg`/`icon-maskable.svg`, offered via a small floating `#install-fab` button + popover (not a banner) — `beforeinstallprompt` on Chrome/Android, manual instructions via UA-sniff on iOS
- **Icon system**: all static UI icons are inline SVG from the `ICONS` map, applied to `[data-icon]` spans by `applyIcons()` — no emoji left in navigation/buttons (see rule above)
- **Chart axes are pre-filled and range-seeded** (`history`/`historyTimes` start at `MAX_POINTS` with nulls/backdated timestamps, `safeChart()` seeds `suggestedMin`/`suggestedMax` per metric) so charts don't visibly rescale/lurch as the first real data points arrive
- **Tooltips work on hover (desktop) and tap (mobile)** via `interaction:{mode:'index', intersect:false}` in `BASE_OPTS` (and mirrored in the multi-chart/session-chart configs) — needed because point radius is 0, so the Chart.js default `intersect:true` trigger could never fire
- Live path unchanged: in-memory `latest`/`lastUpdateMs`, `STALE_MS = 2000` (tuned for the 5 Hz cadence — ~10 missed packets before flagging offline)
- **Persistent path added**: optional Postgres (`DATABASE_URL`, a Railway plugin — not configured by default, degrades gracefully without it) storing every packet as JSONB, auto-grouped into sessions on a 60s gap (`SESSION_GAP_MS`)
- New endpoints: `GET /api/sessions`, `GET /api/sessions/:id/points`, `GET /api/sessions/:id/export.csv` — all public reads, same reasoning as `/api/latest` (nothing sensitive in a telemetry reading)
- **All chart x-axes now show real wall-clock time** — fixed a real bug where live charts plotted against a meaningless incrementing counter and mini-charts hid the x-axis entirely; `/api/latest` responses now carry a server-stamped `received_at` for this (see rules above)
- New **Sessions tab**: lists past sessions with duration/packet count, "VIEW" builds a one-shot set of charts from that session's full history (not live-polled), "CSV" downloads the full session via a direct link
- `CSV_COLUMNS` in `server.js` is a fixed, hand-maintained export column list — must be kept in sync with `METRICS` in `index.html` whenever a packet field is added (see rule above)
- Full tabbed dashboard (Overview, Speed, Power, Temp, RPM, ESC, IMU, Raceline, Link, Multi, Sessions, Raw) — adapted from an earlier project's dashboard design, rewired for this project's actual packet fields
- Live chart history (Overview/dedicated tabs/Multi) is still client-side-only, capped at `MAX_POINTS = 150` (~30s at 5 Hz), separate from the new persistent Sessions path — see rules above
- `TELEMETRY_API_KEY` — must be set in Railway's environment variables for production; falls back to a random per-boot key otherwise (dev-only)

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Current State** section.

"Significant change" means: API contract change (`/api/telemetry` payload shape, auth scheme), new persistent storage added, or a new endpoint. Styling-only edits to `public/index.html` do not require a doc update.
