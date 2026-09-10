// ════════════════════════════════════════════════════════════════════
//  GREENPOWER RECEIVER AGENT
//
//  Runs on whatever computer the greenpower_receiver ESP32 gets plugged
//  into. Watches for new USB serial ports, identifies whether a newly
//  connected device is actually the Greenpower receiver (via the
//  DEVICE_ID handshake — see ../greenpower_receiver/greenpower_receiver.ino),
//  and if so, ASKS the user (via an OS notification) before forwarding
//  any data anywhere. Never forwards silently.
//
//  On acceptance, parses the "JSON:"-prefixed lines the receiver prints
//  per packet and POSTs each one to telemetry_web's /api/telemetry.
//
//  Config: copy config.example.json to config.json and fill in your
//  Railway URL + API key, or set WEBSITE_URL / TELEMETRY_API_KEY env vars
//  instead (env vars win if both are present — useful for a scheduled
//  task where editing a JSON file next to the script is inconvenient).
//
//  V1.4 additions (see receiver_agent/CLAUDE.md for the full rationale
//  behind each of these):
//    • Auto-update — checks a small JSON manifest served alongside the
//      dashboard, and if a newer version is published, downloads and
//      silently installs it (msiexec /qn), no user action needed.
//    • Local GUI — a tiny HTTP server on 127.0.0.1 only, opened from a
//      new tray menu item ("Show GUI"). Shows live status + a tailing
//      view of agent.log, and has a real "Uninstall" button.
//    • Uninstall button does a genuine full removal — runs the same
//      `msiexec /x` a person would use from Settings > Apps, THEN force-
//      deletes anything msiexec doesn't track (npm's node_modules, logs,
//      the PID file), so nothing is left behind.
// ════════════════════════════════════════════════════════════════════

const fs = require('fs');
const path = require('path');
const os = require('os');
const http = require('http');
const { execFile, spawn } = require('child_process');
const { SerialPort } = require('serialport');
const notifier = require('node-notifier');
const SysTray = require('systray').default;

// Bump this alongside GreenpowerAgent.wxs's <Product Version="..."> AND
// telemetry_web/public/agent-version.json's "version" field, every single
// time agent.js's content changes in any way meant to reach existing
// installs — checkForUpdate() below compares THIS constant against that
// manifest, so a content change with no version bump here is invisible to
// auto-update even though the .msi itself got rebuilt.
const AGENT_VERSION = '1.5.9.0';

// ── Logging ─────────────────────────────────────────────────────────
// Once this runs silently at login (see setup.bat), there's no visible
// console to watch — everything also goes to agent.log next to this file
// so a problem can still be diagnosed after the fact. Truncated fresh on
// every start rather than appended forever, since forwarding activity
// itself is deliberately NOT logged per-packet (only state changes and
// errors are), so this shouldn't grow large within one run anyway.
const LOG_PATH = path.join(__dirname, 'agent.log');
try { fs.writeFileSync(LOG_PATH, `── Greenpower receiver agent started ${new Date().toISOString()} ──\n`); } catch (e) { /* non-fatal */ }

function log(line) {
    // Per-line timestamps (not just one at session start) — without these,
    // a burst of repeated errors is indistinguishable from the same errors
    // spread over a long window, which matters a lot when diagnosing things
    // like multiple agent instances fighting over one serial port.
    const stamped = `[${new Date().toISOString()}] ${line}`;
    console.log(stamped);
    try { fs.appendFileSync(LOG_PATH, stamped + '\n'); } catch (e) { /* non-fatal, don't let logging crash the agent */ }
}

// Last N lines of agent.log, for the GUI's log view — re-reads the file
// each call rather than keeping an in-memory ring buffer, since the file
// is already small (truncated per run) and this is only ever called from
// an occasional GUI page load/poll, not a hot path.
function readLogTail(maxLines) {
    try {
        const text = fs.readFileSync(LOG_PATH, 'utf8');
        const lines = text.split('\n').filter(Boolean);
        return lines.slice(-maxLines);
    } catch (e) {
        return [`(couldn't read agent.log: ${e.message})`];
    }
}

// ── Single-instance lock ───────────────────────────────────────────
// The REAL guard is an atomic lock DIRECTORY, not the plain PID file
// below — `fs.mkdirSync()` either creates the directory or fails with
// EEXIST, atomically, with no window where two processes can both
// "succeed". A plain "read agent.pid, decide, then write agent.pid" (the
// original design here) has a real TOCTOU race: two instances launched
// close together (which has actually happened in practice this project —
// e.g. the Startup-folder launcher firing more than once) can both read
// the file before either has written its own PID, both conclude "I'm
// first", and both end up running — exactly the "fighting over one
// serial port" failure this guard exists to prevent in the first place.
// acquireSingleInstanceLock() closes that window: only one process can
// ever hold LOCK_DIR at a time.
//
// This covers every way a second instance could end up running — the
// Startup-folder auto-launch firing while a previous instance from before
// a restart/sleep is somehow still alive, someone double-clicking the
// launcher shortcut twice, running `node agent.js` manually while the
// hidden auto-started one is already up, an auto-update's freshly
// launched instance overlapping briefly with the one it's replacing, etc.
const LOCK_DIR    = path.join(__dirname, '.agent.lock');
const LOCK_PID_FILE = path.join(LOCK_DIR, 'pid');

function isPidAlive(pid) {
    try {
        // Signal 0 doesn't actually send a signal — it's the standard
        // Node/POSIX idiom for "is this PID still alive", and it throws
        // (ESRCH) if not. Works on Windows too via libuv's emulation.
        process.kill(pid, 0);
        return true;
    } catch (e) {
        return false;
    }
}

// Node has no synchronous sleep — Atomics.wait on a throwaway
// SharedArrayBuffer is the standard, dependency-free way to get one
// anyway, and this runs once at startup before anything else (server,
// tray, port scanning) is set up, so blocking the event loop briefly here
// costs nothing real.
function sleepSyncMs(ms) {
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

function acquireSingleInstanceLock() {
    const MAX_ATTEMPTS = 15;
    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        try {
            fs.mkdirSync(LOCK_DIR);
            fs.writeFileSync(LOCK_PID_FILE, String(process.pid));
            return;   // got it — we're the one and only instance
        } catch (e) {
            if (e.code !== 'EEXIST') throw e;   // a real filesystem problem — don't loop forever on something this can't fix

            // Real, empirically-found race here (not a hypothetical): the
            // owning process's mkdirSync and its writeFileSync(pid) below
            // are two SEPARATE synchronous calls, not one atomic operation
            // — a second process can observe EEXIST from the directory
            // existing, but still read the pid file before the owner's
            // writeFileSync has actually landed, getting an empty file /
            // NaN. Treating an unreadable pid as "stale, delete it" (the
            // original version of this code) deleted a lock the other
            // process had JUST created a few microseconds earlier — both
            // processes then created a fresh lock and both survived,
            // confirmed via debug logging on a real run. `couldReadPid`
            // below distinguishes "genuinely stale" (a parseable pid that
            // isn't alive — safe to clean up) from "owner is still
            // mid-write" (an unparseable pid — wait and retry instead;
            // the owner is a handful of nanoseconds from finishing, MAX_
            // ATTEMPTS × 300ms gives enormous headroom for that).
            let ownerPid = null;
            let couldReadPid = false;
            try {
                ownerPid = parseInt(fs.readFileSync(LOCK_PID_FILE, 'utf8').trim(), 10);
                couldReadPid = Number.isFinite(ownerPid);
            } catch (e2) { /* pid file missing/unreadable — treated as "owner still mid-write" below, not stale */ }

            if (couldReadPid && ownerPid !== process.pid && isPidAlive(ownerPid)) {
                if (attempt === 1) log(`[WARN] Another agent instance (PID ${ownerPid}) is already running — terminating it so this one can take over.`);
                try { process.kill(ownerPid, 'SIGTERM'); } catch (e3) { /* already exiting */ }
            } else if (couldReadPid && ownerPid !== process.pid) {
                // Genuinely stale — the owner recorded in it is gone (a
                // crash or unclean exit that skipped cleanupLock() below).
                // Clear it so the next attempt can actually succeed
                // instead of looping forever against a lock nobody's
                // holding anymore.
                try { fs.rmSync(LOCK_DIR, { recursive: true, force: true }); } catch (e3) { /* best effort */ }
            }
            // else: pid file unreadable/empty — owner likely still
            // mid-write (see this function's own comment above for why
            // that must NOT be treated as stale) — just wait and retry.
            sleepSyncMs(300);   // give the old process a moment to actually exit and release the lock dir (or finish writing its pid)
        }
    }
    // Exhausted every attempt — something is genuinely wrong (a lock
    // holder that won't die, or a permissions problem), not just an
    // ordinary race. Exiting here (rather than proceeding anyway) is the
    // whole point of this guard — running alongside another instance is
    // worse than not running at all.
    log('[ERROR] Could not acquire the single-instance lock after multiple attempts — exiting rather than risk running alongside another instance.');
    process.exit(1);
}
acquireSingleInstanceLock();

function cleanupLock() {
    try {
        if (fs.readFileSync(LOCK_PID_FILE, 'utf8').trim() === String(process.pid)) {
            fs.rmSync(LOCK_DIR, { recursive: true, force: true });
        }
    } catch (e) { /* non-fatal — already gone, or never fully acquired */ }
}

// agent.pid is kept as a plain, human/tool-readable record of the current
// PID (setup.bat reads it to kill a previous hidden instance before
// re-launching) — it is NOT the actual mutual-exclusion mechanism
// anymore, LOCK_DIR above is. Writing it is best-effort/non-fatal since
// nothing safety-critical depends on it existing.
const PID_PATH = path.join(__dirname, 'agent.pid');
try { fs.writeFileSync(PID_PATH, String(process.pid)); } catch (e) { /* non-fatal */ }
function cleanupPidFile() {
    try {
        if (fs.readFileSync(PID_PATH, 'utf8').trim() === String(process.pid)) {
            fs.unlinkSync(PID_PATH);
        }
    } catch (e) { /* non-fatal — file may already be gone */ }
}
// tray/guiServer are declared further down but not referenced until one
// of these fires, by which point they're already assigned — safe despite
// the temporal-dead-zone-looking forward reference.
function cleanupTray() {
    try { if (tray) tray.kill(false); } catch (e) { /* non-fatal */ }
}
function cleanupGuiServer() {
    try { if (guiServer) guiServer.close(); } catch (e) { /* non-fatal */ }
}
process.on('exit', cleanupLock);
process.on('exit', cleanupPidFile);
process.on('exit', cleanupTray);
process.on('exit', cleanupGuiServer);
process.on('SIGINT', () => { cleanupLock(); cleanupPidFile(); cleanupTray(); cleanupGuiServer(); process.exit(); });
process.on('SIGTERM', () => { cleanupLock(); cleanupPidFile(); cleanupTray(); cleanupGuiServer(); process.exit(); });

// ── Config ──────────────────────────────────────────────────────────
const CONFIG_PATH = path.join(__dirname, 'config.json');
let fileConfig = {};
if (fs.existsSync(CONFIG_PATH)) {
    try {
        fileConfig = JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
    } catch (e) {
        log(`[ERROR] config.json exists but isn't valid JSON: ${e.message}`);
    }
}

