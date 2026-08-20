// ════════════════════════════════════════════════════════════════════
//  GREENPOWER TELEMETRY WEB
//
//  Always-on dashboard. Runs on Railway 24/7 regardless of whether any
//  receiver is plugged in anywhere. ../receiver_agent posts live packets
//  to POST /api/telemetry whenever it's actively forwarding; the browser
//  dashboard polls GET /api/latest and shows "offline" once data goes
//  stale (no update within STALE_MS).
//
//  This process holds the latest packet in memory only — nothing is
//  persisted to disk or a database. A Railway restart/redeploy clears it,
//  which is fine for a live-status dashboard, not fine if you ever want
//  history/logging (that would need a real datastore, not implemented here).
// ════════════════════════════════════════════════════════════════════

const express = require('express');
const path = require('path');
const crypto = require('crypto');

const app = express();
app.use(express.json({ limit: '10kb' }));   // one telemetry packet is well under 1kb

const PORT = process.env.PORT || 3000;
const STALE_MS = 2000;    // no update in this long = dashboard shows offline (sender transmits at 5 Hz, so 2s is already 10 missed packets)

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

// ── In-memory latest-packet store ──────────────────────────────────
let latest = null;
let lastUpdateMs = 0;

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

app.use(express.static(path.join(__dirname, 'public')));

app.listen(PORT, () => {
    console.log(`[READY] Greenpower telemetry web listening on port ${PORT}`);
});
