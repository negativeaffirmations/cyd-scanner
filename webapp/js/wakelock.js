// Screen Wake Lock — keep the phone screen on while connected so geolocation keeps firing.
// Android Chrome freezes a backgrounded/screen-off page, which silently stops the watchPosition
// callbacks that push GPS to the device (that's why a walk logged with the screen off has only one
// fix). The OS releases the lock whenever the page is hidden, so reacquireWake() must run on every
// visibilitychange to re-take it when the page comes back. All calls are best-effort: a browser
// without the API, or a refusal (battery saver, no gesture), is non-fatal — the app still works,
// GPS just won't survive the screen going off.
import { log } from './util/dom.js';

let wl   = null;   // active WakeLockSentinel, or null
let want = false;  // whether we currently WANT the lock (set true between connect and disconnect)

export async function acquireWake(){
  want = true;
  if (!('wakeLock' in navigator) || wl || document.visibilityState !== 'visible') return;
  try {
    wl = await navigator.wakeLock.request('screen');
    wl.addEventListener('release', () => { wl = null; });   // OS dropped it (page hidden / system)
    log('Screen will stay on to keep GPS updating while connected.');
  } catch (e) { /* refused (battery saver / no gesture) — harmless */ }
}

export async function releaseWake(){
  want = false;
  if (wl) { try { await wl.release(); } catch (e) {} wl = null; }
}

// The OS auto-releases the lock when the page is hidden; re-take it once the page is visible again.
export function reacquireWake(){
  if (want && !wl && document.visibilityState === 'visible') acquireWake();
}
