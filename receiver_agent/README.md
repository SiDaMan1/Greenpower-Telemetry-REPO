# Greenpower Receiver Agent

Runs on whatever computer the `greenpower_receiver` ESP32 is plugged into. Watches for it over USB, asks before forwarding, and relays live telemetry to the web dashboard.

## Setup (one time)

1. Make sure [Node.js](https://nodejs.org) (LTS) is installed.
2. Download `GreenpowerAgentSetup.exe` (from the dashboard's Download Installer button, or the repo root README) and run it.

The installer walks through a normal setup wizard — no admin rights needed (installs to your own user folder, not Program Files). It installs dependencies, config is already baked in, registers itself to start silently every time you log in, and starts it immediately. It also adds a Start Menu shortcut and a proper uninstaller (Add/Remove Programs).

Building the installer yourself (only needed if you've changed something in this folder): see `installer/GreenpowerAgent.iss` — compile with Inno Setup's `ISCC.exe`, no admin rights needed for that either.

<details>
<summary>Old manual flow (still works, just not what the installer button gives you)</summary>

Double-click **`setup.bat`** directly in this folder instead of using the installer — same underlying steps (installs dependencies, asks for your website URL and API key the first time, registers auto-start, starts it immediately), just without the Start Menu shortcut/uninstaller/wizard UI.
</details>

## Using it (every time after that)

1. Plug in the receiver.
2. A Windows notification pops up: *"Forward live telemetry from COM5 to the dashboard?"*
3. Click it.

That's the whole thing — no commands, no terminal. The agent is already running in the background from login; plugging in the receiver is the only action needed.

## If something's not working

The agent runs hidden with no visible window, so check **`agent.log`** in this folder for what it's doing — it logs every device it sees, prompts shown, and any forwarding errors (not every individual packet, just state changes and problems).

Common issues:
- **No notification appears** — confirm `greenpower_receiver.ino` is actually flashed and running (open a serial monitor on the port directly and confirm you see `GREENPOWER_RX_V1` and `JSON:` lines). Also check nothing else (like the Arduino IDE's own Serial Monitor) has the port open — only one program can hold a serial port at a time.
- **Notification appears but forwarding fails** — check `agent.log` for `[WARN] Forward failed`. Usually means the URL or API key in `config.json` is wrong, or the website isn't reachable.
- **Want to change the URL/API key** — delete `config.json` and re-run `setup.bat`, or just edit `config.json` directly (it's plain JSON) and restart the agent (log out/in, or run `node agent.js` manually once).

## What this does NOT do

- Does not remember your choice between plug-ins — unplug and replug the receiver, and it asks again. This is intentional (see `CLAUDE.md`).
- Does not retry failed sends to the dashboard — a dropped request is just logged and skipped, not queued.
- Windows only, tested via Startup-folder auto-start.

## Manual run (for testing, or non-Windows)

```bash
npm install
cp config.example.json config.json   # then fill it in
npm start
```
This runs it in the foreground with normal console output instead of the hidden/logged mode `setup.bat` configures.
