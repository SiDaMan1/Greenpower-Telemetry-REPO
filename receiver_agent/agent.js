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
const AGENT_VERSION = '1.4.0.0';

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

// ── Single-instance PID file ───────────────────────────────────────
// setup.bat ALSO reads this file to kill any previous hidden instance
// before starting a new one, but that only covers the "re-running
// setup.bat" path specifically. The check right below covers every other
// way a second instance could end up running at the same time — the
// Startup-folder auto-launch firing on login while a previous instance
// from before a restart/sleep is somehow still alive, someone double-
// clicking the launcher shortcut twice, running `node agent.js` manually
// while the hidden auto-started one is already up, etc. — by having the
// agent itself check on every single startup, not just when setup.bat
// happens to be the one doing the (re)launching. Left running, a second
// instance would fight the first for the same serial port — the exact
// "Access denied, looks like a hardware/driver problem but isn't" failure
// this file already has a rule about below.
const PID_PATH = path.join(__dirname, 'agent.pid');
try {
    const existingPidRaw = fs.readFileSync(PID_PATH, 'utf8').trim();
    const existingPid = parseInt(existingPidRaw, 10);
    if (existingPid && existingPid !== process.pid) {
        try {
            // Signal 0 doesn't actually send a signal — it's the standard
            // Node/POSIX idiom for "is this PID still alive", and it throws
            // (ESRCH) if not. Works on Windows too via libuv's emulation.
            process.kill(existingPid, 0);
            log(`[WARN] Another agent instance (PID ${existingPid}) is already running — terminating it so this one can take over.`);
            process.kill(existingPid, 'SIGTERM');
        } catch (e) {
            // Not alive — a stale PID file left over from a previous
            // crash/unclean exit that skipped the cleanup handlers below.
            // Nothing to do, just proceed to overwrite it with our own PID.
        }
    }
} catch (e) { /* no existing PID file — normal on first run ever */ }
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
process.on('exit', cleanupPidFile);
process.on('exit', cleanupTray);
process.on('exit', cleanupGuiServer);
process.on('SIGINT', () => { cleanupPidFile(); cleanupTray(); cleanupGuiServer(); process.exit(); });
process.on('SIGTERM', () => { cleanupPidFile(); cleanupTray(); cleanupGuiServer(); process.exit(); });

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
const UPDATE_CHECK_MS     = 6 * 60 * 60 * 1000;   // check every 6h — frequent enough to reach people quickly, rare enough not to matter for bandwidth/load
const GUI_PORT             = 47821;   // arbitrary fixed high port, loopback-only — see startGuiServer()

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
    update: { checking: false, lastCheckedAt: null, latestVersion: null, updateAvailable: false, installing: false, lastError: null },
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
                { title: 'Show GUI', tooltip: 'Open the status/log page in your browser', checked: false, enabled: true },
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
            openGuiInBrowser();
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

function openGuiInBrowser() {
    // "start" is a cmd.exe builtin, not an executable — has to go through
    // the shell. The empty "" first argument is the classic `start` quirk
    // where the first quoted argument is taken as the window TITLE, not
    // the thing to open, if the URL itself is quoted; passing an explicit
    // empty title avoids that misparse.
    execFile('cmd.exe', ['/c', 'start', '""', `http://127.0.0.1:${GUI_PORT}/`], (err) => {
        if (err) log(`[WARN] Couldn't open GUI in browser: ${err.message}`);
    });
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

// First check a short while after startup (not instantly — let the agent
// settle into its normal boot sequence first), then on a steady interval.
setTimeout(() => checkForUpdate(false), 15000);
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
' settling asynchronously, not a real lock: a manual delete of that exact
' same empty folder always succeeded instantly the moment it was actually
' tried, confirming nothing is genuinely blocking it. 60 attempts at 1s
' gives real headroom (~60s) to catch that window automatically instead
' of leaving an empty, harmless-but-visible folder icon behind for the
' user to notice and wonder about.
deleteAttempts = 0
Do While fso.FolderExists("${installFolder}") And deleteAttempts < 90
  fso.DeleteFolder "${installFolder}", True
  If fso.FolderExists("${installFolder}") Then WScript.Sleep 1000
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
//  0.0.0.0 — this must never be reachable from the network, only from a
//  browser on the same machine, opened via the tray's "Show GUI" item.
// ════════════════════════════════════════════════════════════════════

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
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ version: AGENT_VERSION, websiteUrl: WEBSITE_URL, ...guiState }));
        } else if (req.method === 'GET' && req.url === '/api/log') {
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ lines: readLogTail(150) }));
        } else if (req.method === 'POST' && req.url === '/api/check-update') {
            checkForUpdate(true);
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ ok: true }));
        } else if (req.method === 'POST' && req.url === '/api/uninstall') {
            res.writeHead(200, { 'Content-Type': 'application/json' });
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