const WEBSITE_URL = process.env.WEBSITE_URL || fileConfig.websiteUrl;
const API_KEY     = process.env.TELEMETRY_API_KEY || fileConfig.apiKey;

if (!WEBSITE_URL || !API_KEY) {
    log('[ERROR] Missing websiteUrl/apiKey.');
    log('        Copy config.example.json to config.json and fill it in,');
    log('        or set WEBSITE_URL and TELEMETRY_API_KEY environment variables.');
    process.exit(1);
}

// Auto-update manifest and the .msi it points at both live on the same
// dashboard host as WEBSITE_URL (telemetry_web serves both as plain
// static files under public/) — derive the origin instead of requiring a
// second config value nobody would remember to keep in sync.
let DASHBOARD_ORIGIN = null;
try { DASHBOARD_ORIGIN = new URL(WEBSITE_URL).origin; } catch (e) { /* leaves DASHBOARD_ORIGIN null — checkForUpdate() no-ops without it */ }

const DEVICE_ID   = 'GREENPOWER_RX_V1';   // must match greenpower_receiver.ino
const BAUD_RATE   = 115200;
const SCAN_MS     = 2000;    // how often to check for newly plugged-in ports
// Opening the port can reset the board (see identifyPort()'s own comment on
// the DTR-triggered reset finding), and this receiver's own boot sequence
// has a worst-case ~3s wait (`while(!Serial) ... < 3000`) before it's even
// running loop()/answering ID? — 4000ms left almost no margin for a retry
// to land and get answered after that. 6000ms gives real headroom.
const IDENTIFY_TIMEOUT_MS = 6000;
const NOTIFY_TIMEOUT_S    = 20;   // how long the accept/decline prompt stays up
const UPDATE_CHECK_MS     = 60 * 60 * 1000;   // check every 1h (was 6h) — per explicit request, prioritizing reaching people quickly (e.g. after a bad update like 1.5.3.0's) over the small extra bandwidth/load
const GUI_PORT             = 47821;   // arbitrary fixed high port, loopback-only — see startGuiServer()
// Written by the OLD process right before an auto-update hands off to
// msiexec (see performUpdate()), consumed once by the NEW process's own
// startup below — see that block's comment for why a %TEMP% file, not
// something in-process, is what's needed to carry this across the real
// process boundary an update creates.
const UPDATE_SUCCESS_MARKER = path.join(os.tmpdir(), 'greenpower-agent-update-success.json');

// Ports we've already looked at (identified as Greenpower / not / still
// being decided) — keyed by port path, so we don't re-prompt every scan
// tick for the same physical device.
const knownPorts = new Map();   // path -> 'pending' | 'ours' | 'not-ours'

// Live state the GUI reads — kept as a small module-level object rather
// than reaching into knownPorts/closures, since those are keyed/shaped for
// the scan logic's own needs, not for "what should a status page show".
const guiState = {
    startedAt: new Date().toISOString(),
    forwarding: { active: false, port: null, confirmed: false },
    update: { checking: false, lastCheckedAt: null, latestVersion: null, updateAvailable: false, installing: false, lastError: null, justUpdatedTo: null },
};

log(`[READY] Greenpower receiver agent v${AGENT_VERSION} running — watching for USB connections...`);
log(`        Forwarding target: ${WEBSITE_URL}`);

// ── System tray icon ────────────────────────────────────────────────
// The agent runs with no console window at all (see setup.bat's hidden VBS
// launcher) — without this, there's no visible sign the background process
// is even alive short of opening Task Manager. A tray icon plus a menu
// gives a visible "yes, it's running" indicator and an obvious, discoverable
// way to see status or end it, without needing a console/taskbar window.
// tray-icon.ico must stay a real .ico (not .png) — Windows tray icons
// specifically expect that format; see systray's own README for why the
// format differs per-OS.
let tray = null;
try {
    const iconBase64 = fs.readFileSync(path.join(__dirname, 'tray-icon.ico')).toString('base64');
    tray = new SysTray({
        menu: {
            icon: iconBase64,
            title: 'Greenpower Receiver Agent',
            tooltip: 'Greenpower Receiver Agent — running',
            items: [
                // Non-clickable status line — there's no separate "label" item
                // type in this library, so a disabled item does that job.
                { title: 'Greenpower Agent — Running', tooltip: '', checked: false, enabled: false },
                { title: 'Show GUI', tooltip: 'Open the status/log window', checked: false, enabled: true },
                { title: 'Stop Agent', tooltip: 'Stop forwarding and exit', checked: false, enabled: true },
            ],
        },
        debug: false,
        copyDir: true,
    });

    // action.seq_id is the item's index in the items array above — 0 is the
    // disabled status line (never clickable), 1 is "Show GUI", 2 is "Stop
    // Agent". Keep these two branches in sync with the array order if the
    // menu ever grows.
    tray.onClick((action) => {
        if (action.seq_id === 1) {
            openNativeGuiWindow();
        } else if (action.seq_id === 2) {
            log('[INFO] Stop requested from tray icon — exiting.');
            // cleanupPidFile()/cleanupTray()/cleanupGuiServer() all already
            // run via the process 'exit' handlers registered above —
            // process.exit() alone is enough here, no need to duplicate.
            process.exit(0);
        }
    });

    tray.onError((err) => {
        // Non-fatal by design — losing the tray icon shouldn't take down
        // actual telemetry forwarding, which is this agent's real job.
        log(`[WARN] Tray icon error: ${err.message}`);
    });
} catch (e) {
    log(`[WARN] Could not start tray icon (continuing without one): ${e.message}`);
}

// Opens a genuine native Win32 window (WinForms), not a browser tab — a
// direct ask, not just a style choice. No GUI-toolkit dependency added for
// this (no Electron/nw.js — a heavy addition this project's existing
// "keep dependencies minimal" pattern argues against, and a real one given
// this whole installer's no-admin/small-footprint design goals): .NET's
// WinForms is already on every Windows machine this targets, reachable
// via `powershell.exe`, the same "shell out to PowerShell for something
// Node can't do natively" pattern this file already uses for the
// ProductCode COM lookup elsewhere. guiWindowPs1() below generates the
// actual form; this just writes it to %TEMP% (fresh each click — small,
// disposable, not worth caching) and launches it.
// The window is just a client of this agent's own existing loopback HTTP
// API (still on 127.0.0.1:GUI_PORT, unchanged) via Invoke-RestMethod —
// the API didn't need to change at all, only how it's presented to the
// user did.
function openNativeGuiWindow() {
    const ps1Path = path.join(os.tmpdir(), 'greenpower-agent-gui.ps1');
    try {
        // Leading UTF-8 BOM is deliberate — without one, Windows PowerShell
        // 5.1 reads a .ps1 file's own literal text using the system's ANSI
        // codepage, not UTF-8, so any non-ASCII character written directly
        // into guiWindowPs1()'s script text (not just data it fetches at
        // runtime) would risk the same kind of mojibake the log view had.
        // The BOM makes PowerShell detect UTF-8 correctly regardless.
        fs.writeFileSync(ps1Path, '\uFEFF' + guiWindowPs1(), 'utf8');
    } catch (e) {
        log(`[WARN] Couldn't write GUI window script: ${e.message}`);
        return;
    }
    // ⚠️ REAL, CONFIRMED bug — took several passes and a controlled A/B
    // test to actually root-cause, worth recording in full since two
    // earlier, PLAUSIBLE-SOUNDING theories along the way both turned out
    // to be wrong when tested:
    //   1. First theory: "`-WindowStyle Hidden` suppresses the WinForms
    //      window itself, not just the console — switch to a VBS
    //      wrapper." Disproven — the VBS-wrapped launch failed too.
    //   2. Second theory: "powershell.exe defaults to MTA, WinForms needs
    //      STA." `-STA` IS correct and necessary (WinForms genuinely does
    //      require it, and it's kept below) — but adding it alone did NOT
    //      reliably fix the failure in further testing, so it wasn't the
    //      (whole) story either.
    // The ACTUAL root cause, found via a controlled test that spawned the
    // SAME `powershell.exe -Command "exit 42"` four ways and compared
    // exit codes: **`{ detached: true }` on Windows silently breaks
    // powershell.exe's own argument handling** — with it, the process
    // starts, does nothing, and exits 0 (as if no `-Command`/`-File` had
    // been given at all, not even an error); without it, the exact same
    // invocation runs correctly and returns the real exit code. This has
    // nothing to do with `-WindowStyle`, STA/MTA, or anything on the
    // launched-script side — it reproduced with `-Command "exit 42"`
    // alone, no WinForms involved. `detached` was never actually needed
    // here anyway — `child.unref()` alone already accomplishes "don't
    // let this hold the agent's event loop open," which was the entire
    // reason `detached` was added in the first place.
    const child = spawn('powershell.exe', ['-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', ps1Path], { stdio: 'ignore' });
    child.unref();
}

setInterval(scanPorts, SCAN_MS);
scanPorts();

async function scanPorts() {
    let ports;
    try {
        ports = await SerialPort.list();
    } catch (e) {
        log(`[ERROR] Failed to list serial ports: ${e.message}`);
        return;
    }

    const currentPaths = new Set(ports.map(p => p.path));

    // Forget ports that disappeared, so replugging the same device re-runs
    // the identify/prompt flow instead of staying silently ignored forever.
    for (const knownPath of knownPorts.keys()) {
        if (!currentPaths.has(knownPath)) {
            log(`[INFO] ${knownPath} disconnected.`);
            knownPorts.delete(knownPath);
        }
    }

    for (const p of ports) {
        if (!knownPorts.has(p.path)) {
            knownPorts.set(p.path, 'pending');
            identifyPort(p.path);
        }
    }
}

