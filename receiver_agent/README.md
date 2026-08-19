# Greenpower Receiver Agent

Runs on whatever computer the `greenpower_receiver` ESP32 is plugged into. Watches for it over USB, asks before forwarding, and relays live telemetry to the web dashboard.

## Setup

```bash
cd receiver_agent
npm install
cp config.example.json config.json
```

Edit `config.json`:
```json
{
  "websiteUrl": "https://telemetry.yourdomain.com/api/telemetry",
  "apiKey": "same value as TELEMETRY_API_KEY on Railway"
}
```

## Run manually (do this first, before setting up auto-start)

```bash
npm start
```

Plug in the receiver. Within a couple seconds you should see:
```
[FOUND] Greenpower receiver on COM5
```
...followed by a Windows notification asking to forward. Click it to accept — you'll then see `[FORWARD] Starting forwarding from COM5` and the dashboard should go live.

If nothing happens: check that `greenpower_receiver.ino` is actually flashed and running (open a serial monitor on the port directly and confirm you see `GREENPOWER_RX_V1` and `JSON:` lines), and that no other program (like the Arduino IDE's own Serial Monitor) has the port open at the same time — only one program can hold a serial port open at once.

## Auto-start when you log in (Windows)

Once the manual run above works, make it start automatically:

1. Press `Win+R`, type `shell:startup`, hit Enter — this opens your Startup folder.
2. Create a shortcut in that folder pointing to a small batch file, e.g. `start-agent.bat`:
   ```bat
   @echo off
   cd /d "C:\path\to\Greenpower-Telemetry-REPO\receiver_agent"
   node agent.js
   ```
3. Put that `.bat` file's shortcut in the Startup folder. It'll now launch (with a visible console window, so you can see its log output) every time you log in.

For a version that runs silently with no console window, use Windows Task Scheduler instead: create a task triggered "At log on", action = run `node.exe` with argument `agent.js` and "Start in" set to this folder, and check "Run whether user is logged on or not" if you want it to run even without an active session.

## What this does NOT do

- Does not remember your choice between plug-ins — unplug and replug the receiver, and it asks again. This is intentional (see `CLAUDE.md`).
- Does not retry failed sends to the dashboard — a dropped request is just logged and skipped, not queued.
- Only tested on Windows so far.
