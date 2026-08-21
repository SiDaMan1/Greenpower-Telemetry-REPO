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
| `setup.bat` | One-time Windows setup: `npm install`, prompts for config if missing, registers hidden auto-start at login, starts it immediately | **Yes** |
| `config.example.json` | Template for local config — copy to `config.json` and fill in real values (or let `setup.bat` do it) | **Yes, as a template** |
| `config.json` | Real config, **now committed with real values** (the repo owner explicitly asked for this, after being told the repo is public and the key would be publicly visible — see git history) — no longer gitignored. `setup.bat`'s prompt-and-write flow only ever triggers if this file is ever deleted. | **Yes, but real credentials — coordinate before changing** |
| `tray-icon.ico` | System tray icon (32×32, generated from the Greenpower badge logo) | **Yes, regenerate from the same source logo if it ever changes** |
| `agent.log` | Runtime log (gitignored) — the only visibility into the agent once it's running hidden via `setup.bat` | **Generated, not checked in** |
| `package.json` | Dependencies (`serialport`, `node-notifier`, `systray`) | **Yes, if adding a real dependency** |
| `installer/GreenpowerAgent.iss` | Inno Setup script — builds `GreenpowerAgentSetup.exe`, the real Windows installer this folder now ships as (Start Menu shortcut, uninstaller, no admin needed). See its own header comment for build command and why it's `PrivilegesRequired=lowest` | **Yes — keep in sync with what `setup.bat` does; see the rule below** |
| `installer/infobefore.txt` | Plain-text page shown before the installer wizard starts (Node.js prerequisite, no-admin note) | **Yes** |
| `installer/dist/` | Build output (gitignored) — `ISCC.exe`-compiled `.exe` lands here, then gets copied to `telemetry_web/public/GreenpowerAgentSetup.exe` to publish it | **Generated, not checked in** |
| `CLAUDE.md` | This file | **Yes — update after significant changes** |

---

## Rules and Constraints

### Never forward without asking — this is the whole point of this agent existing
`promptToForward()` is not optional and not a "nice to have" — the user explicitly asked for consent-based forwarding rather than silent auto-start, specifically because a website that's always live but only shows real data when a human opted in on that specific machine was the desired behavior. Don't add a "remember my choice, don't ask again" auto-accept mode without the user asking for it — that would defeat the reason this agent exists rather than the website talking to the ESP32 directly.

### Device identification is handshake-based, not just "any serial port"
`identifyPort()` doesn't assume a new serial port is the receiver just because it appeared — it opens the port, sends `ID?\n`, and waits for the `DEVICE_ID` string (`GREENPOWER_RX_V1`) to show up, either from the firmware's boot beacon (if the agent was already watching when the device powered up) or the on-demand reply. Ports that don't answer within `IDENTIFY_TIMEOUT_MS` are marked `'not-ours'` and left alone. This must match `DEVICE_ID` in `../greenpower_receiver/greenpower_receiver.ino` exactly — if that firmware constant changes, update it here too.

### `knownPorts` forgets a port when it disappears, on purpose
So unplugging and replugging the same physical receiver re-triggers the full identify → prompt flow rather than silently doing nothing (or silently resuming forwarding without asking again). Don't "fix" this into a persistent allow-list unless the user specifically asks for that — it would also mean silent forwarding after the first plug-in, which contradicts the consent requirement above.

### `node-notifier`'s response casing is not reliable — always compare case-insensitively
The prompt uses `actions: ['Yes', 'No']`, which on Windows (SnoreToast backend) renders as real toast buttons. This caused a real, confirmed bug: SnoreToast returned the response as lowercase `'yes'` even though the button was defined as `'Yes'` — a strict `response === 'Yes'` check silently treated every acceptance as a decline, so clicking "Yes" never started forwarding and looked from the user's side like the whole system was broken. The fix is `response.toLowerCase() === 'yes'`; don't go back to an exact-case string match. A `[DEBUG]` log line prints the raw `response` value on every prompt — leave that in, it's what caught this bug and will catch the next backend-specific surprise.

### Success/failure confirmation notifications fire once per connection, not per packet
`startForwarding()` tracks `confirmed`/`failureNotified` in closure state scoped to that one port connection. The first successful forward triggers a one-time "Forwarding live telemetry..." toast; if the *first* attempt fails and nothing has succeeded yet, a one-time failure toast fires instead. Don't move this logic into `forwardLine()` itself or make it fire on every call — at the sender's ~500ms packet rate, a real outage would otherwise spam a failure toast twice a second.