function identifyPort(portPath) {
    let settled = false;
    let port;
    try {
        port = new SerialPort({ path: portPath, baudRate: BAUD_RATE }, (err) => {
            if (err) {
                log(`[INFO] Couldn't open ${portPath} (${err.message}) — likely in use by something else, skipping.`);
                knownPorts.set(portPath, 'not-ours');
            }
        });
    } catch (e) {
        // Previously completely silent — the exact same class of gap the
        // identify-timeout branch below already had fixed once (see that
        // rule in CLAUDE.md), just missed here. `new SerialPort(...)` can
        // throw SYNCHRONOUSLY (not just via the async open-error callback
        // above) — e.g. the port handle not fully released yet by a just-
        // killed previous agent instance (see the single-instance PID
        // guard above) is a real, plausible trigger. Without this log line,
        // a port hitting this path produces ZERO output ever, for as long
        // as it stays plugged in — indistinguishable from "the agent never
        // even tried," which is exactly what made this so hard to diagnose
        // from agent.log alone.
        log(`[INFO] Couldn't open ${portPath} (${e.message}) — likely in use by something else, skipping.`);
        knownPorts.set(portPath, 'not-ours');
        return;
    }

    let buffer = '';
    const onData = (chunk) => {
        if (settled) return;
        buffer += chunk.toString('utf8');
        if (buffer.includes(DEVICE_ID)) {
            settled = true;
            clearInterval(retryTimer);
            clearTimeout(timeout);
            port.removeListener('data', onData);
            knownPorts.set(portPath, 'ours');
            promptToForward(portPath, port);
        }
    };
    port.on('data', onData);
    port.on('error', () => { /* handled by open callback / timeout */ });

    // Opening a serial port to an Arduino-style board commonly toggles DTR,
    // which on many boards (including the auto-reset circuit this receiver
    // uses) triggers a genuine hardware RESET of the board — a real,
    // confirmed cause of silent identify failures: the ORIGINAL single
    // ID?\n write (sent once, immediately on open) can race that reset and
    // land while the board is still rebooting, well before its ~3s boot
    // wait finishes and loop()/pollIdentityRequest() is even running to
    // see it. Nothing ever asked again after that one lost write, so a
    // board that was genuinely fine (and DID show up correctly in Device
    // Manager) would still silently fail to identify. Retrying every
    // 500ms for the whole identify window fixes this — whichever write
    // lands after the board's actually finished booting gets answered;
    // the ones lost to an in-progress reset are cheap, harmless no-ops.
    port.write('ID?\n', (err) => { /* ignore write errors, retries/timeout cover it */ });
    const retryTimer = setInterval(() => {
        if (settled) return;
        port.write('ID?\n', (err) => { /* ignore — same reasoning as the first write above */ });
    }, 500);

    const timeout = setTimeout(() => {
        if (settled) return;
        settled = true;
        clearInterval(retryTimer);
        // Previously silent — a real gap that made this exact failure mode
        // (port opens fine, never answers ID?) indistinguishable in
        // agent.log from "nothing ever tried". Logging it here is what
        // actually surfaced the DTR-reset race above during diagnosis.
        log(`[INFO] ${portPath} didn't answer the identify handshake within ${IDENTIFY_TIMEOUT_MS}ms — assuming it's not the Greenpower receiver.`);
        knownPorts.set(portPath, 'not-ours');
        port.removeListener('data', onData);
        port.close(() => {});
    }, IDENTIFY_TIMEOUT_MS);
}

function promptToForward(portPath, port) {
    log(`[FOUND] Greenpower receiver on ${portPath}`);

    notifier.notify(
        {
            title: 'Greenpower Receiver Connected',
            message: `Forward live telemetry from ${portPath} to the dashboard?`,
            wait: true,
            timeout: NOTIFY_TIMEOUT_S,
            actions: ['Yes', 'No'],   // Windows (SnoreToast backend) renders these as real toast buttons
        },
        (err, response, metadata) => {
            // node-notifier's response strings vary by OS/notifier backend, AND
            // by observed behavior even on the SAME backend the button label's
            // case isn't guaranteed to come back as typed (SnoreToast returned
            // lowercase 'yes' for a button defined as 'Yes') — so compare
            // case-insensitively rather than against an exact literal.
            log(`[DEBUG] Notification response: ${JSON.stringify(response)}`);
            const accepted = typeof response === 'string' && response.toLowerCase() === 'yes';
            if (accepted) {
                log(`[FORWARD] Starting forwarding from ${portPath}`);
                startForwarding(portPath, port);
            } else {
                log(`[SKIP] Not forwarding ${portPath} (response: ${response || 'none'}).`);
                port.close(() => {});
            }
        }
    );
}

function startForwarding(portPath, port) {
    let buffer = '';
    // Confirm/alert only once per connection — not per packet, or the very
    // first hiccup on an otherwise-fine link would spam a failure toast every
    // 500ms, and a working link would spam a success toast just as often.
    let confirmed = false;
    let failureNotified = false;

    guiState.forwarding = { active: true, port: portPath, confirmed: false };

    port.on('data', (chunk) => {
        buffer += chunk.toString('utf8');
        let idx;
        while ((idx = buffer.indexOf('\n')) !== -1) {
            const line = buffer.slice(0, idx).trim();
            buffer = buffer.slice(idx + 1);
            if (line.startsWith('JSON:')) {
                forwardLine(line.slice(5), (ok) => {
                    if (ok && !confirmed) {
                        confirmed = true;
                        guiState.forwarding.confirmed = true;
                        log(`[FORWARD] Confirmed — first packet from ${portPath} reached the dashboard.`);
                        notifier.notify({
                            title: 'Greenpower Receiver',
                            message: `Forwarding live telemetry from ${portPath} to the dashboard.`,
                            timeout: 5,
                        });
                    } else if (!ok && !confirmed && !failureNotified) {
                        failureNotified = true;
                        notifier.notify({
                            title: 'Greenpower Receiver — Forwarding Failed',
                            message: 'Could not reach the dashboard. Check config.json and agent.log.',
                            timeout: 8,
                        });
                    }
                });
            }
        }
    });

    port.on('close', () => {
        log(`[INFO] ${portPath} closed — stopped forwarding.`);
        if (guiState.forwarding.port === portPath) {
            guiState.forwarding = { active: false, port: null, confirmed: false };
        }
    });
}

async function forwardLine(jsonStr, onResult) {
    let data;
    try {
        data = JSON.parse(jsonStr);
    } catch (e) {
        log(`[WARN] Bad JSON from receiver, skipping: ${e.message}`);
        if (onResult) onResult(false);
        return;
    }

    try {
        const res = await fetch(WEBSITE_URL, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': `Bearer ${API_KEY}`,
            },
            body: JSON.stringify(data),
        });
        if (!res.ok) {
            log(`[WARN] Forward failed: HTTP ${res.status}`);
            if (onResult) onResult(false);
            return;
        }
        if (onResult) onResult(true);
    } catch (e) {
        log(`[WARN] Forward failed: ${e.message}`);
        if (onResult) onResult(false);
    }
}


// ════════════════════════════════════════════════════════════════════
//  AUTO-UPDATE
//
//  Checks a small static JSON manifest (telemetry_web/public/agent-
//  version.json — same host as WEBSITE_URL, zero server code needed since
//  it's just a static file) for a version newer than AGENT_VERSION. If
//  found, downloads the linked .msi to a temp file and hands off to a
//  detached VBS helper that waits for THIS process to fully exit (so
//  nothing here is holding a file handle Windows Installer needs), then
//  runs `msiexec /i ... /qn`. The installer's own MajorUpgrade element
//  (GreenpowerAgent.wxs) does a clean uninstall-then-reinstall of the old
//  version automatically — nothing extra needed on this side for that
//  part. LaunchAgentNow (also already in the .wxs) starts the new version
//  immediately after install, so this is genuinely hands-off.
// ════════════════════════════════════════════════════════════════════

// Simple dotted-quad numeric compare (MSI Version fields are always
// exactly this shape, e.g. "1.4.0.0") — returns true if `a` is newer than
// `b`. Not a general semver comparator on purpose; MSI versions are a
// fixed 4-part numeric format, and pulling in a semver dependency for this
// one comparison would be a strange trade against this project's existing
// "keep dependencies minimal" pattern.
function isNewerVersion(a, b) {
    const pa = String(a).split('.').map(n => parseInt(n, 10) || 0);
    const pb = String(b).split('.').map(n => parseInt(n, 10) || 0);
    for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
        const x = pa[i] || 0, y = pb[i] || 0;
        if (x !== y) return x > y;
    }
    return false;
}

async function checkForUpdate(manual) {
    if (!DASHBOARD_ORIGIN) {
        log('[WARN] Auto-update: WEBSITE_URL is not a valid URL, can\'t derive the manifest location — skipping.');
        return;
    }
    if (guiState.update.installing) return;   // already mid-install, don't double-trigger

    guiState.update.checking = true;
    try {
        const res = await fetch(`${DASHBOARD_ORIGIN}/agent-version.json`, { cache: 'no-store' });
        if (!res.ok) {
            log(`[WARN] Auto-update: manifest fetch failed, HTTP ${res.status}`);
            guiState.update.lastError = `manifest HTTP ${res.status}`;
            return;
        }
        const manifest = await res.json();
        guiState.update.lastCheckedAt = new Date().toISOString();
        guiState.update.latestVersion = manifest.version || null;
        guiState.update.lastError = null;

        if (manifest.version && manifest.msiUrl && isNewerVersion(manifest.version, AGENT_VERSION)) {
            guiState.update.updateAvailable = true;
            log(`[UPDATE] Newer version available: ${manifest.version} (running ${AGENT_VERSION}) — starting silent update.`);
            await performUpdate(manifest.version, manifest.msiUrl);
        } else {
            guiState.update.updateAvailable = false;
            if (manual) log(`[UPDATE] Already up to date (running ${AGENT_VERSION}, latest is ${manifest.version || 'unknown'}).`);
        }
    } catch (e) {
        log(`[WARN] Auto-update check failed: ${e.message}`);
        guiState.update.lastError = e.message;
    } finally {
        guiState.update.checking = false;
    }
}

async function performUpdate(newVersion, msiUrl) {
    guiState.update.installing = true;
    try {
        const res = await fetch(msiUrl);
        if (!res.ok) {
            log(`[WARN] Auto-update: couldn't download ${msiUrl} (HTTP ${res.status})`);
            guiState.update.installing = false;
            return;
        }
        const buf = Buffer.from(await res.arrayBuffer());
        const tempMsiPath = path.join(os.tmpdir(), `GreenpowerAgentSetup-${newVersion}.msi`);
        fs.writeFileSync(tempMsiPath, buf);
        log(`[UPDATE] Downloaded ${tempMsiPath} (${buf.length} bytes). Handing off to installer and exiting.`);

        // Marker consumed by the NEXT agent process's own startup (see
        // near acquireSingleInstanceLock() below) — per explicit request
        // ("it should open the GUI back up and show successful update"):
        // this process is about to exit and hand off to msiexec, so it
        // has no way to show a "success" state itself; the NEW process
        // that LaunchAgentNow starts after install is what needs to know
        // it just came from an update, not a normal boot, so it can
        // auto-open the GUI and report success. A plain file in %TEMP%
        // (not e.g. an env var) is what survives across the real process
        // boundary here — msiexec/LaunchAgentNow starts a genuinely new
        // process tree, not a child of this one.
        try {
            fs.writeFileSync(UPDATE_SUCCESS_MARKER, JSON.stringify({ version: newVersion }));
        } catch (e) { /* non-fatal — worst case, the update still completes, just without the auto-reopen/success message */ }

        notifier.notify({
            title: 'Greenpower Receiver Agent — Updating',
            message: `Installing version ${newVersion}. The agent will restart automatically.`,
            timeout: 6,
        });

        // A detached helper does the actual install AFTER this process has
        // fully exited — msiexec replacing agent.js while this same file is
        // still loaded/running is exactly the kind of file-lock race a
        // background auto-updater must not risk. See the helper's own
        // comment (buildUpdateHelperVbs) for the full reasoning.
        const helperPath = writeUpdateHelperVbs(tempMsiPath, process.pid);
        const child = spawn('wscript.exe', [helperPath], { detached: true, stdio: 'ignore' });
        child.unref();

        // Give the notification a moment to actually reach the OS before
        // this process (and its notifier child process, if any) disappears.
        setTimeout(() => process.exit(0), 1500);
    } catch (e) {
        log(`[WARN] Auto-update install failed: ${e.message}`);
        guiState.update.installing = false;
        guiState.update.lastError = e.message;
    }
}

