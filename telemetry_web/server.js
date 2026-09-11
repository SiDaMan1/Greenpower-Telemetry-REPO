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
const fs = require('fs');
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

        -- User-assigned display name for a session — NULL until someone
        -- renames it via PATCH /api/sessions/:id, in which case the UI
        -- falls back to the existing "SESSION #<id>" label. A rename is
        -- purely cosmetic, doesn't touch started_at/ended_at/packet_count.
        ALTER TABLE sessions ADD COLUMN IF NOT EXISTS name TEXT;

        -- Local sessions — telemetry recorded straight to the sender's own
        -- SD card (see greenpower_sender's initSdCard()/logToSD()), never
        -- transmitted over LoRa at all. These never touch the live
        -- /api/telemetry path or the 'sessions' table above — a live
        -- session is inferred purely from a gap in real-time packet
        -- arrival (see SESSION_GAP_MS), which doesn't apply to a file
        -- uploaded well after the fact, all at once. receiver_agent finds
        -- these by reading a physically-inserted SD card (see its own
        -- CLAUDE.md) and POSTs each LOG*.CSV file here as one local
        -- session via POST /api/local-sessions.
        CREATE TABLE IF NOT EXISTS local_sessions (
            id           SERIAL PRIMARY KEY,
            name         TEXT,
            source_file  TEXT,
            started_at   TIMESTAMPTZ,
            ended_at     TIMESTAMPTZ,
            packet_count INTEGER     NOT NULL DEFAULT 0,
            uploaded_at  TIMESTAMPTZ NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS local_session_points (
            id               BIGSERIAL PRIMARY KEY,
            local_session_id INTEGER     NOT NULL REFERENCES local_sessions(id),
            received_at      TIMESTAMPTZ NOT NULL,
            millis_ms        BIGINT,
            data             JSONB       NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_local_telemetry_session ON local_session_points(local_session_id);
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
    lastUpdateMs = Date.now();
    // received_at is stamped here (server receive time), not trusted from the
    // client — this is what the live dashboard charts plot on their x-axis,
    // separate from telemetry_points.received_at (a DB column, set at insert
    // time by Postgres itself) that sessions use. Don't pass this stamped
    // copy to recordPoint() — the DB's own column is the source of truth for
    // stored history, this one's only for the in-memory /api/latest response.
    latest = { ...req.body, received_at: lastUpdateMs };
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
            `SELECT id, name, started_at, ended_at, packet_count
             FROM sessions
             ORDER BY started_at DESC
             LIMIT 200`
        );
        res.json(result.rows);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

// Rename a session — purely cosmetic, see the `name` column's own comment
// above the CREATE TABLE up top. Public/no-auth, same reasoning as every
// other session endpoint on this page. An empty/whitespace-only name is
// stored as NULL rather than an empty string, so the UI's existing
// "SESSION #<id>" fallback kicks back in instead of showing a blank title.
app.patch('/api/sessions/:id', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    const name = typeof req.body.name === 'string' ? req.body.name.trim().slice(0, 200) : '';
    try {
        const result = await pool.query(
            'UPDATE sessions SET name = $1 WHERE id = $2 RETURNING id, name',
            [name || null, req.params.id]
        );
        if (result.rowCount === 0) return res.status(404).json({ error: 'session not found' });
        res.json(result.rows[0]);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

// Mass delete — the UI's "select sessions, delete selected" flow. Takes an
// array of ids in the body rather than looping DELETE /api/sessions/:id
// once per selection client-side, so a 40-session selection is one
// request/one transaction instead of 40. Same delete-points-then-session
// logic as the single-session DELETE endpoint below, just batched; also
// resets currentSessionId the same way if the live session happens to be
// among the ones deleted.
app.post('/api/sessions/bulk-delete', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    const ids = Array.isArray(req.body.ids) ? req.body.ids.filter(id => Number.isFinite(Number(id))) : [];
    if (ids.length === 0) return res.status(400).json({ error: 'ids must be a non-empty array' });
    const client = await pool.connect();
    try {
        await client.query('BEGIN');
        await client.query('DELETE FROM telemetry_points WHERE session_id = ANY($1::int[])', [ids]);
        const result = await client.query('DELETE FROM sessions WHERE id = ANY($1::int[])', [ids]);
        await client.query('COMMIT');
        if (ids.map(String).includes(String(currentSessionId))) {
            currentSessionId = null;
        }
        res.json({ deleted: result.rowCount });
    } catch (e) {
        await client.query('ROLLBACK').catch(() => {});
        res.status(500).json({ error: e.message });
    } finally {
        client.release();
    }
});

// Delete a session and all its points — per explicit request, a real
// destructive action exposed to the dashboard. Deliberately public/no-auth,
// same reasoning as every other session endpoint above ("nothing in a
// telemetry session is sensitive enough to gate behind the API key") — the
// UI's own confirmation modal (see index.html) is the actual safety net
// against ACCIDENTAL deletion, not server-side auth; this matches the
// honest security posture this whole app already has (see the onboarding
// password's own "UI speed-bump, not real access control" note in
// CLAUDE.md) rather than pretending a new endpoint should be held to a
// stricter standard than every read endpoint already sitting next to it.
// telemetry_points has no ON DELETE CASCADE on its session_id foreign key
// (the schema up top never declared one), so points are deleted explicitly
// first, in a transaction — if either delete fails, both roll back rather
// than leaving orphaned points or a session with no points silently gone.
app.delete('/api/sessions/:id', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    const id = req.params.id;
    const client = await pool.connect();
    try {
        await client.query('BEGIN');
        await client.query('DELETE FROM telemetry_points WHERE session_id = $1', [id]);
        const result = await client.query('DELETE FROM sessions WHERE id = $1', [id]);
        await client.query('COMMIT');
        if (result.rowCount === 0) {
            return res.status(404).json({ error: 'session not found' });
        }
        // If the session just deleted was the currently-active one (e.g.
        // someone deletes an in-progress session while the car's still
        // transmitting), reset currentSessionId so the next packet starts
        // a genuinely new session row instead of trying to insert into a
        // session id that no longer exists (which would fail the
        // telemetry_points foreign key constraint until the normal
        // SESSION_GAP_MS timeout eventually forced a new session anyway).
        if (String(currentSessionId) === String(id)) {
            currentSessionId = null;
        }
        res.status(204).end();
    } catch (e) {
        await client.query('ROLLBACK').catch(() => {});
        res.status(500).json({ error: e.message });
    } finally {
        client.release();
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
    'pitch_deg', 'accel_g', 'lateral_g', 'vertical_g',
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

// ── Local sessions (uploaded from a vehicle's SD card via receiver_agent) ──
// See the local_sessions/local_session_points CREATE TABLE comment above
// for what these are and why they're a separate table from `sessions`.
// Reuses the SAME CSV_COLUMNS/csvEscape() as live sessions below so the
// exported file has an identical shape either way — a local session's
// points just never populate seq/rssi/snr (no LoRa link involved).

// Upload one local session — called by receiver_agent, authenticated the
// same way live telemetry POSTs are (this genuinely is coming from an
// external process on someone's laptop, unlike the read/delete/rename
// endpoints above which only the dashboard's own UI calls).
app.post('/api/local-sessions', requireApiKey, async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    const { name, sourceFile, points } = req.body;
    if (!Array.isArray(points) || points.length === 0) {
        return res.status(400).json({ error: 'points must be a non-empty array' });
    }
    const client = await pool.connect();
    try {
        await client.query('BEGIN');
        const times = points.map(p => new Date(p.receivedAt)).filter(d => !isNaN(d.getTime()));
        const startedAt = times.length ? new Date(Math.min(...times)) : new Date();
        const endedAt = times.length ? new Date(Math.max(...times)) : new Date();
        const sessionResult = await client.query(
            `INSERT INTO local_sessions (name, source_file, started_at, ended_at, packet_count)
             VALUES ($1, $2, $3, $4, $5) RETURNING id`,
            [(name || sourceFile || 'Local session').slice(0, 200), sourceFile || null, startedAt, endedAt, points.length]
        );
        const sessionId = sessionResult.rows[0].id;

        // Bulk insert via a single multi-row statement rather than one
        // INSERT per point — a full session can be thousands of rows, and
        // this is a single, infrequent upload (not the hot 5Hz live path),
        // so it's worth batching properly rather than looping awaits.
        const values = [];
        const placeholders = [];
        points.forEach((p, i) => {
            const base = i * 4;
            placeholders.push(`($${base + 1}, $${base + 2}, $${base + 3}, $${base + 4})`);
            const d = new Date(p.receivedAt);
            values.push(sessionId, isNaN(d.getTime()) ? new Date() : d, p.millisMs ?? null, p.data || {});
        });
        await client.query(
            `INSERT INTO local_session_points (local_session_id, received_at, millis_ms, data) VALUES ${placeholders.join(',')}`,
            values
        );
        await client.query('COMMIT');
        res.status(201).json({ id: sessionId, packetCount: points.length });
    } catch (e) {
        await client.query('ROLLBACK').catch(() => {});
        res.status(500).json({ error: e.message });
    } finally {
        client.release();
    }
});

app.get('/api/local-sessions', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    try {
        const result = await pool.query(
            `SELECT id, name, source_file, started_at, ended_at, packet_count, uploaded_at
             FROM local_sessions
             ORDER BY started_at DESC NULLS LAST
             LIMIT 200`
        );
        res.json(result.rows);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

app.patch('/api/local-sessions/:id', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    const name = typeof req.body.name === 'string' ? req.body.name.trim().slice(0, 200) : '';
    try {
        const result = await pool.query(
            'UPDATE local_sessions SET name = $1 WHERE id = $2 RETURNING id, name',
            [name || null, req.params.id]
        );
        if (result.rowCount === 0) return res.status(404).json({ error: 'local session not found' });
        res.json(result.rows[0]);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

app.delete('/api/local-sessions/:id', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    const client = await pool.connect();
    try {
        await client.query('BEGIN');
        await client.query('DELETE FROM local_session_points WHERE local_session_id = $1', [req.params.id]);
        const result = await client.query('DELETE FROM local_sessions WHERE id = $1', [req.params.id]);
        await client.query('COMMIT');
        if (result.rowCount === 0) return res.status(404).json({ error: 'local session not found' });
        res.status(204).end();
    } catch (e) {
        await client.query('ROLLBACK').catch(() => {});
        res.status(500).json({ error: e.message });
    } finally {
        client.release();
    }
});

app.post('/api/local-sessions/bulk-delete', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    const ids = Array.isArray(req.body.ids) ? req.body.ids.filter(id => Number.isFinite(Number(id))) : [];
    if (ids.length === 0) return res.status(400).json({ error: 'ids must be a non-empty array' });
    const client = await pool.connect();
    try {
        await client.query('BEGIN');
        await client.query('DELETE FROM local_session_points WHERE local_session_id = ANY($1::int[])', [ids]);
        const result = await client.query('DELETE FROM local_sessions WHERE id = ANY($1::int[])', [ids]);
        await client.query('COMMIT');
        res.json({ deleted: result.rowCount });
    } catch (e) {
        await client.query('ROLLBACK').catch(() => {});
        res.status(500).json({ error: e.message });
    } finally {
        client.release();
    }
});

app.get('/api/local-sessions/:id/points', async (req, res) => {
    if (!pool) return res.status(503).json({ error: 'database not configured' });
    try {
        const result = await pool.query(
            `SELECT received_at, data
             FROM local_session_points
             WHERE local_session_id = $1
             ORDER BY received_at ASC`,
            [req.params.id]
        );
        res.json(result.rows);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

app.get('/api/local-sessions/:id/export.csv', async (req, res) => {
    if (!pool) return res.status(503).send('database not configured');
    try {
        const result = await pool.query(
            `SELECT received_at, data
             FROM local_session_points
             WHERE local_session_id = $1
             ORDER BY received_at ASC`,
            [req.params.id]
        );

        res.setHeader('Content-Type', 'text/csv');
        res.setHeader('Content-Disposition', `attachment; filename="local-session-${req.params.id}.csv"`);

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

// ── App version (for the client's "an update is available" check) ──────
// A content hash of the actually-served index.html, computed once at
// boot — deliberately NOT a hand-maintained version string, so there's
// nothing to remember to bump/keep in sync across two files. It changes
// if and only if the served page actually did, which happens naturally
// on every real deploy (a fresh file on disk). Public, no auth — same
// reasoning as /api/latest, nothing sensitive in a hash.
let APP_VERSION = 'dev';
try {
    const indexHtml = fs.readFileSync(path.join(__dirname, 'public', 'index.html'));
    APP_VERSION = crypto.createHash('sha1').update(indexHtml).digest('hex').slice(0, 10);
} catch (e) {
    console.warn('[WARN] Could not hash public/index.html for /api/version:', e.message);
}

app.get('/api/version', (req, res) => {
    res.json({ version: APP_VERSION });
});

app.use(express.static(path.join(__dirname, 'public')));

const server = app.listen(PORT, () => {
    console.log(`[READY] Greenpower telemetry web listening on port ${PORT}`);
});

// ── Graceful shutdown ──────────────────────────────────────────────────
// Railway sends SIGTERM to the OLD container whenever a new deploy rolls
// out (and again on a manual restart) — this is a normal, expected part
// of every deploy, not a crash. Node has no default handler for SIGTERM
// though, so without one the process dies mid-signal and npm's wrapper
// reports that as "npm error / command failed / signal SIGTERM", reading
// exactly like a real failure in Railway's logs even on a totally healthy
// deploy. Handling it explicitly and exiting with a real code 0 (closing
// the DB pool first, if one's configured) makes npm see a clean exit
// instead — this is what actually stops that message from appearing on
// every single redeploy, not a config flag.
function shutdown(signal) {
    console.log(`[INFO] ${signal} received — shutting down gracefully.`);
    server.close(() => {
        if (pool) {
            pool.end().finally(() => process.exit(0));
        } else {
            process.exit(0);
        }
    });
    // Belt-and-suspenders — if something (a stuck connection) keeps
    // server.close()'s callback from ever firing, don't hang forever and
    // force Railway to SIGKILL after its own grace period; exit cleanly
    // on our own timeout first.
    setTimeout(() => process.exit(0), 5000).unref();
}
process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('SIGINT', () => shutdown('SIGINT'));
