// Notification service worker (js/features/notify.js). Android Chrome only shows notifications through a
// service worker (registration.showNotification), so this exists for that and nothing else: NO fetch handler and
// NO caching, so it can never serve a stale copy of the app. The page itself raises every notification; this file
// only brings the tab back when one is tapped.
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", e => e.waitUntil(self.clients.claim()));

self.addEventListener("notificationclick", e => {
  e.notification.close();
  const data = e.notification.data || {};
  e.waitUntil((async () => {
    const all = await self.clients.matchAll({ type: "window", includeUncontrolled: true });
    // Prefer the tab that raised it (same scope); focus it and tell it what was tapped.
    const tab = all.find(c => c.url.startsWith(self.registration.scope)) || all[0];
    if (tab) {
      try { await tab.focus(); } catch (_) {}
      tab.postMessage({ type: "cflo-notify-click", mac: data.mac || "", kind: data.kind || "" });
      return;
    }
    // The tab is gone: open the app (the BLE link has to be reconnected by hand).
    await self.clients.openWindow(self.registration.scope);
  })());
});