// Self-locating-style hidden helper (same "no visible console window"
// requirement as agent-launcher.vbs and the installer's own WixQuietExec
// custom action — a flashing cmd window during a silent background update
// would be a startling, unexplained thing for someone to see). Written
// fresh into %TEMP% on every update (not shipped as a static installed
// file) since its content embeds the specific PID/msi path for this one
// update — there's nothing to "install", it's a disposable one-shot script.
function writeUpdateHelperVbs(msiPath, waitForPid) {
    const vbs = `
Set fso = CreateObject("Scripting.FileSystemObject")
Set WshShell = CreateObject("WScript.Shell")

' Wait for the currently-running agent to fully exit — Windows Installer
' replacing agent.js (and other installed files) while this same process
' still has them loaded is exactly the race this wait avoids. Bounded to
' ~30s so a stuck process can't wedge the update forever.
pid = "${waitForPid}"
attempts = 0
Do While attempts < 100
  found = False
  For Each p In GetObject("winmgmts:").ExecQuery("Select ProcessId from Win32_Process Where ProcessId=" & pid)
    found = True
  Next
  If Not found Then Exit Do
  WScript.Sleep 300
  attempts = attempts + 1
Loop

' Silent install — MajorUpgrade in GreenpowerAgent.wxs handles cleanly
' replacing the old version; LaunchAgentNow (also in the .wxs) starts the
' new version immediately once install finishes, so nothing else here
' needs to launch it.
WshShell.Run "msiexec.exe /i ""${msiPath}"" /qn /norestart", 0, True

On Error Resume Next
fso.DeleteFile "${msiPath}", True
fso.DeleteFile WScript.ScriptFullName, True
`.trim();
    const vbsPath = path.join(os.tmpdir(), `greenpower-agent-update-${Date.now()}.vbs`);
    fs.writeFileSync(vbsPath, vbs);
    return vbsPath;
}

// Checks immediately on boot (was a 15s-after-startup delay — per explicit
// request, "check on boot"), then every UPDATE_CHECK_MS thereafter. A
// too-early network hiccup right at boot isn't a real risk here: a failed
// fetch is already caught and logged as [WARN] (see checkForUpdate()'s own
// try/catch), not fatal — it just quietly waits for the next hourly tick.
checkForUpdate(false);
setInterval(() => checkForUpdate(false), UPDATE_CHECK_MS);


// ════════════════════════════════════════════════════════════════════
//  UNINSTALL  (real, full removal — used by the GUI's Uninstall button)
//
//  Goal: after clicking Uninstall, NOTHING of this agent is left on the
//  system — no files, no registry, no Startup entry, no Add/Remove
//  Programs listing, no running process. `msiexec /x` alone doesn't
//  fully achieve this: it correctly removes every WiX-tracked Component
//  (files installed by the MSI itself, the two shortcuts, the registry
//  keys used as per-user KeyPaths), but `npm install` (run as a
//  CustomAction during install, see GreenpowerAgent.wxs) populates
//  node_modules AFTER install, and files created that way are NOT MSI-
//  tracked Components — msiexec has no idea they exist, and won't remove
//  them. Same for agent.log/agent.pid, written at runtime. So the real
//  flow here is: msiexec /x first (the "correct", registered uninstall),
//  THEN force-delete whatever's left in the install folder.
// ════════════════════════════════════════════════════════════════════

// Must match GreenpowerAgent.wxs's <Product UpgradeCode="..."> EXACTLY —
// that value is fixed across every build (unlike ProductCode, which WiX
// regenerates fresh each build via Id="*"), which is exactly why this is
// the right thing to look up BY, not something to hardcode a ProductCode
// for directly.
const UPGRADE_CODE = '{1CF7948C-D8F6-410F-A05B-0B14F255A3F6}';

// Finds the ProductCode GUID Windows Installer actually registered THIS
// install under — needed because `msiexec /x` takes a ProductCode, and
// there's no fixed constant for that to hardcode (see UPGRADE_CODE above).
//
// A first attempt at this scanned HKCU\...\Uninstall for a DisplayName
// match via `reg query /s` — that turned out to be WRONG for this
// specific install type, confirmed empirically: a per-user MSI install
// (InstallScope="perUser") does NOT create an entry there at all. Its
// real registration lives under
// HKLM\SOFTWARE\...\Installer\UserData\<user-SID>\Products\<COMPRESSED
// GUID>\InstallProperties — readable without admin (scoped to the
// current user's own SID subtree) but keyed by a "compressed"/packed GUID
// encoding (a byte-order-reversed re-packing of the real ProductCode),
// not the standard dashed-GUID form `msiexec /x` actually needs. Manually
// decoding that packing is exactly the kind of fragile, easy-to-get-
// subtly-wrong text munging this project has hit real bugs from before.
//
// The robust fix: ask the Windows Installer COM API directly —
// `Installer.RelatedProducts(upgradeCode)` returns the real, standard-
// format ProductCode(s) for a given UpgradeCode, which is exactly what
// this needs and is the same mechanism `msiexec`/Windows itself uses
// internally. Confirmed working directly against a real install before
// shipping this (not just reasoned about). Shelled out via powershell.exe
// (same `New-Object -ComObject WindowsInstaller.Installer` pattern this
// project's own build-verification steps already use) since Node has no
// native COM support and adding one just for this single lookup isn't
// worth a new dependency.
// Returns EVERY ProductCode currently registered under this UpgradeCode,
// not just one. Normally there's exactly one — but a real edge case was
// found and confirmed while testing this: installing two builds that
// happen to share the same Version (e.g. rapid local iteration without
// bumping Version between them, or any other reason two ProductCodes
// under the same UpgradeCode end up registered at once) means
// MajorUpgrade's "replace the old ProductCode" behavior doesn't
// necessarily collapse them down to one — RelatedProducts can genuinely
// return more than one GUID. Uninstalling only the first one found would
// leave the other one still registered (still shows in Settings > Apps,
// still has leftover HKLM UserData registration) — the opposite of the
// "zero trace" goal this whole feature exists for. So every match found
// here gets uninstalled, not just one.
function findInstalledProductCodes(callback) {
    const psCmd = `$installer = New-Object -ComObject WindowsInstaller.Installer; ` +
                  `$r = $installer.RelatedProducts('${UPGRADE_CODE}'); ` +
                  `foreach ($p in $r) { Write-Output $p }`;
    execFile('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', psCmd], { windowsHide: true }, (err, stdout) => {
        if (err) { callback([]); return; }
        const matches = String(stdout).match(/\{[0-9A-Fa-f-]{36}\}/g);
        callback(matches || []);
    });
}

function writeUninstallHelperVbs(productCodes, installFolder, waitForPid) {
    // Startup-folder shortcut path — msiexec /x's own StartupShortcut
    // component removal should already delete this, but it's cheap
    // insurance to also delete it explicitly by its known, fixed name
    // (matches the Name="GreenpowerReceiverAgent" Shortcut in the .wxs).
    const startupLnk = path.join(os.homedir(), 'AppData', 'Roaming', 'Microsoft', 'Windows', 'Start Menu', 'Programs', 'Startup', 'GreenpowerReceiverAgent.lnk');

    const vbs = `
Set fso = CreateObject("Scripting.FileSystemObject")
Set WshShell = CreateObject("WScript.Shell")

' Wait for the agent to fully exit before touching its own files/folder.
pid = "${waitForPid}"
attempts = 0
Do While attempts < 100
  found = False
  For Each p In GetObject("winmgmts:").ExecQuery("Select ProcessId from Win32_Process Where ProcessId=" & pid)
    found = True
  Next
  If Not found Then Exit Do
  WScript.Sleep 300
  attempts = attempts + 1
Loop

On Error Resume Next

' The REAL, registered uninstall — removes every MSI-tracked File,
' Shortcut, and RegistryValue, and takes this off the Add/Remove
' Programs / Settings > Apps list. Loops over EVERY related ProductCode
' found (see findInstalledProductCodes()'s own comment for why there can
' genuinely be more than one) rather than assuming there's only one.
${productCodes.length > 0
    ? productCodes.map(pc => `WshShell.Run "msiexec.exe /x ${pc} /qn /norestart", 0, True`).join('\n')
    : "' No ProductCode found — msiexec /x skipped, falling through to manual cleanup only."}

' Belt-and-suspenders folder removal — the REAL fix for node_modules
' (populated by the post-install npm install CustomAction, not an
' MSI-tracked Component) and agent.log/agent.pid (written at runtime,
' also untracked) now lives in GreenpowerAgent.wxs itself: a dedicated
' un-impersonated CustomAction (RemoveAgentRuntimeFiles, Impersonate="no")
' removes all three AS PART OF the same msiexec /x transaction above,
' running with the Windows Installer service's own full rights rather
' than whatever token launched msiexec /x — see that CustomAction's own
' comment in the .wxs for why this had to move there. By the time
' msiexec /x (called above, wait=True) has returned, INSTALLFOLDER should
' already be gone entirely via WiX's own RemoveFolder, run natively as
' part of that same transaction.
' This retry loop is just insurance for a separate, smaller timing quirk
' also seen during testing: with the actual CONTENT cleanup above already
' working correctly (confirmed empty via direct inspection immediately
' after msiexec /x returns — this is not a permissions problem, that part
' is fixed), the now-empty top-level folder entry itself can still take
' up to roughly a minute to actually disappear (Test-Path/Explorer both
' still report it existing that whole time) before clearing on its own —
' looked like leftover NTFS/Windows Installer teardown bookkeeping
' settling asynchronously, not a real lock.
'
' ⚠️ REAL, CONFIRMED bug found here: the original version of this loop
' called fso.DeleteFolder(installFolder, True) — a RECURSIVE delete —
' unconditionally on every retry, for up to 90 SECONDS after Uninstall
' was clicked, with no check for what might legitimately be at that path
' BY THEN. A user who reinstalled within that ~90s window (a completely
' reasonable thing to do — nothing in the UI suggests waiting) got their
' brand new install's files deleted out from under it by this leftover
' script, moments after they'd been written — confirmed as the actual
' cause of a real "install fails right after using Uninstall" report,
' not a hypothetical. Windows Installer itself was never the problem;
' this script deleting a live reinstall was.
' Fix: check the folder is genuinely EMPTY before every single delete
' attempt, and stop immediately (don't delete, don't keep retrying) the
' moment it isn't. A leftover from THIS uninstall is always empty by
' this point (real content is already gone via the CustomAction above) —
' anything with actual files/subfolders in it by the time a retry runs
' is, by construction, something else's content now (a reinstall), never
' this uninstall's own leftover, and must be left alone.
deleteAttempts = 0
Do While fso.FolderExists("${installFolder}") And deleteAttempts < 90
  Set installFolderObj = fso.GetFolder("${installFolder}")
  If installFolderObj.Files.Count = 0 And installFolderObj.SubFolders.Count = 0 Then
    fso.DeleteFolder "${installFolder}", True
    If fso.FolderExists("${installFolder}") Then WScript.Sleep 1000
  Else
    Exit Do
  End If
  deleteAttempts = deleteAttempts + 1
Loop

' Belt-and-suspenders in case msiexec /x didn't run (no ProductCode found)
' or didn't fully clean these up for any reason.
If fso.FileExists("${startupLnk}") Then fso.DeleteFile "${startupLnk}", True
WshShell.RegDelete "HKCU\\Software\\Greenpower\\ReceiverAgent\\"

' Self-delete — this script is disposable, written fresh to %TEMP% for
' this one uninstall, not something meant to persist.
fso.DeleteFile WScript.ScriptFullName, True
`.trim();
    const vbsPath = path.join(os.tmpdir(), `greenpower-agent-uninstall-${Date.now()}.vbs`);
    fs.writeFileSync(vbsPath, vbs);
    return vbsPath;
}