### Fetch requires Node 18+
`forwardLine()` uses the global `fetch` built into Node 18+, no `node-fetch` dependency. If this ever needs to run on an older Node version, that's a real added dependency, not just a version bump.

### `agent.pid` is what makes `setup.bat` idempotent — don't remove it
The agent writes its own PID to `agent.pid` on startup. `setup.bat` reads that file and `taskkill /F`s the old process before launching a new one, every time it's re-run. Without this, re-running `setup.bat` (e.g. to pick up a code change) piles up multiple hidden `node.exe` instances — this actually happened once: an old instance held a serial port open, and the new instance's `SerialPort.list()`/open attempt failed with "Access denied" on that port, which looked like a hardware/driver problem but was really just a stale process fighting the new one for the same port. If you change how the agent is launched (e.g. a different auto-start mechanism), keep an equivalent single-instance guard.

### `log()` timestamps every line, not just the session start
Needed for real diagnosis — a burst of identical errors within milliseconds (e.g. duplicate processes fighting over one port) looks identical to the same errors spread across a 20-second window in an unstamped log, and those two situations point at completely different root causes. Keep per-line timestamps if `log()` is ever refactored.

### The `log()` helper exists because the agent runs invisibly — don't call `console.*` directly
`setup.bat` registers the agent to run via a hidden `wscript`-launched process with no visible window (`WshShell.Run "node agent.js", 0, False`), so `console.log`/`console.error` output goes nowhere anyone can see. Every message in this file goes through `log()`, which writes to both stdout (useful for the manual `npm start` path) and `agent.log`. If you add new log output, use `log()`, not `console.*` directly, or it'll be invisible in the normal (hidden, auto-started) way this actually runs.

### `agent.log` is truncated fresh on every start, not appended forever
By design — this agent deliberately doesn't log per-packet forwarding activity (only state changes: found/prompted/forwarding/errors), so log volume per run stays small and a fresh-per-run log is simpler than rotation. Don't add per-packet logging to `forwardLine()`'s success path — that would both spam `agent.log` and make the file grow unbounded on an agent left running for weeks.

### The tray icon is a genuine npm package named `systray`, not `node-systray` or `node-systray-v2`
Confirmed by actually installing it — `node-systray`/`node-systray-v2` are the GitHub repo/fork names (and what a lot of copy-pasted READMEs/tutorials show as the import), but the package actually published to npm is called `systray` (`npm i systray`). It's a CommonJS default export, so `require('systray').default`, not `require('systray')` directly (verified empirically — `require('systray')` returns `{ default: SysTrayClass }`). Menu items need all four fields (`title`/`tooltip`/`checked`/`enabled`) — they're non-optional in the type. `action.seq_id` in `onClick()` is the item's index in the `items` array (index 0 = the disabled "Running" status line, index 1 = "Stop Agent" here). `kill(false)` stops the tray helper process without also calling `process.exit()` itself — this agent already has its own `process.on('exit', ...)` cleanup (PID file + tray) shared by every exit path, so the click handler just calls `process.exit(0)` and lets that shared cleanup run once, rather than duplicating it inline.

