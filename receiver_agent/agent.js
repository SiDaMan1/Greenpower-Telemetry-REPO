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

// ── Config ──────────────────────────────────────────────────────────
const CONFIG_PATH = path.join(__dirname, 'config.json');
let fileConfig = {};
if (fs.existsSync(CONFIG_PATH)) {
    try {
        fileConfig = JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
    } catch (e) {
        console.error(`[ERROR] config.json exists but isn't valid JSON: ${e.message}`);
    }
}

const WEBSITE_URL = process.env.WEBSITE_URL || fileConfig.websiteUrl;
const API_KEY     = process.env.TELEMETRY_API_KEY || fileConfig.apiKey;

if (!WEBSITE_URL || !API_KEY) {
    console.error('[ERROR] Missing websiteUrl/apiKey.');
    console.error('        Copy config.example.json to config.json and fill it in,');
    console.error('        or set WEBSITE_URL and TELEMETRY_API_KEY environment variables.');
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

console.log('[READY] Greenpower receiver agent running — watching for USB connections...');
console.log(`        Forwarding target: ${WEBSITE_URL}`);

setInterval(scanPorts, SCAN_MS);
scanPorts();

async function scanPorts() {
    let ports;
    try {
        ports = await SerialPort.list();
    } catch (e) {
        console.error(`[ERROR] Failed to list serial ports: ${e.message}`);
        return;
    }

    const currentPaths = new Set(ports.map(p => p.path));

    // Forget ports that disappeared, so replugging the same device re-runs
    // the identify/prompt flow instead of staying silently ignored forever.
    for (const knownPath of knownPorts.keys()) {
        if (!currentPaths.has(knownPath)) {
            console.log(`[INFO] ${knownPath} disconnected.`);
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
                console.log(`[INFO] Couldn't open ${portPath} (${err.message}) — likely in use by something else, skipping.`);
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
    console.log(`[FOUND] Greenpower receiver on ${portPath}`);

    notifier.notify(
        {
            title: 'Greenpower Receiver Connected',
            message: `Forward live telemetry from ${portPath} to the dashboard?\nClick this notification to start forwarding.`,
            wait: true,
            timeout: NOTIFY_TIMEOUT_S,
        },
        (err, response) => {
            // node-notifier's response strings vary by OS/notifier backend —
            // 'activate' (clicked) is the reliable "yes" signal on Windows.
            // Anything else (timeout, dismissed) is treated as "no".
            if (response === 'activate') {
                console.log(`[FORWARD] Starting forwarding from ${portPath}`);
                startForwarding(portPath, port);
            } else {
                console.log(`[SKIP] Not forwarding ${portPath} (no response / declined).`);
                port.close(() => {});
            }
        }
    );
}

function startForwarding(portPath, port) {
    let buffer = '';
    port.on('data', (chunk) => {
        buffer += chunk.toString('utf8');
        let idx;
        while ((idx = buffer.indexOf('\n')) !== -1) {
            const line = buffer.slice(0, idx).trim();
            buffer = buffer.slice(idx + 1);
            if (line.startsWith('JSON:')) {
                forwardLine(line.slice(5));
            }
        }
    });

    port.on('close', () => {
        console.log(`[INFO] ${portPath} closed — stopped forwarding.`);
    });
}

async function forwardLine(jsonStr) {
    let data;
    try {
        data = JSON.parse(jsonStr);
    } catch (e) {
        console.error(`[WARN] Bad JSON from receiver, skipping: ${e.message}`);
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
            console.error(`[WARN] Forward failed: HTTP ${res.status}`);
        }
    } catch (e) {
        console.error(`[WARN] Forward failed: ${e.message}`);
    }
}