function triggerUninstall(onHandedOff) {
    log('[UNINSTALL] Uninstall requested from GUI — locating installed product(s)...');
    findInstalledProductCodes((productCodes) => {
        if (productCodes.length === 0) {
            log('[WARN] Uninstall: could not find a registered ProductCode for "Greenpower Receiver Agent" — proceeding with manual file/registry cleanup only (msiexec /x will be skipped, so it will NOT be removed from Settings > Apps).');
        } else {
            log(`[UNINSTALL] Found ${productCodes.length} ProductCode(s): ${productCodes.join(', ')}. Handing off to uninstall helper and exiting.`);
        }
        const helperPath = writeUninstallHelperVbs(productCodes, __dirname, process.pid);
        const child = spawn('wscript.exe', [helperPath], { detached: true, stdio: 'ignore' });
        child.unref();
        if (onHandedOff) onHandedOff();
        setTimeout(() => process.exit(0), 800);
    });
}


// ════════════════════════════════════════════════════════════════════
//  LOCAL GUI  (loopback-only HTTP server — status, log, uninstall)
//
//  No framework, no new dependency — a background agent that already
//  keeps its dependency list deliberately small (see CLAUDE.md) doesn't
//  need Express for three routes. Bound to 127.0.0.1 specifically, never
//  0.0.0.0 — this must never be reachable from the network, only from
//  processes on the same machine.
//
//  This HTTP API is presented to the user as a native WinForms window
//  (openNativeGuiWindow()/guiWindowPs1() above), NOT a browser tab — this
//  server's routes are its data layer either way, unchanged by that
//  choice. guiPageHtml() (below) is kept as a working fallback reachable
//  by navigating to http://127.0.0.1:GUI_PORT/ directly (e.g. if
//  PowerShell/WinForms were ever unavailable for some reason) — nothing
//  currently links to it, "Show GUI" no longer opens a browser.
// ════════════════════════════════════════════════════════════════════

