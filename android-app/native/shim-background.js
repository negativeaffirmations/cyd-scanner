// COINTEL-FLO app only (docs/android-app.md "Stage 2"): keep working with the screen off / the app in the background.
//   - a foreground service (CfloBgService via the CfloBg plugin) while a scanner is connected: Android then keeps the
//     app process, and so this page, its BLE link, live upload and alert checks, running in the background
//   - navigator.geolocation on native GPS: the web view stops geolocation while the page is hidden, which would stop
//     the phone-GPS feed to the scanner (webapp/js/gps.js) the moment the screen goes off
//   - a 1 s "cflo-bg-tick" window event while in the background: a hidden page's own timers are throttled (down to
//     once a minute after a while), and notify.js checks for alerts on a 1 s timer; it also listens for this event
//   - a "Run in the background" card (Settings -> Notifications): Android's battery-optimization exemption
// Classic script, injected by scripts/build-www.mjs after shim-bluetooth.js; does nothing outside the app.
(function(){
  "use strict";
  const C = window.Capacitor;
  const BG = C && C.Plugins && C.Plugins.CfloBg;
  if(!BG || !C.isNativePlatform || !C.isNativePlatform()) return;
  const app = window.__cfloApp || {};
  const NAMES_KEY = "cflo.unitNames";          // unit names by device id (webapp/js/ble/devices.js)
  const STOP_DELAY_MS = +window.__cfloTestStopDelay || 10000;                // Switch scanner = disconnect + connect (up to 8 s): don't flap the service
  // While the service runs, CfloWebView keeps the page "visible" to Chromium so it keeps running in the background;
  // the real app state comes from the plugin's appState event.
  let appFg = true;
  C.addListener("CfloBg", "appState", ev => {
    appFg = !!(ev && ev.fg);
    if(appFg && typeof refresh === "function") refresh();   // back from the battery prompt etc.: repaint the card
  });
  const visible = () => appFg && document.visibilityState === "visible";
  function nameOf(id){ try{ return (JSON.parse(localStorage.getItem(NAMES_KEY) || "{}") || {})[id] || ""; }catch(e){ return ""; } }

  // ---------- the foreground service follows the scanner link ----------
  const live = new Set();
  let running = false, stopTimer = 0, shownText = "", lastLostId = "";
  function texts(){
    const names = [...live].map(id => nameOf(id) || "a scanner");
    const who = names.length ? names.join(", ") : "a scanner";
    return { title: "Connected to " + who,
             text: "Scanning, alerts and GPS keep running with the screen off. Tap to open COINTEL-FLO." };
  }
  async function startOrUpdate(){
    const t = texts();
    // The location state is part of the key (REV-001): a location grant after the start must restart the service so
    // it gains its location type, or GPS stops with the screen off.
    const key = t.title + t.text + (locOn ? "|L" : "");
    if(running && key === shownText) return;
    try{ await BG.start(t); running = true; shownText = key; }
    catch(e){ console.warn("background service:", (e && e.message) || e); note("service start failed (" + (visible() ? "open" : "background") + ")"); }
  }
  function stopSoon(){
    clearTimeout(stopTimer);
    stopTimer = setTimeout(() => {
      stopTimer = 0;
      if(live.size || !running) return;
      running = false; shownText = "";
      // Lost on its own while nobody is looking: leave a note, since the alerts stop with the link.
      const lost = lastLostId && !visible()
        ? "Lost the link to " + (nameOf(lastLostId) || "the scanner") + ". Alerts are off until you reconnect. Tap to open COINTEL-FLO."
        : "";
      BG.stop(lost ? { lost } : {}).catch(() => {});
    }, STOP_DELAY_MS);
  }
  const link = app.bleLink;
  if(link && link.events){
    link.events.addEventListener("link", ev => {
      note("scanner " + (ev.connected ? "connected" : ev.lost ? "link lost" : "disconnected") + " (links " +
        (live.size + (ev.connected ? 1 : -1)) + ", " + (visible() ? "app open" : "app in background") + ")");
      if(ev.connected){
        live.add(ev.id); lastLostId = "";
        clearTimeout(stopTimer); stopTimer = 0;
        startOrUpdate();
        maybeTip();
      }else{
        live.delete(ev.id);
        lastLostId = ev.lost ? ev.id : "";
        if(!live.size) stopSoon();
      }
    });
  }
  // The unit name arrives after the scanner's STATUS (a moment after connecting): repaint the notification then.
  // Only while visible: Android may refuse a service start request from the background.
  // REV-004: the service can also end on its own (its Disconnect button, Android): follow it, and restart it while a
  // scanner is still connected.
  setInterval(async () => {
    if(!live.size || !visible()) return;
    try{ if(!(await BG.status()).running && !stopTimer){ running = false; shownText = ""; } }catch(e){}
    startOrUpdate();
  }, 5000);
  // REV-002: a page that starts has no link yet, so a service left by an earlier page (activity recreated) must go.
  BG.stop({}).catch(() => {});
  C.addListener("CfloBg", "disconnectRequested", () => { if(link && link.disconnectAll) link.disconnectAll(); });
  // Background log: every tick is answered with the page's counters, proving its JavaScript still runs.
  const diag = () => (window.__cfloApp && window.__cfloApp.diag) || {};
  const note = m => { try{ BG.diagNote({ m }).catch(() => {}); }catch(e){} };
  C.addListener("CfloBg", "tick", () => {
    if(!visible()) window.dispatchEvent(new Event("cflo-bg-tick"));
    const d = diag();
    try{ BG.ack({ c: "ble=" + (d.ble || 0) + " dets=" + (d.dets || 0) + " shown=" + (d.shown || 0) + " failed=" + (d.failed || 0) +
      " links=" + live.size + " vis=" + (visible() ? 1 : 0) }).catch(() => {}); }catch(e){}
  });

  // ---------- navigator.geolocation on native GPS ----------
  const PERMISSION_DENIED = 1, POSITION_UNAVAILABLE = 2, TIMEOUT = 3;
  const posErr = (code, message) => ({ code, message, PERMISSION_DENIED, POSITION_UNAVAILABLE, TIMEOUT });
  const toPos = l => ({
    coords: { latitude: l.lat, longitude: l.lon, accuracy: l.acc, altitude: l.alt == null ? null : l.alt,
              altitudeAccuracy: null, heading: l.hdg == null ? null : l.hdg, speed: l.spd == null ? null : l.spd },
    timestamp: l.time || Date.now(),
  });
  const watches = new Map();   // id -> {ok, err}
  let nextId = 1, locOn = false, locStarting = null, lastPos = null, stopLocTimer = 0;
  C.addListener("CfloBg", "location", l => {
    if(!l || typeof l.lat !== "number" || typeof l.lon !== "number") return;
    lastPos = toPos(l);
    for(const w of [...watches.values()]){ try{ w.ok(lastPos); }catch(e){ console.warn(e); } }
  });
  function ensureLoc(){
    clearTimeout(stopLocTimer); stopLocTimer = 0;
    if(locOn || locStarting) return;
    // locStarting is cleared BEFORE any callback runs (REV-005): gps.js's error handler starts a new one-shot at once.
    locStarting = BG.startLocation().then(() => {
      locStarting = null;
      locOn = true;
      if(running) startOrUpdate();   // the service can add its location type now that permission exists
    }, e => {
      locStarting = null;
      const denied = (e &&(e.code === "denied" || /denied/i.test(e.message || "")));
      const err = posErr(denied ? PERMISSION_DENIED : POSITION_UNAVAILABLE, (e && e.message) || "Location unavailable");
      for(const w of [...watches.values()]){ try{ if(w.err) w.err(err); }catch(x){} }
    });
  }
  function maybeStopLoc(){
    if(watches.size || stopLocTimer) return;
    stopLocTimer = setTimeout(() => {   // a one-shot right after a watch ends shouldn't restart the GPS
      stopLocTimer = 0;
      if(watches.size) return;
      locOn = false;
      BG.stopLocation().catch(() => {});
    }, 2000);
  }
  const geolocation = {
    watchPosition(ok, err){
      const id = nextId++;
      watches.set(id, { ok, err });
      ensureLoc();
      return id;
    },
    clearWatch(id){ if(watches.delete(id)) maybeStopLoc(); },
    getCurrentPosition(ok, err, opts){
      const maxAge = (opts && +opts.maximumAge) || 0;
      if(lastPos && Date.now() - lastPos.timestamp <= maxAge){ setTimeout(() => ok(lastPos), 0); return; }
      let done = false;
      const end = () => { done = true; clearTimeout(t); geolocation.clearWatch(id); };
      const id = geolocation.watchPosition(p => { if(done) return; end(); ok(p); },
        e => { if(done) return; end(); if(err) err(e); });
      const to = opts && opts.timeout != null ? +opts.timeout : Infinity;   // like the browser: no timeout by default
      const t = isFinite(to) ? setTimeout(() => { if(done) return; end(); if(err) err(posErr(TIMEOUT, "Timeout expired")); },
        Math.max(0, to)) : 0;
    },
  };
  try{ Object.defineProperty(navigator, "geolocation", { value: geolocation, configurable: true }); }catch(e){}

  // ---------- "Run in the background" card (Settings -> Notifications) ----------
  let exempt = null;
  async function refresh(){
    try{ exempt = !!(await BG.status()).batteryExempt; }catch(e){ exempt = null; }
    render();
  }
  function render(){
    const st = document.getElementById("bgState"), btn = document.getElementById("bgAllowBtn");
    if(!st || !btn) return;
    if(exempt){
      st.textContent = "Allowed. While a scanner is connected the app keeps scanning, alerting and sending GPS with the screen off; a \"Connected to …\" notification shows it is running.";
      st.className = "sub keystate ok";
      btn.hidden = true;
    }else{
      st.textContent = "The phone's battery saver can pause the app with the screen off, which stops alerts and GPS. Allow it to run in the background:";
      st.className = "sub keystate warn";
      btn.hidden = false;
    }
  }
  function addCard(){
    const anchor = document.getElementById("nfCard");
    if(!anchor || document.getElementById("bgCard")) return;
    const card = document.createElement("div");
    card.className = "card"; card.id = "bgCard"; card.style.marginTop = "12px";
    card.innerHTML = '<strong class="cardhdr">Run in the background</strong>' +
      '<div id="bgState" class="sub keystate" style="margin:.4em 0">Checking…</div>' +
      '<button id="bgAllowBtn" hidden>Allow background running</button>' +
      '<div class="sub" style="margin-top:.6em">The background log records what the app did with the screen off (counts and phone state only, no devices or places).</div>' +
      '<button id="bgLogBtn" class="secondary" style="margin-top:.4em">Background log</button>';
    anchor.insertAdjacentElement("beforebegin", card);
    document.getElementById("bgLogBtn").addEventListener("click", showLog);
    document.getElementById("bgAllowBtn").addEventListener("click", async () => {
      try{ exempt = !!(await BG.requestBatteryExemption()).batteryExempt; render(); }
      catch(e){ refresh(); }
    });
    refresh();
  }
  // The log viewer: newest last, Save writes it to Downloads/COINTEL-FLO (through shim-platform.js's download path).
  async function showLog(){
    let text = "", state = "";
    try{ const r = await BG.diagLog(); text = r.text || ""; state = r.state || ""; }catch(e){ text = "Could not read the log: " + ((e && e.message) || e); }
    const d = document.createElement("dialog");
    d.innerHTML = '<div class="modalhdr"><strong>Background log</strong></div>' +
      '<div class="sub" data-r="state" style="margin:.3em 0"></div>' +
      '<pre data-r="log" style="max-height:55vh;overflow:auto;font-size:.68rem;white-space:pre-wrap;margin:0"></pre>' +
      '<div class="fsactions dlgactions"><button class="secondary" data-r="clear">Clear</button> ' +
      '<button class="secondary" data-r="save">Save</button> <button data-r="close">Close</button></div>';
    const $r = k => d.querySelector('[data-r="' + k + '"]');
    $r("state").textContent = "Now: " + state;
    $r("log").textContent = text || "(empty)";
    const end = () => { try{ d.close(); }catch(e){} d.remove(); };
    $r("close").addEventListener("click", end);
    d.addEventListener("cancel", e => { e.preventDefault(); end(); });
    $r("clear").addEventListener("click", async () => { try{ await BG.diagClear(); }catch(e){} end(); showLog(); });
    $r("save").addEventListener("click", () => {
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
      a.download = "cflo-background-" + new Date().toISOString().slice(0, 16).replace(/[:T]/g, "") + ".txt";
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 60000);
    });
    document.body.append(d); d.showModal();
    const pre = $r("log"); pre.scrollTop = pre.scrollHeight;
  }
  // Once per app start, on the first connection while the app is not exempt: point at the card.
  let tipped = false;
  function maybeTip(){
    if(tipped) return; tipped = true;
    BG.status().then(s => {
      if(s && s.batteryExempt) return;
      const t = document.getElementById("toast");
      if(!t) return;
      const msg = "To keep alerts coming with the screen off: Settings → Notifications → Allow background running.";
      t.textContent = msg;
      t.className = "toast show"; t.hidden = false;
      // Hide only our own text: a web app toast shown meanwhile keeps its own timer (REV-014).
      setTimeout(() => { if(t.textContent === msg){ t.classList.remove("show"); t.hidden = true; } }, 6000);
    }).catch(() => {});
  }
  document.addEventListener("visibilitychange", () => { if(visible()) refresh(); });
  if(document.readyState === "loading") document.addEventListener("DOMContentLoaded", addCard);
  else addCard();

  // bgRunning(): the service is up for the current link (notify.js's status line, REV-015).
  window.__cfloApp = Object.assign(window.__cfloApp || {}, { background: true, bgRunning: () => running && live.size > 0 });
})();
