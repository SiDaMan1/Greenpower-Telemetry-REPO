// ════════════════════════════════════════════════════════════════════
//  GREENPOWER TELEMETRY WEB
//
//  Always-on dashboard. Runs on Railway 24/7 regardless of whether any
//  receiver is plugged in anywhere. ../receiver_agent posts live packets
//  to POST /api/telemetry whenever it's actively forwarding; the browser
//  dashboard polls GET /api/latest and shows "offline" once data goes
//  stale (no update within STALE_MS).
//
//  Persistence: every packet is also written to Postgres (via DATABASE_URL,
//  which Railway auto-injects once a Postgres plugin is attached to this
//  service — see README.md). Packets are grouped into "sessions" — a new
//  session starts automatically whenever a packet arrives after a gap of
//  SESSION_GAP_MS with no data, so a drive/test session boundary doesn't
//  need any explicit start/stop signal from the agent or firmware.
//
//  Without DATABASE_URL set, the app still runs — /api/latest keeps working
//  off the in-memory value, but session storage/history/CSV export are
//  disabled rather than crashing. This matters for local dev without a
//  local Postgres instance.
// ════════════════════════════════════════════════════════════════════

const express = require('express');
const path = require('path');
const crypto = require('crypto');
const { Pool } = require('pg');

const app = express();
app.use(express.json({ limit: '10kb' }));   // one telemetry packet is well under 1kb

const PORT = process.env.PORT || 3000;
const STALE_MS = 2000;          // no update in this long = dashboard shows offline
const SESSION_GAP_MS = 60000;   // no packet in this long = next packet starts a NEW session

// ── API key ─────────────────────────────────────────────────────────
// Set TELEMETRY_API_KEY in Railway's environment variables for real use.
// Without it, a random key is generated at boot and printed once to the
// server log — fine for local testing, useless in production since nobody
// else can read Railway's log to get it. Set the env var for a real deploy.
let API_KEY = process.env.TELEMETRY_API_KEY;
if (!API_KEY) {
    API_KEY = crypto.randomBytes(24).toString('hex');
    console.warn('[WARN] TELEMETRY_API_KEY not set — generated a temporary key for this run:');
    console.warn(`        ${API_KEY}`);
    console.warn('        Set TELEMETRY_API_KEY as a real environment variable for production.');
}

// ── In-memory latest-packet store (unchanged — this is what /api/latest serves) ──
let latest = null;
let lastUpdateMs = 0;

// ── Database (optional — degrades gracefully if not configured) ──────
let pool = null;
let currentSessionId = null;
let currentSessionLastPacketMs = 0;

if (process.env.DATABASE_URL) {
    pool = new Pool({
        connectionString: process.env.DATABASE_URL,
        ssl: { rejectUnauthorized: false },   // Railway's internal Postgres needs this
    });

    pool.query(`
        CREATE TABLE IF NOT EXISTS sessions (
            id           SERIAL PRIMARY KEY,
            started_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
            ended_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
            packet_count INTEGER     NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS telemetry_points (
            id          BIGSERIAL PRIMARY KEY,
            session_id  INTEGER     NOT NULL REFERENCES sessions(id),
            received_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            data        JSONB       NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_telemetry_session     ON telemetry_points(session_id);
        CREATE INDEX IF NOT EXISTS idx_telemetry_received_at ON telemetry_points(received_at);
    `).then(() => {
        console.log('[OK]   Database schema ready');
    }).catch((e) => {
        console.error('[ERROR] Database schema init failed:', e.message);
    });
} else {
    console.warn('[WARN] DATABASE_URL not set — session history/CSV export are disabled.');
    console.warn('        Add a Postgres plugin in Railway to enable them (see README.md).');
}

