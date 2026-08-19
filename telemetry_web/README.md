# Greenpower Telemetry — Web Dashboard

Always-on live dashboard for the Greenpower vehicle. Shows telemetry when [`receiver_agent`](../receiver_agent) is forwarding, otherwise shows offline. Meant to run on [Railway](https://railway.app), reachable at your own domain.

## Local test run

```bash
cd telemetry_web
npm install
npm start
```

Open `http://localhost:3000`. Without `TELEMETRY_API_KEY` set, the server prints a temporary one to the console at boot — use that for local testing.

## Deploying to Railway

1. Push this repo to GitHub (if not already).
2. In Railway: **New Project → Deploy from GitHub repo**, select this repo.
3. Set the **root directory** for the service to `telemetry_web` (Railway needs to know this isn't the repo root — it's a subfolder).
4. In the service's **Variables** tab, add:
   - `TELEMETRY_API_KEY` — a long random string you generate yourself (e.g. `openssl rand -hex 24`). This is the value `receiver_agent` needs to be configured with too.
5. Railway auto-detects the Node app from `package.json` and deploys. You'll get a `*.up.railway.app` URL immediately.

## Pointing your GoDaddy domain at it

1. In Railway: service → **Settings → Networking → Custom Domain**, add your domain or subdomain (e.g. `telemetry.yourdomain.com`).
2. Railway gives you a DNS record (usually a CNAME for a subdomain).
3. In GoDaddy's DNS management for the domain, add that record.
4. Wait for propagation — Railway auto-issues an SSL certificate once the record verifies.

A subdomain (`telemetry.yourdomain.com`) is simplest. Pointing the bare root domain needs an A/ALIAS record instead of a CNAME — see Railway's docs if that's what you want.

## API

- `POST /api/telemetry` — accepts one telemetry packet as JSON, requires `Authorization: Bearer <TELEMETRY_API_KEY>`. This is what `receiver_agent` calls.
- `GET /api/latest` — public, returns `{ online, ageMs, data }`. This is what the dashboard polls.
