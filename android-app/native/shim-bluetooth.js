// COINTEL-FLO app only (docs/android-app.md): navigator.bluetooth on top of the native BLE plugin
// (@capacitor-community/bluetooth-le), because Android's web view has no Web Bluetooth. The web app's own code is
// unchanged: this provides the slice of Web Bluetooth it uses.
//   navigator.bluetooth.requestDevice / getDevices; BluetoothDevice (id, name, gatt, watchAdvertisements,
//   gattserverdisconnected, advertisementreceived); GATT server / service / characteristic (readValue, writeValue,
//   writeValueWithResponse, writeValueWithoutResponse, startNotifications, stopNotifications,
//   characteristicvaluechanged).
// Classic script, injected before the web app's module by scripts/build-www.mjs; does nothing outside the app.
(function(){
  "use strict";
  const C = window.Capacitor;
  const BLE = C && C.Plugins && C.Plugins.BluetoothLe;
  if(!BLE) return;

  const SVC = "9a1e0000-2b7e-4c1a-9b00-1a2b3c4d5e6f";   // the scanners' service (webapp/js/config.js SVC)
  const KNOWN_KEY = "cflo.app.devices";                 // [{id, name}] scanners this app has connected to
  const NAMES_KEY = "cflo.unitNames";                   // unit names by device id (webapp/js/ble/devices.js)
  const OP_TIMEOUT = 5000;                               // the plugin's own default; a stuck read must not hold the queue long

  // ---------- helpers ----------
  const lc = s => String(s || "").toLowerCase();
  function toHex(data){
    let u8;
    if(data instanceof Uint8Array) u8 = data;
    else if(ArrayBuffer.isView(data)) u8 = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    else if(data instanceof ArrayBuffer) u8 = new Uint8Array(data);
    else throw new TypeError("BufferSource expected");
    let s = "";
    for(let i = 0; i < u8.length; i++) s += (u8[i] < 16 ? "0" : "") + u8[i].toString(16);
    return s;
  }
  function fromHex(hex){
    hex = String(hex || "").replace(/[^0-9a-f]/gi, "");
    const u8 = new Uint8Array(hex.length >> 1);
    for(let i = 0; i < u8.length; i++) u8[i] = parseInt(hex.substr(i * 2, 2), 16);
    return new DataView(u8.buffer);
  }
  const domErr = (msg, name) => new DOMException(msg, name);
  const errOf = (e, name) => domErr((e && e.message) || String(e), name || "NetworkError");
  function readJson(key, dflt){ try{ return JSON.parse(localStorage.getItem(key) || "") || dflt; }catch(e){ return dflt; } }

  // Android runs one GATT operation per connection at a time and the plugin does not queue them: serialize every
  // read / write / notification change here (Chrome queued internally; the web app's own gattq covers only writes).
  // When the plugin gives up on an operation (its 5 s timeout) Android's GATT can still be busy with it, and the next
  // one is refused at once ("Reading/Writing characteristic failed."): retry such a refusal once, a moment later.
  // A refused operation never reached the scanner, so a retried write is not sent twice.
  let tail = Promise.resolve();
  const refused = e => /characteristic failed|descriptor failed|notifications failed/i.test((e && (e.message || e.errorMessage)) || String(e));
  function gattOp(fn){
    const run = tail.then(async () => {
      try{ return await fn(); }
      catch(e){ if(!refused(e)) throw e; await new Promise(r => setTimeout(r, 400)); return fn(); }
    });
    tail = run.catch(() => {});
    return run;
  }

  // ---------- plugin start-up (permissions: Nearby devices + location) ----------
  let ready = null;
  function init(){
    if(!ready) ready = (async () => {
      await BLE.initialize({ androidNeverForLocation:false });
      let on = false;
      try{ on = (await BLE.isEnabled()).value; }catch(e){}
      if(!on){ try{ await BLE.requestEnable(); }catch(e){ throw domErr("Bluetooth is off", "NotAllowedError"); } }
    })().catch(e => { ready = null; throw e; });
    return ready;
  }

  // ---------- remembered scanners ----------
  function known(){ return readJson(KNOWN_KEY, []).filter(d => d && d.id); }
  function remember(dev){
    const list = known().filter(d => d.id !== dev.id);
    list.unshift({ id:dev.id, name:dev.name || "" });
    try{ localStorage.setItem(KNOWN_KEY, JSON.stringify(list.slice(0, 32))); }catch(e){}
  }
  const unitName = id => readJson(NAMES_KEY, {})[id] || "";

  // ---------- one shared LE scan (chooser + presence) ----------
  // Android ignores a scan that starts more than 5 times in 30 s (no results, no error) and throttles one that runs
  // over 30 min: so a stop waits 10 s for the next user (the switcher re-subscribes after every transfer), and a long
  // scan is restarted every 25 min.
  const scanSubs = new Set();
  let scanning = false, scanHandle = null, scanStarting = null, stopTimer = 0, renewTimer = 0;
  const STOP_DELAY_MS = 10000, RENEW_MS = 25 * 60000;
  // In the background (app not on screen) the presence scan pauses: the page keeps running there now (CfloWebView), and
  // Switch scanner's presence watch would otherwise scan the whole walk. It resumes for the same subscribers on return.
  let appBg = false, bgPaused = false;
  if(C.Plugins.CfloBg) C.addListener("CfloBg", "appState", ev => {
    appBg = !(ev && ev.fg);
    if(appBg){
      if(scanning || scanStarting){
        bgPaused = true;
        if(stopTimer){ clearTimeout(stopTimer); stopTimer = 0; }
        (scanStarting || Promise.resolve()).catch(() => {}).then(() => {
          if(!appBg || !scanning) return;
          scanning = false; clearInterval(renewTimer); renewTimer = 0;
          BLE.stopLEScan().catch(() => {});
        });
      }
    }else if(bgPaused){
      bgPaused = false;
      const subs = [...scanSubs]; scanSubs.clear();
      subs.forEach(f => scanSub(f).catch(() => {}));
    }
  });
  async function scanSub(fn){
    scanSubs.add(fn);
    if(stopTimer){ clearTimeout(stopTimer); stopTimer = 0; }
    if(appBg){ bgPaused = true; return; }   // starts when the app is back on screen
    if(scanning) return;
    if(!scanStarting) scanStarting = (async () => {
      await init();
      if(!scanHandle) scanHandle = C.addListener("BluetoothLe", "onScanResult", r => { scanSubs.forEach(f => { try{ f(r); }catch(e){} }); });
      await BLE.requestLEScan({ services:[SVC], allowDuplicates:true, scanMode:2 });
      scanning = true;
      clearInterval(renewTimer);
      renewTimer = setInterval(() => {
        if(!scanning) return;
        BLE.stopLEScan().catch(() => {}).then(() => scanning ? BLE.requestLEScan({ services:[SVC], allowDuplicates:true, scanMode:2 }) : 0).catch(() => {});
      }, RENEW_MS);
    })().finally(() => { scanStarting = null; });
    try{ await scanStarting; }
    catch(e){ scanSubs.delete(fn); throw e; }
    if(!scanSubs.size) scanStopSoon();   // everyone left while it was starting
  }
  function scanUnsub(fn){ scanSubs.delete(fn); if(!scanSubs.size) scanStopSoon(); }
  function scanStopSoon(){
    if(stopTimer) return;
    stopTimer = setTimeout(() => {
      stopTimer = 0;
      if(scanSubs.size || !scanning) return;
      scanning = false;
      clearInterval(renewTimer); renewTimer = 0;
      BLE.stopLEScan().catch(() => {});
    }, STOP_DELAY_MS);
  }

  // ---------- link events for the app layer (native/shim-background.js: the background service) ----------
  // "link" events carry {id, connected, lost}: lost = the scanner went away on its own (not a disconnect() call).
  const linkEvents = new EventTarget();
  // Counters for the background log (native/shim-background.js): BLE notifications, DETS packets (cumulative).
  const diag = Object.assign((window.__cfloApp = window.__cfloApp || {}).diag || (window.__cfloApp.diag = {}), Object.assign({ ble:0, dets:0, shown:0, failed:0 }, window.__cfloApp.diag));
  function linkEvent(id, connected, lost){
    const ev = new Event("link"); ev.id = id; ev.connected = connected; ev.lost = !!lost;
    try{ linkEvents.dispatchEvent(ev); }catch(e){}
  }

  // ---------- device objects (one per id, so identity comparisons in the web app hold) ----------
  const devices = new Map();
  function deviceFor(id, name){
    let d = devices.get(id);
    if(!d){ d = new ShimDevice(id, name); devices.set(id, d); }
    else if(name && !d.name) d.name = name;
    return d;
  }

  class ShimDevice extends EventTarget {
    constructor(id, name){
      super();
      this.id = id;
      this.name = name || "CFLO";
      this.gatt = new ShimServer(this);
      this.watchingAdvertisements = false;
      this._adv = null;
    }
    async watchAdvertisements(opts){
      const signal = opts && opts.signal;
      if(signal && signal.aborted) throw domErr("aborted", "AbortError");
      // Hook the abort FIRST: it can come while the scan is still starting (permission prompts), and a listener
      // added to an already-aborted signal never runs (that left a scan running for the whole session).
      if(signal) signal.addEventListener("abort", () => this._unwatch(), { once:true });
      if(this._adv) return;
      const fn = r => {
        if(!r || !r.device || r.device.deviceId !== this.id) return;
        const ev = new Event("advertisementreceived");
        ev.rssi = r.rssi; ev.txPower = r.txPower; ev.name = r.localName || "";
        this.dispatchEvent(ev);
      };
      this._adv = fn;
      try{ await scanSub(fn); }
      catch(e){ if(this._adv === fn) this._adv = null; scanUnsub(fn); throw errOf(e, "NotAllowedError"); }
      if(this._adv !== fn || (signal && signal.aborted)){ this._unwatch(); throw domErr("aborted", "AbortError"); }
      this.watchingAdvertisements = true;
    }
    _unwatch(){ if(this._adv){ scanUnsub(this._adv); this._adv = null; } this.watchingAdvertisements = false; }
    forget(){ return Promise.resolve(); }
  }

  class ShimServer {
    constructor(device){
      this.device = device;
      this.connected = false;
      this._gen = 0;            // bumps on every connect / disconnect: a connect that was cancelled can't come alive
      this._cancel = null;      // rejects the connect() in flight (disconnect() during a connect)
      this._discHandle = null;
      this._services = [];
      this._chars = new Map();  // uuid -> ShimCharacteristic (this connection)
    }
    async connect(){
      await init();
      const d = this.device, gen = ++this._gen;
      if(!this._discHandle) this._discHandle = C.addListener("BluetoothLe", "disconnected|" + d.id, () => this._lost());
      // Android often never reports a cancelled pending connect, so the plugin's connect() would wait out its own
      // 15 s: disconnect() rejects this one at once instead (the web app's switch gives up after 8 s).
      const cancelled = new Promise((_, rej) => { this._cancel = () => rej(domErr("connection cancelled", "AbortError")); });
      const attempt = BLE.connect({ deviceId:d.id, timeout:15000 });
      try{ await Promise.race([attempt, cancelled]); }
      catch(e){
        attempt.then(() => { if(gen !== this._gen || !this.connected) BLE.disconnect({ deviceId:d.id }).catch(() => {}); }, () => {});
        throw e instanceof DOMException ? e : errOf(e, "NetworkError");
      }
      finally{ this._cancel = null; }
      if(gen !== this._gen){ BLE.disconnect({ deviceId:d.id }).catch(() => {}); throw domErr("connection cancelled", "AbortError"); }
      let svcs = [];
      try{ svcs = (await BLE.getServices({ deviceId:d.id })).services || []; }catch(e){}
      this._services = svcs;
      this._chars = new Map();
      this.connected = true;
      remember(d);
      linkEvent(d.id, true);
      return this;
    }
    // Like Web Bluetooth: `connected` is false at once and gattserverdisconnected follows. The event waits for the
    // plugin to confirm, so the plugin's own "disconnected" notice (sent just before that) is already past and
    // can't hit a quick reconnect to the same scanner.
    disconnect(){
      const was = this.connected;
      this._gen++;
      if(this._cancel) this._cancel();
      this._teardown();
      if(was) linkEvent(this.device.id, false, false);
      let fired = !was;
      const fire = () => { if(fired) return; fired = true; this.device.dispatchEvent(new Event("gattserverdisconnected")); };
      BLE.disconnect({ deviceId:this.device.id }).catch(() => {}).then(fire);
      if(was) setTimeout(fire, 3000);
    }
    _lost(){                          // the scanner went away (plugin notice); ignored once we're already down
      if(!this.connected) return;
      this._teardown();
      linkEvent(this.device.id, false, true);
      setTimeout(() => this.device.dispatchEvent(new Event("gattserverdisconnected")), 0);
    }
    _teardown(){
      this.connected = false;
      this._chars.forEach(c => c._drop());
      this._chars = new Map();
    }
    async getPrimaryService(uuid){
      if(!this.connected) throw domErr("GATT Server is disconnected.", "NetworkError");
      const s = this._services.find(x => lc(x.uuid) === lc(uuid));
      if(!s) throw domErr("No Services matching UUID " + uuid + " found in Device.", "NotFoundError");
      return new ShimService(this, s);
    }
    async getPrimaryServices(){ return this._services.map(s => new ShimService(this, s)); }
  }

  class ShimService {
    constructor(server, raw){ this._server = server; this._raw = raw; this.uuid = lc(raw.uuid); this.device = server.device; this.isPrimary = true; }
    async getCharacteristic(uuid){
      if(!this._server.connected) throw domErr("GATT Server is disconnected.", "NetworkError");
      const raw = (this._raw.characteristics || []).find(c => lc(c.uuid) === lc(uuid));
      if(!raw) throw domErr("No Characteristics matching UUID " + uuid + " found in Service.", "NotFoundError");
      const key = this.uuid + "|" + lc(uuid);
      let ch = this._server._chars.get(key);
      if(!ch){ ch = new ShimCharacteristic(this, raw); this._server._chars.set(key, ch); }
      return ch;
    }
  }

  class ShimCharacteristic extends EventTarget {
    constructor(service, raw){
      super();
      this.service = service;
      this.uuid = lc(raw.uuid);
      this.properties = raw.properties || {};
      this.value = null;
      this._notif = null;
    }
    _ids(){ return { deviceId:this.service.device.id, service:this.service.uuid, characteristic:this.uuid, timeout:OP_TIMEOUT }; }
    _live(){ if(!this.service._server.connected) throw domErr("GATT Server is disconnected.", "NetworkError"); }
    async readValue(){
      this._live();
      let r;
      try{ r = await gattOp(() => BLE.read(this._ids())); }catch(e){ throw errOf(e); }
      this.value = fromHex(r && r.value);
      return this.value;
    }
    async _write(data, withResponse){
      this._live();
      const opts = Object.assign(this._ids(), { value:toHex(data) });
      try{ await gattOp(() => withResponse ? BLE.write(opts) : BLE.writeWithoutResponse(opts)); }
      catch(e){ throw errOf(e); }
    }
    writeValue(data){ return this._write(data, !!this.properties.write || !this.properties.writeWithoutResponse); }
    writeValueWithResponse(data){ return this._write(data, true); }
    writeValueWithoutResponse(data){ return this._write(data, false); }
    async startNotifications(){
      this._live();
      if(this._notif) return this;
      const ids = this._ids();
      const isDets = this.uuid.startsWith("9a1e0006");   // the live detection stream (background diagnostics)
      this._notif = C.addListener("BluetoothLe", "notification|" + ids.deviceId + "|" + ids.service + "|" + ids.characteristic, ev => {
        diag.ble++; if(isDets) diag.dets++;
        this.value = fromHex(ev && ev.value);
        this.dispatchEvent(new Event("characteristicvaluechanged"));
      });
      try{ await gattOp(() => BLE.startNotifications(ids)); }
      catch(e){ this._drop(); throw errOf(e); }
      return this;
    }
    async stopNotifications(){
      const had = !!this._notif;
      this._drop();
      if(had && this.service._server.connected){ try{ await gattOp(() => BLE.stopNotifications(this._ids())); }catch(e){} }
      return this;
    }
    _drop(){ if(this._notif){ try{ this._notif.remove(); }catch(e){} this._notif = null; } }
  }

  // ---------- the chooser (replaces Chrome's device picker) ----------
  function chooser(){
    return new Promise((resolve, reject) => {
      const seen = new Map();   // id -> {id, name, rssi, at}
      const dlg = document.createElement("dialog");
      dlg.className = "appchooser";
      dlg.innerHTML =
        '<div class="modalhdr"><strong>Connect to a scanner</strong></div>' +
        '<div class="sub" data-r="msg">Looking for scanners…</div>' +
        '<div data-r="list"></div>' +
        '<div class="fsactions dlgactions"><button class="secondary" data-r="cancel">Cancel</button></div>';
      const $r = k => dlg.querySelector('[data-r="' + k + '"]');
      let done = false, timer = 0;
      const finish = (val, err) => {
        if(done) return; done = true;
        clearInterval(timer); scanUnsub(onResult);
        try{ dlg.close(); }catch(e){} dlg.remove();
        if(err) reject(err); else resolve(val);
      };
      const label = s => unitName(s.id) || (known().find(k => k.id === s.id) || {}).name || s.name || "CFLO";
      // One row per scanner, kept in place and updated in place (REV-003): rebuilding and re-sorting by signal every
      // tick swapped rows under the finger. A row is placed once (used-before units first) and leaves after 10 s quiet.
      const rowEls = new Map();
      function render(){
        const now = Date.now(), list = $r("list");
        for(const [id, el] of rowEls) if(!seen.has(id) || now - seen.get(id).at >= 10000){ el.remove(); rowEls.delete(id); }
        for(const s of seen.values()){
          if(now - s.at >= 10000) continue;
          let row = rowEls.get(s.id);
          const usedBefore = !!unitName(s.id) || known().some(k => k.id === s.id);
          if(!row){
            row = document.createElement("button");
            row.className = "sessrow keyrow appchoice";
            row.type = "button";
            row.innerHTML = '<span class="material-symbols-outlined">bluetooth</span><span class="grow"></span><span class="sub"></span>';
            row.addEventListener("click", () => finish(deviceFor(s.id, s.name)));
            row.dataset.used = usedBefore ? "1" : "";
            const firstNew = usedBefore ? [...list.children].find(c => !c.dataset.used) : null;
            list.insertBefore(row, firstNew || null);
            rowEls.set(s.id, row);
          }
          row.children[1].textContent = label(s) + (usedBefore ? "" : " (new)");
          row.children[2].textContent = s.rssi + " dBm";
        }
        $r("msg").textContent = rowEls.size ? "Tap a scanner. Ones you have used before show their name." : "Looking for scanners… Make sure the scanner is on and nearby.";
      }
      function onResult(r){
        if(!r || !r.device) return;
        seen.set(r.device.deviceId, { id:r.device.deviceId, name:r.localName || r.device.name || "", rssi:r.rssi, at:Date.now() });
      }
      $r("cancel").addEventListener("click", () => finish(null, domErr("User cancelled the requestDevice() chooser.", "NotFoundError")));
      dlg.addEventListener("cancel", e => { e.preventDefault(); finish(null, domErr("User cancelled the requestDevice() chooser.", "NotFoundError")); });
      document.body.append(dlg);
      dlg.showModal();
      timer = setInterval(render, 700);
      render();
      scanSub(onResult).catch(e => finish(null, errOf(e, "NotAllowedError")));
    });
  }

  const bluetooth = {
    async requestDevice(){ await init(); return chooser(); },
    async getDevices(){ return known().map(k => deviceFor(k.id, k.name)); },
    async getAvailability(){ try{ await init(); return true; }catch(e){ return false; } },
  };
  try{ Object.defineProperty(navigator, "bluetooth", { value:bluetooth, configurable:true }); }
  catch(e){ navigator.bluetooth = bluetooth; }
  window.BluetoothDevice = ShimDevice;
  window.__cfloApp = Object.assign(window.__cfloApp || {}, { ble:true, bleLink: {
    events: linkEvents,
    // Every scanner this app holds a link to, disconnected the normal way (the web app sees gattserverdisconnected).
    disconnectAll(){ devices.forEach(d => { if(d.gatt.connected) d.gatt.disconnect(); }); },
  } });
})();
