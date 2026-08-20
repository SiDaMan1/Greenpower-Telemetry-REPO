# Greenpower Telemetry — Web Dashboard

Always-on live dashboard for the Greenpower vehicle. Shows telemetry when [`receiver_agent`](../receiver_agent) is forwarding, otherwise shows offline. Meant to run on [Railway](https://railway.app), reachable at your own domain.

## Local test run

```bash
cd telemetry_web
npm install
npm start
```

Open `http://localhost:3000`. Without `TELEMETRY_API_KEY` set, the server prints a temporary one to the console at boot — use that for local testing. Without `DATABASE_URL` set, the app still runs fine — live data keeps working, but the Sessions tab shows "unavailable" instead of erroring.

## Deploying to Railway

1. Push this repo to GitHub (if not already).
2. In Railway: **New Project → Deploy from GitHub repo**, select this repo.
3. Set the **root directory** for the service to `telemetry_web` (Railway needs to know this isn't the repo root — it's a subfolder).
4. In the service's **Variables** tab, add:
   - `TELEMETRY_API_KEY` — a long random string you generate yourself (e.g. `openssl rand -hex 24`). This is the value `receiver_agent` needs to be configured with too.
5. Railway auto-detects the Node app from `package.json` and deploys. You'll get a `*.up.railway.app` URL immediately.

## Adding persistent session history (Sessions tab + CSV export)

Without this step, the site works exactly as before — live data, no history. To enable the Sessions tab:

1. In your Railway project, click **+ New → Database → Add PostgreSQL**.
2. Railway automatically injects a `DATABASE_URL` variable into every service in the same project — including this one. No copy-pasting a connection string required.
3. Redeploy (or just wait for the next deploy) — on boot, the server creates its two tables (`sessions`, `telemetry_points`) automatically if they don't already exist. Check the deploy log for `[OK] Database schema ready`.
4. That's it — every packet `receiver_agent` forwards from now on is also written to the database, grouped into sessions automatically (a new session starts whenever a packet arrives after 60+ seconds of no data). The Sessions tab lists them, lets you view a few key charts per session, and export the full session as CSV.

Nothing before this step is retroactive — sessions only start recording once `DATABASE_URL` is present.

## Pointing your GoDaddy domain at it

1. In Railway: service → **Settings → Networking → Custom Domain**, add your domain or subdomain (e.g. `telemetry.yourdomain.com`).
2. Railway gives you a DNS record (usually a CNAME for a subdomain).
3. In GoDaddy's DNS management for the domain, add that record.
4. Wait for propagation — Railway auto-issues an SSL certificate once the record verifies.

A subdomain (`telemetry.yourdomain.com`) is simplest. Pointing the bare root domain needs an A/ALIAS record instead of a CNAME — see Railway's docs if that's what you want.

## API

- `POST /api/telemetry` — accepts one telemetry packet as JSON, requires `Authorization: Bearer <TELEMETRY_API_KEY>`. This is what `receiver_agent` calls.
- `GET /api/latest` — public, returns `{ online, ageMs, data }`. This is what the dashboard polls.
- `GET /api/sessions` — public, returns the most recent 200 sessions (`id`, `started_at`, `ended_at`, `packet_count`). `503` if no database is configured.
- `GET /api/sessions/:id/points` — public, returns every recorded packet for that session (`received_at` + the packet JSON), oldest first.
- `GET /api/sessions/:id/export.csv` — public, streams the full session as a CSV download with a fixed column order (defined in `CSV_COLUMNS` in `server.js`).