// Generates the native GUI window's actual PowerShell/WinForms source —
// see openNativeGuiWindow()'s own comment for why this exists instead of
// a GUI-toolkit dependency. Pure client of this file's own HTTP API
// (Invoke-RestMethod against 127.0.0.1:GUI_PORT) — every value the window
// shows comes from the same /api/status and /api/log routes the old
// browser page used, so the two stay in sync automatically; nothing here
// duplicates agent state directly.
function guiWindowPs1() {
    return `
# ── DPI + visual-styles bootstrap — MUST run before any Form/control is
# created ────────────────────────────────────────────────────────────
# Real, confirmed root cause of "everything looks pixelated/blurry":
# TWO separate things a normal C# WinForms app's Main() always does
# automatically (via Application.EnableVisualStyles()/an app manifest)
# that a Form hosted directly from a PowerShell script never got:
#   1. No DPI-awareness declaration at all — Windows silently renders
#      an unaware app at 96 DPI into an offscreen bitmap, then STRETCHES
#      that bitmap to fit an actual scaled display (100% is rare on a
#      modern laptop; 125%/150% is typical) — this is exactly what a
#      blurry/pixelated (as opposed to just "small") UI looks like.
#   2. No Application.EnableVisualStyles() call — without it, every
#      control (buttons especially) renders with the classic unthemed
#      Windows 2000-era GDI look (flat, aliased, blocky) instead of the
#      current theme's smoother visual style. This alone independently
#      contributes to looking dated, on top of the DPI issue.
# SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) is the modern
# (Windows 10 1703+) fix for #1; it must be set before the first window
# handle is created, which is why this whole block sits above even the
# Add-Type -AssemblyName System.Windows.Forms line's actual usage.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;
public class NativeDpi {
    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
}
public class NativeDwm {
    [DllImport("dwmapi.dll")]
    public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);
}
public class NativeCursor {
    [DllImport("user32.dll")]
    public static extern IntPtr LoadCursor(IntPtr hInstance, int lpCursorName);
}
// ⚠️ REAL bug, reported directly: "copying text is still buggy, it
// flashes when updating." A plain Clear() followed by a burst of
// AppendText() calls repaints the RichTextBox on every intermediate
// step by default — visibly blanking it for a frame before the new
// content finishes filling back in, every single time the log
// actually changes. WM_SETREDRAW (there is no public BeginUpdate/
// EndUpdate on RichTextBox the way there is on ListBox/ListView) is
// the standard, documented way to suspend a control's repainting for a
// batch of changes and repaint once at the end instead.
public class NativeRedraw {
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, int msg, bool wParam, int lParam);
}
// ⚠️ REAL bug, reported directly: the hand-cursor fix on buttons didn't
// carry over to the "GUI available at ..." link inside the log — because
// RichTextBox's OWN built-in URL-hover cursor is set internally by its
// native WndProc handling of WM_SETCURSOR, not by anything settable from
// managed code's Cursor property. Overriding it requires intercepting
// that exact message ourselves, which requires a real subclass — there's
// no property/event on plain RichTextBox that reaches this.
public class HandCursorRichTextBox : RichTextBox {
    // Set once from the PowerShell side to the SAME real system hand
    // cursor handle the buttons use, so link and button hover cursors
    // are visually identical, not two different fixes.
    public static IntPtr HandCursorHandle = IntPtr.Zero;
    [DllImport("user32.dll")]
    static extern IntPtr SetCursor(IntPtr hCursor);
    // ⚠️ REAL bug, reported directly ("there is still a tiny hand on
    // the links") in the FIRST attempt at this fix, which checked
    // SelectionFont.Underline as a proxy for "is this character part of
    // a detected link". That was wrong: DetectUrls marks a link using
    // the native RichEdit control's own CFE_LINK character effect — a
    // completely separate bit from the standard CFE_UNDERLINE effect
    // that Font.Underline actually reflects. A detected link LOOKS
    // underlined (that's RichEdit's own default rendering for CFE_LINK)
    // but Font.Underline never becomes true for it, so the old check
    // was always false and this override never actually fired for a
    // real link. Reading the real CFE_LINK bit would need manually
    // marshaling the native CHARFORMAT2 struct via EM_GETCHARFORMAT —
    // doable, but fragile to get exactly right. Simpler and just as
    // correct here: since this box's own content is entirely generated
    // by this same script, independently regex-matching the same
    // http(s) URLs DetectUrls would find, directly against Text, avoids
    // needing to query native formatting at all.
    static readonly System.Text.RegularExpressions.Regex UrlPattern =
        new System.Text.RegularExpressions.Regex(@"https?://[^\s)]+", System.Text.RegularExpressions.RegexOptions.Compiled);
    protected override void WndProc(ref Message m) {
        const int WM_SETCURSOR = 0x0020;
        if (m.Msg == WM_SETCURSOR && HandCursorHandle != IntPtr.Zero) {
            Point pos = PointToClient(Cursor.Position);
            int idx = GetCharIndexFromPosition(pos);
            string text = Text;
            if (idx >= 0 && idx < text.Length) {
                foreach (System.Text.RegularExpressions.Match match in UrlPattern.Matches(text)) {
                    if (idx >= match.Index && idx < match.Index + match.Length) {
                        SetCursor(HandCursorHandle);
                        m.Result = (IntPtr)1;
                        return;
                    }
                }
            }
        }
        base.WndProc(ref m);
    }
}
"@
# DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4. Wrapped in try/catch —
# this API doesn't exist pre-Windows 10 1703, and failing to set it should
# degrade to the old blurry-but-working behavior, never crash the GUI.
try { [NativeDpi]::SetProcessDpiAwarenessContext([IntPtr]::new(-4)) | Out-Null } catch {}
[System.Windows.Forms.Application]::EnableVisualStyles()
[System.Windows.Forms.Application]::SetCompatibleTextRenderingDefault($false)

# ⚠️ REAL bug, reported directly: "the tiny hand when hovering over
# buttons... needs to be normal sized." [System.Windows.Forms.Cursors]::Hand
# is NOT the real OS pointer — it's a small, fixed-resolution cursor
# bitmap baked into System.Windows.Forms.dll itself, authored for 96 DPI,
# that does NOT participate in the DPI-aware cursor scaling Windows
# applies to its own native/system cursors. On a scaled display (this
# machine measures 200%) it renders visibly smaller than the plain arrow
# cursor sitting right next to it, which is the real OS cursor and DOES
# scale correctly. Fixed by loading the actual system hand cursor
# directly via user32's LoadCursor(NULL, IDC_HAND) instead of using the
# WinForms-bundled one — this is a real native OS cursor handle, so it
# scales exactly like the arrow cursor does. Falls back to the old
# (small but functional) Cursors.Hand if LoadCursor ever fails for any
# reason, rather than leaving buttons with no hand cursor at all.
try {
    $IDC_HAND = 32649
    $realHandCursor = New-Object System.Windows.Forms.Cursor([NativeCursor]::LoadCursor([IntPtr]::Zero, $IDC_HAND))
} catch {
    $realHandCursor = [System.Windows.Forms.Cursors]::Hand
}
# Same real cursor handle, handed to HandCursorRichTextBox's own
# WM_SETCURSOR override (see its class comment above) so the log's link
# hover matches the buttons' hover exactly, not a second separate fix.
[HandCursorRichTextBox]::HandCursorHandle = $realHandCursor.Handle

$apiBase = "http://127.0.0.1:${GUI_PORT}"

# ── Windows 11-ish palette ──────────────────────────────────────────
# Plain neutrals + one accent, matching the modern Settings-app look
# (light "Mica" gray page, white cards, a single blue accent) rather
# than the flat all-white/all-default-gray dialog this window used to
# be. Kept as named variables, not scattered literals, so the whole
# look can be re-tuned from one place.
$colorBg      = [System.Drawing.Color]::FromArgb(243, 243, 243)
$colorCard    = [System.Drawing.Color]::White
$colorBorder  = [System.Drawing.Color]::FromArgb(229, 229, 229)
$colorText    = [System.Drawing.Color]::FromArgb(32, 32, 32)
$colorSubtext = [System.Drawing.Color]::FromArgb(96, 96, 96)
$colorAccent  = [System.Drawing.Color]::FromArgb(0, 103, 192)
$colorAccentHover = [System.Drawing.Color]::FromArgb(16, 119, 209)
$colorAccentDown  = [System.Drawing.Color]::FromArgb(0, 89, 165)
$colorGood    = [System.Drawing.Color]::FromArgb(16, 124, 16)
$colorWarn    = [System.Drawing.Color]::FromArgb(157, 93, 0)
$colorBad     = [System.Drawing.Color]::FromArgb(196, 43, 28)
$colorBadBg   = [System.Drawing.Color]::FromArgb(253, 236, 234)
$colorBadBgHover = [System.Drawing.Color]::FromArgb(250, 219, 216)
$colorMuted   = [System.Drawing.Color]::FromArgb(120, 120, 120)

# Fonts — "Segoe UI Variable" is the real Windows 11 system font (Settings,
# Notepad, every restyled inbox app); "Segoe UI" (Windows 10-era) is the
# fallback. .NET's Font constructor substitutes gracefully on its own if
# a named family isn't installed (older Windows builds), so no separate
# availability check/try-catch is needed here — worst case on an old
# machine it silently lands on the same Segoe UI as before.
$fontDisplay = New-Object System.Drawing.Font("Segoe UI Variable Display", 15, [System.Drawing.FontStyle]::Bold)
$fontHeading = New-Object System.Drawing.Font("Segoe UI Variable Text", 11, [System.Drawing.FontStyle]::Bold)
$fontBody    = New-Object System.Drawing.Font("Segoe UI Variable Text", 9.5)
$fontSmall   = New-Object System.Drawing.Font("Segoe UI Variable Small", 9)
$fontMono    = New-Object System.Drawing.Font("Cascadia Mono", 9.5)

# WinForms has no native border-radius — this is the standard way to
# fake one: clip a control to a rounded-rectangle Region. Used on the
# status card and both buttons below instead of the sharp 90s-dialog
# rectangles this window used to have. Radius bumped 6-8px -> 10-12px
# this pass for a visibly softer, more Windows-11 (vs. Windows-8-tile)
# feel per follow-up feedback.
# Shared by Set-RoundedRegion (the clip) and the Uninstall button's own
# hand-drawn border (see below) — both need the EXACT same geometry, or
# a border drawn to plain rectangle bounds visibly clashes with a
# region clipped to rounded corners (see that button's own comment).
function Get-RoundedPath($w, $h, $radius) {
    $d = $radius * 2
    if ($w -lt $d) { $d = $w }
    if ($h -lt $d) { $d = $h }
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($w - $d, 0, $d, $d, 270, 90)
    $path.AddArc($w - $d, $h - $d, $d, $d, 0, 90)
    $path.AddArc(0, $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}
function Set-RoundedRegion($ctrl, $radius) {
    $ctrl.Region = New-Object System.Drawing.Region((Get-RoundedPath $ctrl.Width $ctrl.Height $radius))
}

# Fetches JSON over loopback HTTP and decodes it as UTF-8 explicitly.
# NOT Invoke-RestMethod: its fallback text-encoding when a response
# doesn't pin one down is ambiguous and version/locale-dependent, and
# that ambiguity is exactly what was turning this agent's real "-"
# (em dash) characters in agent.log into garbled "a-circumflex"-style
# text in this window — the log content itself was always correct
# UTF-8, only how this window decoded it wasn't. WebClient with an
# explicit Encoding removes that ambiguity outright.
function Invoke-JsonUtf8($uri) {
    $wc = New-Object System.Net.WebClient
    $wc.Encoding = [System.Text.Encoding]::UTF8
    $raw = $wc.DownloadString($uri)
    return $raw | ConvertFrom-Json
}

# Shortens a log line's own leading ISO-8601 timestamp to a short local
# time for DISPLAY only — per explicit request ("the date/time is a
# little long"). agent.log itself keeps the full ISO timestamp
# unchanged (still needed there for real diagnosis/sorting — see
# CLAUDE.md's log() rule); this only reformats what this window shows.
function Format-LogLine($line) {
    if ($line -match '^\\[([0-9T:.\\-]+Z)\\](.*)$') {
        try {
            $dt = [DateTime]::Parse($Matches[1], [System.Globalization.CultureInfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::RoundtripKind)
            return "[" + $dt.ToLocalTime().ToString("h:mm:ss tt") + "]" + $Matches[2]
        } catch { return $line }
    }
    return $line
}

# ⚠️ REAL bug, found via FOUR separate live screenshot rounds against a
# real 200%-scaled display, not reasoned about in the abstract — every
# one of WinForms' own built-in auto-scaling mechanisms turned out to
# be unreliable here, so this window computes and applies DPI scaling
# itself instead of trusting any of them:
#   1. No AutoScale property set at all -> tiny, blurry (the original
#      bug this whole pass exists to fix) — SetProcessDpiAwarenessContext
#      alone stops the OS's blurry bitmap-stretch, but does nothing to
#      grow the hardcoded pixel Location/Size values to compensate, so
#      text renders crisp but boxes stay tiny.
#   2. AutoScaleMode = Dpi (no explicit AutoScaleDimensions) -> content
#      rendered shifted/clipped (confirmed via screenshot).
#   3. AutoScaleMode = None -> labels/cards overlapped (confirmed via
#      screenshot, and independently by the user's own screenshot of
#      the exact same bug).
#   4. AutoScaleDimensions=(96,96) + AutoScaleMode=Dpi (the textbook
#      WinForms-designer-generated combination) + an explicit
#      PerformAutoScale() call after every control was added -> STILL
#      only the Form's own outer Size scaled; every child control
#      stayed at its original tiny size (confirmed via screenshot:
#      correctly-sized window, all content still crammed tiny in one
#      corner). This is a known rough edge of .NET Framework WinForms'
#      (not .NET/.NET-Core's newer WinForms) AutoScale machinery when
#      combined with true Per-Monitor-V2 process DPI awareness — the
#      two were never really designed to cooperate, and PowerShell 5.1
#      hosts the older .NET Framework CLR, not the improved modern one.
# The actual, reliable fix: don't use AutoScale at all. Measure the
# REAL current DPI ourselves (Graphics.DpiX against the desktop),
# compute a plain scale factor against a 96-DPI design baseline, and
# multiply every hardcoded pixel Location/Size by it directly via the
# S()/Pt()/Sz() helpers below. Font point sizes are NOT scaled this way
# — GDI already renders point-sized fonts at the correct physical size
# once the process is genuinely DPI-aware, confirmed by every one of
# the screenshots above already showing correctly large, crisp text.
$measureGfx = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero)
$dpiScale = $measureGfx.DpiX / 96.0
$measureGfx.Dispose()
function S([int]$n) { return [int]([math]::Round($n * $dpiScale)) }
function Pt([int]$x, [int]$y) { return New-Object System.Drawing.Point((S $x), (S $y)) }
function Sz([int]$w, [int]$h) { return New-Object System.Drawing.Size((S $w), (S $h)) }

$form = New-Object System.Windows.Forms.Form
$form.Text = "Greenpower Receiver Agent"
$form.Size = Sz 640 700
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.BackColor = $colorBg
$form.Font = $fontBody
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::None
$form.Icon = [System.Drawing.SystemIcons]::Application

# Best-effort Mica backdrop + rounded window corners — the two most
# visually distinctive Windows 11 chrome details. Windows 11 only
# (22621+); wrapped in try/catch since DwmSetWindowAttribute simply
# fails (not throws, but caught defensively anyway) for an unknown
# attribute on older Windows — the window still looks correct without
# it, just without this one extra polish layer. Safe to force the
# handle into existence here now — with AutoScale off entirely, there's
# no implicit scale-pass timing left to disturb.
try {
    $hwnd = $form.Handle
    $DWMWA_WINDOW_CORNER_PREFERENCE = 33; $cornerPref = 2   # DWMWCP_ROUND
    [NativeDwm]::DwmSetWindowAttribute($hwnd, $DWMWA_WINDOW_CORNER_PREFERENCE, [ref]$cornerPref, 4) | Out-Null
    $DWMWA_SYSTEMBACKDROP_TYPE = 38; $backdropType = 2      # DWMSBT_MAINWINDOW (Mica)
    [NativeDwm]::DwmSetWindowAttribute($hwnd, $DWMWA_SYSTEMBACKDROP_TYPE, [ref]$backdropType, 4) | Out-Null
} catch {}

$lblTitle = New-Object System.Windows.Forms.Label
$lblTitle.Location = Pt 24 20
$lblTitle.Size = Sz 560 32
$lblTitle.Text = "Greenpower Receiver Agent"
$lblTitle.Font = $fontDisplay
$lblTitle.ForeColor = $colorText
$form.Controls.Add($lblTitle)

$lblVersion = New-Object System.Windows.Forms.Label
$lblVersion.Location = Pt 24 54
$lblVersion.Size = Sz 560 18
$lblVersion.Text = "Version (loading...)"
$lblVersion.Font = $fontSmall
$lblVersion.ForeColor = $colorSubtext
$form.Controls.Add($lblVersion)

# ── Status card ───────────────────────────────────────────────────
$card = New-Object System.Windows.Forms.Panel
$card.Location = Pt 24 86
$card.Size = Sz 576 128
$card.BackColor = $colorCard
$form.Controls.Add($card)
Set-RoundedRegion $card (S 10)

$lblForwarding = New-Object System.Windows.Forms.Label
$lblForwarding.Location = Pt 20 16
$lblForwarding.Size = Sz 536 24
$lblForwarding.Text = "Not connected"
$lblForwarding.Font = $fontHeading
$lblForwarding.ForeColor = $colorMuted
$card.Controls.Add($lblForwarding)

$sep = New-Object System.Windows.Forms.Panel
$sep.Location = Pt 20 48
$sep.Size = Sz 536 1
$sep.BackColor = $colorBorder
$card.Controls.Add($sep)

$lblTarget = New-Object System.Windows.Forms.Label
$lblTarget.Location = Pt 20 60
$lblTarget.Size = Sz 536 20
$lblTarget.Text = "Dashboard: (loading...)"
$lblTarget.Font = $fontBody
$lblTarget.ForeColor = $colorText
$card.Controls.Add($lblTarget)

$lblUpdate = New-Object System.Windows.Forms.Label
$lblUpdate.Location = Pt 20 86
$lblUpdate.Size = Sz 536 20
$lblUpdate.Text = "Update status: (loading...)"
$lblUpdate.Font = $fontBody
$lblUpdate.ForeColor = $colorText
$card.Controls.Add($lblUpdate)

# ── Log ───────────────────────────────────────────────────────────
# "(latest 150 lines)" removed from the label per explicit request —
# unnecessary detail; the log still only ever holds the latest 150
# lines underneath (readLogTail(150) on the agent side, unchanged),
# just not called out in the UI anymore.
$lblLog = New-Object System.Windows.Forms.Label
$lblLog.Location = Pt 24 226
$lblLog.Size = Sz 300 20
$lblLog.Text = "Activity log"
$lblLog.Font = $fontHeading
$lblLog.ForeColor = $colorText
$form.Controls.Add($lblLog)

# Copies the last 50 lines to the clipboard — per explicit request, so
# a user can paste recent activity into a support message without
# having to manually select/scroll inside the log box (selecting text
# there and Ctrl+C already worked, since ReadOnly only blocks editing,
# not selection — this is purely a one-click convenience on top of
# that, not a fix for a missing capability). Deliberately the last 50,
# not the full possible 150 — recent activity is almost always what's
# actually relevant to a support conversation, and a shorter paste is
# easier for someone else to read through.
# Borderless, same reasoning as the Uninstall button's own fix above —
# a FlatAppearance border traces the button's original RECTANGULAR
# bounds and visibly clashes with Set-RoundedRegion's rounded clip
# (confirmed here too: with the border on, the rounding was barely
# perceptible against the white background). White-on-light-gray
# already gives enough contrast against the page background without
# needing a border at all.
$btnCopyLog = New-Object System.Windows.Forms.Button
$btnCopyLog.Text = "Copy"
$btnCopyLog.Location = Pt 500 220
$btnCopyLog.Size = Sz 100 26
$btnCopyLog.FlatStyle = "Flat"
$btnCopyLog.FlatAppearance.BorderSize = 0
$btnCopyLog.FlatAppearance.MouseOverBackColor = $colorBg
$btnCopyLog.BackColor = [System.Drawing.Color]::White
$btnCopyLog.ForeColor = $colorText
$btnCopyLog.Font = $fontSmall
$btnCopyLog.Cursor = $realHandCursor
# One shared, reused Timer for the "Copied!" revert — not a fresh Timer
# per click — same reuse idiom this project's own dashboard JS already
# uses for a revert-after-delay (see telemetry_web's showExitToast).
$copyResetTimer = New-Object System.Windows.Forms.Timer
$copyResetTimer.Interval = 1500
$copyResetTimer.Add_Tick({ $btnCopyLog.Text = "Copy"; $copyResetTimer.Stop() })
$btnCopyLog.Add_Click({
    $recent = $lastLogLines | Select-Object -Last 50
    $text = ($recent | ForEach-Object { Format-LogLine $_ }) -join "\`r\`n"
    try {
        [System.Windows.Forms.Clipboard]::SetText($text)
        $btnCopyLog.Text = "Copied!"
        $copyResetTimer.Stop()
        $copyResetTimer.Start()
    } catch {}
})
$form.Controls.Add($btnCopyLog)
Set-RoundedRegion $btnCopyLog (S 6)

# HandCursorRichTextBox (see class comment above), not a plain
# RichTextBox — the only way to color individual lines by log level
# (see Add-LogLine below) without hand-rolling a custom-drawn list, AND
# the only way to get a correctly-sized hover cursor over its
# auto-detected links. This is also what actually fixes the "buggy
# looking" garbled characters report: Invoke-JsonUtf8 above decodes
# the log text correctly before it ever reaches this control.
$rtbLog = New-Object HandCursorRichTextBox
$rtbLog.Location = Pt 24 250
$rtbLog.Size = Sz 576 318
$rtbLog.ReadOnly = $true
$rtbLog.WordWrap = $false
$rtbLog.ScrollBars = "Both"
$rtbLog.BorderStyle = "FixedSingle"
$rtbLog.BackColor = [System.Drawing.Color]::White
# HideSelection defaults to true on every TextBoxBase-derived control —
# a selection is visually hidden (grayed out to invisible against a
# white background) the instant focus moves anywhere else, e.g. to the
# Copy button right next to this box. That reads exactly like "my
# highlight got removed" even independent of the rebuild-preservation
# fix in Refresh-Status below, so both are needed together.
$rtbLog.HideSelection = $false
$rtbLog.Font = $fontMono
# ⚠️ REAL bug, reported directly: "when I click on links you need to
# make it so they open." DetectUrls (on by default) only ever
# auto-FORMATS a detected URL (blue, underlined) — RichTextBox never
# opens anything on click by itself; that requires handling
# LinkClicked explicitly, which this control never did until now.
$rtbLog.Add_LinkClicked({
    param($sender, $e)
    try { Start-Process $e.LinkText } catch {}
})
$form.Controls.Add($rtbLog)

# ── Buttons ───────────────────────────────────────────────────────
# Hover/pressed FlatAppearance colors added this pass — a real Windows
# 11 button visibly reacts to the pointer; a flat, static-colored
# button (the previous version) is part of what read as "not fully
# modern" even with rounded corners.
$btnUpdate = New-Object System.Windows.Forms.Button
$btnUpdate.Text = "Check for Updates"
$btnUpdate.Location = Pt 24 592
$btnUpdate.Size = Sz 210 38
$btnUpdate.FlatStyle = "Flat"
$btnUpdate.FlatAppearance.BorderSize = 0
$btnUpdate.FlatAppearance.MouseOverBackColor = $colorAccentHover
$btnUpdate.FlatAppearance.MouseDownBackColor = $colorAccentDown
$btnUpdate.BackColor = $colorAccent
$btnUpdate.ForeColor = [System.Drawing.Color]::White
$btnUpdate.Font = $fontBody
$btnUpdate.Cursor = $realHandCursor
$btnUpdate.Add_Click({
    $lblUpdate.Text = "Update status: checking..."
    try { Invoke-RestMethod -Uri "$apiBase/api/check-update" -Method Post -TimeoutSec 5 | Out-Null } catch {}
})
$form.Controls.Add($btnUpdate)
Set-RoundedRegion $btnUpdate (S 8)

# ⚠️ REAL bug, reported directly: "the uninstall button has a weird
# line around it with cutoff corners." Root cause: FlatAppearance's
# BorderSize/BorderColor draws a plain RECTANGULAR outline around the
# button's original bounds, but Set-RoundedRegion (below) clips the
# button to a ROUNDED-rectangle Region — the two don't compose: the
# straight-edged border gets clipped unevenly by the rounded region,
# reading exactly as "a weird line with cutoff corners." Rather than
# hand-draw a custom border that exactly traces the rounded clip path
# (fragile — a 1px pen stroke drawn ON a region's own boundary tends to
# get half-clipped too, producing a faint/uneven line of its own), this
# button is now borderless, matching the primary button's clean flat
# style, differentiated by a light red-tinted fill instead of an
# outline — a common, simpler "danger/secondary" treatment that has no
# border to clash with the rounded clip in the first place.
$btnUninstall = New-Object System.Windows.Forms.Button
$btnUninstall.Text = "Uninstall"
$btnUninstall.Location = Pt 394 592
$btnUninstall.Size = Sz 206 38
$btnUninstall.FlatStyle = "Flat"
$btnUninstall.FlatAppearance.BorderSize = 0
$btnUninstall.FlatAppearance.MouseOverBackColor = $colorBadBgHover
$btnUninstall.BackColor = $colorBadBg
$btnUninstall.ForeColor = $colorBad
$btnUninstall.Font = $fontBody
$btnUninstall.Cursor = $realHandCursor
$btnUninstall.Add_Click({
    $result = [System.Windows.Forms.MessageBox]::Show(
        "This will completely remove the Greenpower Receiver Agent from this computer, including all files and settings. Continue?",
        "Uninstall Greenpower Receiver Agent",
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Warning)
    if ($result -eq [System.Windows.Forms.DialogResult]::Yes) {
        try { Invoke-RestMethod -Uri "$apiBase/api/uninstall" -Method Post -TimeoutSec 5 | Out-Null } catch {}
        [System.Windows.Forms.MessageBox]::Show(
            "Uninstalling in the background. This window and the agent will now close.",
            "Greenpower Receiver Agent") | Out-Null
        $form.Close()
    }
})
$form.Controls.Add($btnUninstall)
Set-RoundedRegion $btnUninstall (S 8)

# Appends one line to the log with a color picked from its [LEVEL]
# tag — turns the log from a flat wall of text into something
# scannable at a glance (errors jump out red, a found receiver jumps
# out green), the same way a real log viewer would. Timestamp shortened
# via Format-LogLine before display (see its own comment above).
function Add-LogLine($line) {
    $color = $colorText
    if ($line -match '\\[ERROR\\]') { $color = $colorBad }
    elseif ($line -match '\\[WARN\\]') { $color = $colorWarn }
    elseif ($line -match '\\[FOUND\\]') { $color = $colorGood }
    elseif ($line -match '\\[OK\\]') { $color = $colorGood }
    elseif ($line -match '\\[DEBUG\\]') { $color = $colorAccent }
    $rtbLog.SelectionStart = $rtbLog.TextLength
    $rtbLog.SelectionLength = 0
    $rtbLog.SelectionColor = $color
    $rtbLog.AppendText((Format-LogLine $line) + "\`r\`n")
}

$lastLogText = ""
$lastLogLines = @()   # raw (unformatted) lines from the last successful /api/log fetch — what the Copy button pulls its last-50 from

function Refresh-Status {
    try {
        $status = Invoke-JsonUtf8("$apiBase/api/status")
        $lblVersion.Text = "Version " + $status.version
        if ($status.forwarding.active) {
            if ($status.forwarding.confirmed) {
                $lblForwarding.Text = "Connected - forwarding via " + $status.forwarding.port
                $lblForwarding.ForeColor = $colorGood
            } else {
                $lblForwarding.Text = "Connecting... (" + $status.forwarding.port + ")"
                $lblForwarding.ForeColor = $colorWarn
            }
        } else {
            $lblForwarding.Text = "Not connected"
            $lblForwarding.ForeColor = $colorMuted
        }
        $lblTarget.Text = "Dashboard: " + $status.websiteUrl
        $u = $status.update
        # Checked FIRST, ahead of every other branch — per explicit
        # request ("it should open the GUI back up and show successful
        # update"). This window is either the SAME one that was open
        # during the update (which just spent a stretch showing "agent
        # not responding" while the old process was gone) or a brand
        # new one the agent auto-opened itself right after restarting —
        # either way, a distinct, obviously-different green success line
        # here is what actually confirms the update completed, instead
        # of just quietly reverting to a normal status line that looks
        # no different from any other check.
        if ($u.justUpdatedTo) {
            $lblUpdate.Text = "Successfully updated to " + $u.justUpdatedTo
            $lblUpdate.ForeColor = $colorGood
        } elseif ($u.installing) {
            $lblUpdate.Text = "Update status: installing update..."
            $lblUpdate.ForeColor = $colorText
        } elseif ($u.checking) {
            $lblUpdate.Text = "Update status: checking..."
            $lblUpdate.ForeColor = $colorText
        } elseif ($u.updateAvailable) {
            $lblUpdate.Text = "Update status: update available (" + $u.latestVersion + ")"
            $lblUpdate.ForeColor = $colorText
        } elseif ($u.lastCheckedAt) {
            $lblUpdate.Text = "Update status: up to date (checked " + ([DateTime]$u.lastCheckedAt).ToLocalTime().ToString("t") + ")"
            $lblUpdate.ForeColor = $colorText
        } else {
            $lblUpdate.Text = "Update status: not checked yet"
            $lblUpdate.ForeColor = $colorText
        }
    } catch {
        $lblVersion.Text = "Version (agent not responding)"
    }
    try {
        $logResp = Invoke-JsonUtf8("$apiBase/api/log")
        $newText = [string]::Join("\`n", $logResp.lines)
        # ⚠️ REAL bug, confirmed two ways: (1) reported directly — "the
        # copy button does not copy the last 50 activity lines"; (2)
        # reported separately — "[selecting] text in the log is buggy,
        # it updates and removes any highlight." Same root cause for
        # both: a plain lastLogText/lastLogLines ASSIGNMENT made INSIDE
        # this function creates a new variable scoped to THIS FUNCTION
        # CALL ONLY — PowerShell assignment never writes through to an
        # outer/script-scope variable of the same name unless told to.
        # So the script-scope lastLogLines the Copy button reads was
        # never actually being updated (always empty), AND the
        # not-equal comparison below was comparing against a
        # script-scope lastLogText value that likewise never changed
        # from its initial empty string — meaning it was ALWAYS true,
        # and the log box was being torn down and fully rebuilt on
        # literally EVERY 3-second tick regardless of whether anything
        # actually changed, wiping out any in-progress text selection
        # every single time. The script: scope modifier below makes
        # both assignments actually persist where they need to.
        if ($newText -ne $script:lastLogText) {
            $script:lastLogText = $newText
            $script:lastLogLines = $logResp.lines
            # Preserve the user's selection/scroll position across a
            # real rebuild instead of always yanking to the bottom —
            # per the same report above. If there's an active selection
            # (the user is trying to read/copy something), restore that
            # exact range afterward instead of touching scroll at all.
            # If there's no selection and the caret was already at the
            # very end (the normal "just watching it scroll" case),
            # keep auto-following to the bottom as before. Otherwise
            # (caret parked elsewhere, no selection — e.g. mid-scroll
            # reading older lines) leave the view alone.
            $savedStart = $rtbLog.SelectionStart
            $savedLength = $rtbLog.SelectionLength
            $wasAtBottom = ($savedLength -eq 0) -and ($savedStart -ge $rtbLog.TextLength - 1)
            # WM_SETREDRAW off/on around the whole Clear()+refill —
            # eliminates the visible blank-flash a bare Clear() causes
            # (see NativeRedraw's own comment above). Invalidate() at the
            # end forces one single real repaint of the final content,
            # since turning WM_SETREDRAW back on alone doesn't itself
            # trigger a repaint.
            $WM_SETREDRAW = 0x000B
            [NativeRedraw]::SendMessage($rtbLog.Handle, $WM_SETREDRAW, $false, 0) | Out-Null
            $rtbLog.Clear()
            foreach ($line in $logResp.lines) { Add-LogLine $line }
            if ($savedLength -gt 0) {
                $maxStart = [Math]::Max(0, $rtbLog.TextLength - 1)
                $restoredStart = [Math]::Min($savedStart, $maxStart)
                $restoredLength = [Math]::Min($savedLength, $rtbLog.TextLength - $restoredStart)
                $rtbLog.Select($restoredStart, $restoredLength)
            } elseif ($wasAtBottom) {
                $rtbLog.SelectionStart = $rtbLog.TextLength
                $rtbLog.ScrollToCaret()
            }
            [NativeRedraw]::SendMessage($rtbLog.Handle, $WM_SETREDRAW, $true, 0) | Out-Null
            $rtbLog.Invalidate()
        }
    } catch {}
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 3000
$timer.Add_Tick({ Refresh-Status })
$timer.Start()

Refresh-Status
[System.Windows.Forms.Application]::Run($form)
`.trim();
}