// Data is stored as JSONB rather than one column per telemetry field on
// purpose — telemetry_packet_t has changed shape twice already in this
// project (ESC fields, RPM period-based rework, etc.), and JSONB means
// adding a new field upstream never requires a matching DB migration here.
// CSV export below defines its own fixed column order instead of trusting
// whatever keys happen to be present in any single row.
async function recordPoint(data) {
    if (!pool) return;   // no DB configured — silently skip, /api/latest still works

    try {
        const now = Date.now();
        const gapMs = now - currentSessionLastPacketMs;

        if (currentSessionId === null || gapMs > SESSION_GAP_MS) {
            const result = await pool.query(
                'INSERT INTO sessions (started_at, ended_at) VALUES (now(), now()) RETURNING id'
            );
            currentSessionId = result.rows[0].id;
        }
        currentSessionLastPacketMs = now;

        await pool.query(
            'INSERT INTO telemetry_points (session_id, data) VALUES ($1, $2)',
            [currentSessionId, data]
        );
        await pool.query(
            'UPDATE sessions SET ended_at = now(), packet_count = packet_count + 1 WHERE id = $1',
            [currentSessionId]
        );
    } catch (e) {
        console.error('[ERROR] Failed to record telemetry point:', e.message);
    }
}

function requireApiKey(req, res, next) {
    const auth = req.get('Authorization') || '';
    const token = auth.startsWith('Bearer ') ? auth.slice(7) : null;
    if (token !== API_KEY) {
        return res.status(401).json({ error: 'invalid or missing API key' });
    }
    next();
}

// receiver_agent posts here — one JSON body per forwarded packet
app.post('/api/telemetry', requireApiKey, (req, res) => {
    latest = req.body;
    lastUpdateMs = Date.now();
    recordPoint(req.body);   // fire-and-forget — don't make the agent wait on a DB write
    res.status(204).end();
});

// dashboard polls here — public, read-only, no auth needed
app.get('/api/latest', (req, res) => {
    const ageMs = latest ? Date.now() - lastUpdateMs : Infinity;
    res.json({
        online: ageMs < STALE_MS,
        ageMs: Number.isFinite(ageMs) ? ageMs : null,
        data: latest,
    });
});

// ── Session history (public reads — same reasoning as /api/latest: nothing
//    in a telemetry session is sensitive enough to gate behind the API key) ──

app.get('/api/sessions', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    try {
        const result = await pool.query(
            `SELECT id, started_at, ended_at, packet_count
             FROM sessions
             ORDER BY started_at DESC
             LIMIT 200`
        );
        res.json(result.rows);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

app.get('/api/sessions/:id/points', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    try {
        const result = await pool.query(
            `SELECT received_at, data
             FROM telemetry_points
             WHERE session_id = $1
             ORDER BY received_at ASC`,
            [req.params.id]
        );
        res.json(result.rows);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

// Fixed column order — deliberately not derived from whatever keys happen to
// be in the first row, so the CSV stays stable and readable even if a given
// row is missing a field (e.g. ESC data before the ESC link existed).
const CSV_COLUMNS = [
    'received_at', 'seq', 'rssi', 'snr', 'flags',
    'speed_mph', 'latitude', 'longitude', 'hdop', 'satellites',
    'temp_f', 'batt_volt', 'motor_volt', 'current_a',
    'roll_deg', 'pitch_deg', 'yaw_deg', 'accel_g', 'lateral_g', 'vertical_g',
    'motor_rpm', 'wheel_rpm',
    'esc_valid', 'esc_mode', 'esc_state', 'esc_setpoint_pct', 'esc_live_pct', 'esc_ramp_pct',
];

function csvEscape(val) {
    if (val === null || val === undefined) return '';
    const s = String(val);
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

app.get('/api/sessions/:id/export.csv', async (req, res) => {
    if (!pool) return res.status(503).send('database not configured');
    try {
        const result = await pool.query(
            `SELECT received_at, data
             FROM telemetry_points
             WHERE session_id = $1
             ORDER BY received_at ASC`,
            [req.params.id]
        );

        res.setHeader('Content-Type', 'text/csv');
        res.setHeader('Content-Disposition', `attachment; filename="session-${req.params.id}.csv"`);

        res.write(CSV_COLUMNS.join(',') + '\n');
        for (const row of result.rows) {
            const merged = { ...row.data, received_at: row.received_at.toISOString() };
            res.write(CSV_COLUMNS.map(col => csvEscape(merged[col])).join(',') + '\n');
        }
        res.end();
    } catch (e) {
        res.status(500).send(e.message);
    }
});

app.use(express.static(path.join(__dirname, 'public')));

app.listen(PORT, () => {
    console.log(`[READY] Greenpower telemetry web listening on port ${PORT}`);
});