### Tray icon failures are non-fatal — losing the tray icon must never take down forwarding
`new SysTray(...)` and its `onError` handler are wrapped so a tray-init failure (e.g. running on a Windows build/permission setup where the tray helper binary can't launch) only logs a `[WARN]` and continues — actual telemetry forwarding is this agent's real job and shouldn't depend on a status icon succeeding. If the tray ever needs to become load-bearing for something, that's a deliberate design change, not a refactor.

### Restarting the agent is required to pick up agent.js changes — it doesn't hot-reload
Obvious in hindsight but worth stating: a code change here (like the tray icon addition) only affects the *next* time the agent starts (re-run `setup.bat`, or `taskkill` the old `node.exe` per `agent.pid` and start it again) — an already-running instance keeps running the code it loaded at its own startup.

### The installer (`installer/GreenpowerAgent.iss`) duplicates `setup.bat`'s logic in Pascal Script — keep them in sync
`GreenpowerAgentSetup.exe` is the primary distribution now (linked from both the dashboard's Download Installer button and the repo root README), but `setup.bat` still exists and still works standalone — they're two independent implementations of the same steps (check Node.js is on PATH, `npm install`, write a hidden Startup-folder VBS launcher, kill any previous `agent.pid` process, launch now), not one calling the other. If the setup FLOW itself changes (a new step, a different auto-start mechanism, a new prerequisite check), update both — the `.iss` file's `[Code]` section (`NodeIsOnPath`, `StopExistingAgent`, `WriteStartupLauncher`, `CurStepChanged`) and `setup.bat` will silently drift apart otherwise.
**No admin rights, deliberately**: `PrivilegesRequired=lowest` + `DefaultDirName={localappdata}\Programs\GreenpowerAgent` means the installer never triggers a UAC prompt and never needs Program Files — matters because whoever's running this on a laptop at the track may not have admin on that machine. Don't change the install directory to a Program Files path or otherwise make this require elevation without a real reason.
**Node.js is still a required prerequisite, not bundled.** Packaging the agent into a single, fully standalone `.exe` (via `pkg` or Node's built-in Single Executable Applications feature) was considered and deliberately not attempted — `serialport` is a native addon and `systray` spawns its own helper binary, and bundling native addons into a SEA/pkg single-file executable is a known-fragile combination with edge cases this environment couldn't verify actually work on a real target machine. If that's ever attempted, treat it as a real project with real device testing, not a quick swap.
**Rebuild command** (from `receiver_agent/installer/`, no admin needed): `"%LocalAppData%\Programs\Inno Setup 7\ISCC.exe" GreenpowerAgent.iss` → output lands in `installer/dist/GreenpowerAgentSetup.exe` → copy that over `telemetry_web/public/GreenpowerAgentSetup.exe` to actually publish it (same "rebuild by hand, it's a snapshot not a live proxy" pattern `greenpower-agent.zip` used before it, see `telemetry_web/CLAUDE.md`).

### `setup.bat`'s config-write block avoids delayed-expansion pitfalls on purpose
The `set /p` + config.json write logic uses `goto`/labels instead of nesting inside an `if (...)` parenthesized block — batch expands `%VAR%` at parse time for a whole parenthesized block, so a variable set with `set /p` earlier in the *same* block reads back empty. If you touch `setup.bat`, keep variable-set-then-use sequences as separate top-level lines (or add `setlocal enabledelayedexpansion` + `!VAR!` if you reintroduce a block) rather than reintroducing this bug.

---

## Current State (V1.3 — real Windows installer added)

- Windows-focused (development/testing done on Windows); other platforms untested
- No persistent "remember this device" — every unplug/replug re-prompts
- No retry/queue on forward failure — a failed POST to `telemetry_web` is logged and dropped, not retried
- **Primary distribution is now `GreenpowerAgentSetup.exe`**, a real Inno Setup installer (`installer/GreenpowerAgent.iss`) — Start Menu shortcut, uninstaller in Add/Remove Programs, no admin rights needed. `setup.bat` still exists and still works as a manual/standalone alternative — see the rule above on keeping the two in sync. Both do the same underlying steps: install deps, register a hidden auto-start launcher in the user's Startup folder (`%APPDATA%\...\Startup\GreenpowerReceiverAgent.vbs`), start it immediately. After that, normal use is plug-in + one notification click — no terminal, no manual `npm start`.
- Runs hidden (no console window) once auto-started; `agent.log` (truncated per run) is the only runtime visibility
- System tray icon: shows a Greenpower badge icon in the taskbar tray for as long as the agent process is alive (whether or not anything is currently forwarding), with a right-click menu (`Greenpower Agent — Running` status line + `Stop Agent`) — see the `systray` rules above for the exact package/API gotchas. `config.json` is committed with real credentials (previously gitignored) at the repo owner's explicit request.
- **Not yet tested end-to-end on a real machine** — the installer was built and compiles cleanly, but actually running it (which would install for real, run `npm install`, register Startup auto-start, and start the agent talking to the live API) wasn't done in the session that built it, deliberately, since that has real persistent side effects on whatever machine runs it. Worth a real test pass before treating it as fully proven.

---

## Self-Maintenance Requirement

After every significant change to this project, update this file's **Current State** section.

"Significant change" means: `DEVICE_ID` changes, the JSON schema forwarded changes, the consent/prompt flow changes, or auto-start gets actually implemented. Comment-only edits do not require a doc update.