function guiPageHtml() {
    return `<!doctype html>
<html><head><meta charset="utf-8"><title>Greenpower Receiver Agent</title>
<style>
  body { font-family: -apple-system, Segoe UI, Arial, sans-serif; background:#0b0c11; color:#e6e6e6; margin:0; padding:24px; }
  h1 { font-size:18px; margin:0 0 4px; }
  .ver { color:#8a8f98; font-size:12px; margin-bottom:20px; }
  .card { background:#171719; border:1px solid #2a2b30; border-radius:8px; padding:16px; margin-bottom:16px; }
  .row { display:flex; justify-content:space-between; padding:4px 0; font-size:13px; }
  .row .k { color:#8a8f98; }
  .dot { display:inline-block; width:8px; height:8px; border-radius:50%; margin-right:6px; }
  .dot.on { background:#3ecf5e; } .dot.off { background:#6b6f76; }
  pre#log { background:#0b0c11; border:1px solid #2a2b30; border-radius:6px; padding:12px; height:320px; overflow-y:auto; font-size:12px; line-height:1.5; white-space:pre-wrap; word-break:break-all; }
  button { background:#2a2b30; color:#e6e6e6; border:1px solid #3a3b42; border-radius:6px; padding:8px 14px; font-size:13px; cursor:pointer; margin-right:8px; }
  button:hover { background:#34353c; }
  button.danger { background:#3a1c1c; border-color:#5c2626; color:#ff9a9a; }
  button.danger:hover { background:#4a2222; }
  #msg { font-size:13px; margin-top:10px; }
</style></head>
<body>
  <h1>Greenpower Receiver Agent</h1>
  <div class="ver">v${AGENT_VERSION}</div>

  <div class="card">
    <div class="row"><span class="k">Status</span><span><span class="dot on"></span>Running</span></div>
    <div class="row"><span class="k">Forwarding</span><span id="fwd">—</span></div>
    <div class="row"><span class="k">Dashboard target</span><span>${WEBSITE_URL}</span></div>
    <div class="row"><span class="k">Started</span><span>${guiState.startedAt}</span></div>
    <div class="row"><span class="k">Update status</span><span id="upd">—</span></div>
  </div>

  <div class="card">
    <button onclick="checkUpdate()">Check for Updates Now</button>
    <button class="danger" onclick="doUninstall()">Uninstall</button>
    <div id="msg"></div>
  </div>

  <div class="card">
    <div class="row"><span class="k">Log (latest 150 lines)</span><span></span></div>
    <pre id="log">loading…</pre>
  </div>

<script>
async function refresh() {
  try {
    const r = await fetch('/api/status'); const s = await r.json();
    document.getElementById('fwd').textContent = s.forwarding.active
      ? (s.forwarding.confirmed ? ('Yes — ' + s.forwarding.port) : ('Connecting… — ' + s.forwarding.port))
      : 'No';
    const u = s.update;
    document.getElementById('upd').textContent = u.installing ? 'Installing update…'
      : u.checking ? 'Checking…'
      : u.updateAvailable ? ('Update available: ' + u.latestVersion)
      : (u.lastCheckedAt ? ('Up to date (checked ' + new Date(u.lastCheckedAt).toLocaleTimeString() + ')') : 'Not checked yet');
  } catch (e) {}
  try {
    const r2 = await fetch('/api/log'); const j = await r2.json();
    const pre = document.getElementById('log');
    const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 10;
    pre.textContent = j.lines.join('\\n');
    if (atBottom) pre.scrollTop = pre.scrollHeight;
  } catch (e) {}
}
async function checkUpdate() {
  document.getElementById('msg').textContent = 'Checking for updates…';
  await fetch('/api/check-update', { method: 'POST' });
  setTimeout(refresh, 1000);
}
async function doUninstall() {
  if (!confirm('This will completely remove the Greenpower Receiver Agent from this computer, including all files and settings. Continue?')) return;
  document.getElementById('msg').textContent = 'Uninstalling — this window will stop updating shortly. The agent is being removed in the background.';
  await fetch('/api/uninstall', { method: 'POST' });
}
refresh();
setInterval(refresh, 3000);
</script>
</body></html>`;
}

