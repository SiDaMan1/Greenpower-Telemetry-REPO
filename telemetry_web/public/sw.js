// ════════════════════════════════════════════════════════════════════
//  SERVICE WORKER — installability only, deliberately does NO caching.
//
//  This is a live-telemetry dashboard. Caching API responses (or the page
//  shell in a way that could serve stale content) would actively work
//  against the app's whole purpose — showing a cached "LIVE" reading from
//  five minutes ago is worse than no offline support at all. The only job
//  of this file is to satisfy the browser's installability checklist
//  (Chrome/Android in particular requires a registered service worker with
//  a fetch handler before it'll fire the beforeinstallprompt event) — every
//  request is passed straight through to the network untouched.
//
//  If real offline support is ever wanted, that's a deliberate feature to
//  design (what SHOULD show when offline — an explicit "stale" state, not
//  silently-cached numbers), not a default to fall into here.
// ════════════════════════════════════════════════════════════════════

self.addEventListener('install', () => {
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', () => {
  // Intentionally does not call event.respondWith() — registering the
  // listener is what matters for installability, not intercepting anything.
});
