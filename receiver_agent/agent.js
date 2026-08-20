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
// ════════════════════════════════════════════════════════════════════

const fs = require('fs');
const path = require('path');
const { SerialPort } = require('serialport');
const notifier = require('node-notifier');
const SysTray = require('systray').default;

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

// ── Single-instance PID file ───────────────────────────────────────
// setup.bat reads this file to kill any previous hidden instance before
// starting a new one — otherwise re-running setup.bat piles up multiple
// node.exe processes, and an old one holding a serial port open causes a
// confusing "Access denied" on that port in the NEW instance, which looks
// like a hardware/driver problem but is actually just a stale process.
const PID_PATH = path.join(__dirname, 'agent.pid');
try { fs.writeFileSync(PID_PATH, String(process.pid)); } catch (e) { /* non-fatal */ }
function cleanupPidFile() {
    try {
        if (fs.readFileSync(PID_PATH, 'utf8').trim() === String(process.pid)) {
            fs.unlinkSync(PID_PATH);
        }
    } catch (e) { /* non-fatal — file may already be gone */ }
}
// tray is declared further down (after config load) but not referenced
// until one of these fires, by which point it's already been assigned —
// safe despite the temporal-dead-zone-looking forward reference.
function cleanupTray() {
    try { if (tray) tray.kill(false); } catch (e) { /* non-fatal */ }
}
process.on('exit', cleanupPidFile);
process.on('exit', cleanupTray);
process.on('SIGINT', () => { cleanupPidFile(); cleanupTray(); process.exit(); });
process.on('SIGTERM', () => { cleanupPidFile(); cleanupTray(); process.exit(); });

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

const DEVICE_ID   = 'GREENPOWER_RX_V1';   // must match greenpower_receiver.ino
const BAUD_RATE   = 115200;
const SCAN_MS     = 2000;    // how often to check for newly plugged-in ports
const IDENTIFY_TIMEOUT_MS = 4000;
const NOTIFY_TIMEOUT_S    = 20;   // how long the accept/decline prompt stays up

// Ports we've already looked at (identified as Greenpower / not / still
// being decided) — keyed by port path, so we don't re-prompt every scan
// tick for the same physical device.
const knownPorts = new Map();   // path -> 'pending' | 'ours' | 'not-ours'

log('[READY] Greenpower receiver agent running — watching for USB connections...');
log(`        Forwarding target: ${WEBSITE_URL}`);

// ── System tray icon ────────────────────────────────────────────────
// The agent runs with no console window at all (see setup.bat's hidden VBS
// launcher) — without this, there's no visible sign the background process
// is even alive short of opening Task Manager. A tray icon plus a one-item
// "Stop Agent" menu gives a visible "yes, it's running" indicator and an
// obvious, discoverable way to end it, without needing a console/taskbar
// window. tray-icon.ico must stay a real .ico (not .png) — Windows tray
// icons specifically expect that format; see systray's own README for why
// the format differs per-OS.
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
                { title: 'Stop Agent', tooltip: 'Stop forwarding and exit', checked: false, enabled: true },
            ],
        },
        debug: false,
        copyDir: true,
    });

    tray.onClick((action) => {
        if (action.seq_id === 1) {
            log('[INFO] Stop requested from tray icon — exiting.');
            // cleanupPidFile()/cleanupTray() both already run via the
            // process 'exit' handlers registered above — process.exit()
            // alone is enough here, no need to duplicate that cleanup.
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
        knownPorts.set(portPath, 'not-ours');
        return;
    }

    let buffer = '';
    const onData = (chunk) => {
        if (settled) return;
        buffer += chunk.toString('utf8');
        if (buffer.includes(DEVICE_ID)) {
            settled = true;
            clearTimeout(timeout);
            port.removeListener('data', onData);
            knownPorts.set(portPath, 'ours');
            promptToForward(portPath, port);
        }
    };
    port.on('data', onData);
    port.on('error', () => { /* handled by open callback / timeout */ });

    // In case the boot beacon already went by before we opened the port,
    // ask directly — the firmware answers ID? immediately either way.
    port.write('ID?\n', (err) => { /* ignore write errors, timeout covers it */ });

    const timeout = setTimeout(() => {
        if (settled) return;
        settled = true;
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