function startGuiServer() {
    const server = http.createServer((req, res) => {
        if (req.method === 'GET' && req.url === '/') {
            res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
            res.end(guiPageHtml());
        } else if (req.method === 'GET' && req.url === '/api/status') {
            // charset=utf-8 explicit here (and on every JSON route below) —
            // see guiWindowPs1()'s Invoke-JsonUtf8() comment for why: this
            // agent's log lines contain real non-ASCII characters (an em
            // dash, "—"), and PowerShell's Invoke-RestMethod has an
            // ambiguous fallback text-encoding when a response doesn't pin
            // one down — that's what was turning them into "â" in the GUI.
            // This header alone isn't the fix (the GUI decodes explicitly
            // now regardless), but it's the correct, spec-compliant thing
            // to send either way.
            res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
            res.end(JSON.stringify({ version: AGENT_VERSION, websiteUrl: WEBSITE_URL, ...guiState }));
        } else if (req.method === 'GET' && req.url === '/api/log') {
            res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
            res.end(JSON.stringify({ lines: readLogTail(150) }));
        } else if (req.method === 'POST' && req.url === '/api/check-update') {
            checkForUpdate(true);
            res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
            res.end(JSON.stringify({ ok: true }));
        } else if (req.method === 'POST' && req.url === '/api/uninstall') {
            res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
            res.end(JSON.stringify({ ok: true }));
            triggerUninstall();
        } else {
            res.writeHead(404);
            res.end('Not found');
        }
    });

    // Loopback-only — the second argument to listen() is the bind address;
    // omitting it would default to all interfaces, which this must never do.
    server.listen(GUI_PORT, '127.0.0.1', () => {
        log(`[OK]   GUI available at http://127.0.0.1:${GUI_PORT}/ (open via the tray icon's "Show GUI")`);
    });
    server.on('error', (e) => {
        // Non-fatal — same reasoning as the tray icon's own error handling.
        // A likely cause: another agent instance's GUI server still holds
        // the port (e.g. a previous instance the single-instance guard
        // above hasn't fully torn down yet) — the GUI is a convenience,
        // not load-bearing for actual telemetry forwarding.
        log(`[WARN] GUI server failed to start on port ${GUI_PORT}: ${e.message}`);
    });
    return server;
}

let guiServer = null;
try {
    guiServer = startGuiServer();
} catch (e) {
    log(`[WARN] Could not start GUI server (continuing without one): ${e.message}`);
}

// Consumed once per real auto-update — per explicit request ("it should
// open the GUI back up and show successful update"). See performUpdate()
// and UPDATE_SUCCESS_MARKER's own comments for why this crosses via a
// %TEMP% file rather than anything in-process: the OLD process that
// wrote it is long gone by the time this (genuinely new) process starts.
// Placed after startGuiServer() so the auto-opened window's very first
// poll has a real server to reach — a startup race here is harmless
// either way (the GUI already tolerates "agent not responding" for a
// tick or two on every normal manual open, same code path).
try {
    if (fs.existsSync(UPDATE_SUCCESS_MARKER)) {
        const marker = JSON.parse(fs.readFileSync(UPDATE_SUCCESS_MARKER, 'utf8'));
        fs.unlinkSync(UPDATE_SUCCESS_MARKER);   // consume once — a later normal restart must not keep re-announcing this
        guiState.update.justUpdatedTo = marker.version || AGENT_VERSION;
        log(`[UPDATE] Successfully updated to v${AGENT_VERSION}.`);
        // Cleared after a while so a GUI window left open long after the
        // update doesn't keep claiming "just updated" indefinitely.
        setTimeout(() => { guiState.update.justUpdatedTo = null; }, 60000);
        openNativeGuiWindow();
    }
} catch (e) {
    log(`[WARN] Couldn't process update-success marker: ${e.message}`);
}
