// COINTEL-FLO app only (docs/android-app.md): stand-ins for browser features Android's web view lacks.
//   - "Scan QR" buttons beside the key / server paste boxes (the phone camera can't hand a QR link to the app)
//   - window.open of an outside page (Wi-Fi download page, links) -> the phone's browser
// Classic script, injected by scripts/build-www.mjs before the web app's module; does nothing outside the app.
(function(){
  "use strict";
  const C = window.Capacitor;
  if(!C || !C.Plugins || !C.isNativePlatform || !C.isNativePlatform()) return;
  const P = C.Plugins;

  // ---------- QR scanning (Google code scanner via ML Kit: no camera permission, Google draws the UI) ----------
  async function scanQr(){
    const S = P.BarcodeScanner;
    if(!S) throw new Error("QR scanner missing from this build");
    try{
      const a = await S.isGoogleBarcodeScannerModuleAvailable();
      if(a && a.available === false){
        await S.installGoogleBarcodeScannerModule();
        throw new Error("The QR scanner is being installed by Google Play services. Try again in a moment.");
      }
    }catch(e){ if(/installed by Google/.test(e.message)) throw e; }
    const r = await S.scan({ formats:["QR_CODE"] });
    const b = r && r.barcodes && r.barcodes[0];
    return b ? (b.rawValue || b.displayValue || "") : "";
  }


  function addScanButton(afterId, onText){
    const anchor = document.getElementById(afterId);
    if(!anchor || document.getElementById(afterId + "Qr")) return;
    const b = document.createElement("button");
    b.id = afterId + "Qr";
    b.type = "button";
    b.className = "secondary appqr";
    b.style.marginTop = ".4em";
    b.innerHTML = '<span class="material-symbols-outlined" style="vertical-align:-5px;font-size:20px">qr_code_scanner</span> Scan QR';
    b.addEventListener("click", async () => {
      let text = "";
      try{ text = (await scanQr()).trim(); }
      catch(e){ if(!/cancel/i.test(e && e.message || "")) alertMsg("QR scan failed: " + (e && e.message || e)); return; }
      if(text) onText(text);
    });
    anchor.insertAdjacentElement("afterend", b);
  }
  // Messages through the web app's own toast element (#toast, util/dom.js toast()).
  let toastTimer = 0;
  function showToast(msg, err){
    const t = document.getElementById("toast");
    if(!t){ console.warn(msg); return; }
    t.textContent = msg; t.className = "toast show" + (err ? " err" : ""); t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.classList.remove("show"); t.hidden = true; }, 4500);
  }
  const alertMsg = msg => showToast(msg, true);
  const okMsg = msg => showToast(msg, false);
  // A small yes/no dialog in the web app's dialog style.
  function confirmDlg(title, msg, yes){
    return new Promise(resolve => {
      const d = document.createElement("dialog");
      d.innerHTML = '<div class="modalhdr"><strong></strong></div><div class="sub" style="margin:.4em 0 .8em"></div>' +
        '<div class="fsactions dlgactions"><button class="secondary" data-a="no">Cancel</button> <button class="danger" data-a="yes"></button></div>';
      d.querySelector("strong").textContent = title;
      d.querySelector(".sub").textContent = msg;
      d.querySelector('[data-a="yes"]').textContent = yes || "OK";
      const end = v => { try{ d.close(); }catch(e){} d.remove(); resolve(v); };
      d.querySelector('[data-a="no"]').addEventListener("click", () => end(false));
      d.querySelector('[data-a="yes"]').addEventListener("click", () => end(true));
      d.addEventListener("cancel", e => { e.preventDefault(); end(false); });
      document.body.append(d); d.showModal();
    });
  }
  const fillAndClick = (inputId, btnId) => text => {
    const inp = document.getElementById(inputId), btn = document.getElementById(btnId);
    if(!inp || !btn) return;
    inp.value = text;
    inp.dispatchEvent(new Event("input", { bubbles:true }));
    btn.click();
  };

  function wireQr(){
    // Settings -> Scanner keys, and the "Scanner key needed" dialog: the paste box + its own button do the rest.
    addScanButton("keyAddBtn", fillAndClick("keyPaste", "keyAddBtn"));
    addScanButton("authSaveBtn", fillAndClick("authPaste", "authSaveBtn"));
    // Settings -> Remote Server: the scan-analyzer pairing QR is a link carrying #server=…&token=…. Only that
    // kind is accepted here, and replacing a server that is already paired asks first (SEC-A2: a QR shown by
    // someone else would otherwise send every later upload to their server).
    addScanButton("rsPairBtn", async text => {
      const frag = text.indexOf("#") >= 0 ? text.slice(text.indexOf("#") + 1) : "";
      const m = /(?:^|&)server=([^&]*)/.exec(frag);
      if(!m){ alertMsg("That QR is not a server pairing link."); return; }
      let host = "";
      try{ host = new URL(decodeURIComponent(m[1])).host; }catch(e){ alertMsg("That QR has a bad server address."); return; }
      let cur = "";
      try{ cur = new URL(localStorage.getItem("rs.server") || "").host; }catch(e){}
      const paired = (() => { try{ return !!localStorage.getItem("rs.key"); }catch(e){ return false; } })();
      if(paired && cur && cur !== host &&
         !(await confirmDlg("Replace the paired server?", "You are paired with " + cur + ". This QR pairs the app with " + host +
            " instead, and every later upload goes there. Only continue if " + host + " is your own server.", "Replace"))) return;
      // Same link scanned twice: an unchanged hash fires no hashchange, so clear it first.
      if(location.hash.replace(/^#/, "") === frag){ location.hash = ""; setTimeout(() => { location.hash = frag; }, 0); }
      else location.hash = frag;
    });
  }

  // ---------- downloads -> Download/COINTEL-FLO/ (CfloFilesPlugin, android/app/src/main/java/...) ----------
  // The web app saves with a temporary <a download href="blob:…"> it clicks and revokes at once (util/dom.js
  // saveBlob); the web view ignores those. Remember each blob behind its URL, catch the click, and write the blob
  // to the phone's Downloads in base64 chunks. Saves run one after another (a scan can save two files in a row).
  const blobs = new Map();
  const origCreate = URL.createObjectURL.bind(URL), origRevoke = URL.revokeObjectURL.bind(URL);
  URL.createObjectURL = function(o){ const u = origCreate(o); if(o instanceof Blob) blobs.set(u, o); return u; };
  URL.revokeObjectURL = function(u){ blobs.delete(u); return origRevoke(u); };

  const CHUNK = 3 * 256 * 1024;   // a multiple of 3, so each chunk's base64 stands alone
  const b64Of = blob => new Promise((res, rej) => {
    const r = new FileReader();
    r.onload = () => { const s = String(r.result); res(s.slice(s.indexOf(",") + 1)); };
    r.onerror = () => rej(r.error || new Error("read failed"));
    r.readAsDataURL(blob);
  });
  let saveTail = Promise.resolve();
  function saveFile(blob, name){
    const F = P.CfloFiles;
    if(!F){ alertMsg("Saving files is missing from this build."); return; }
    const run = saveTail.then(async () => {
      const { id } = await F.begin({ name, mime: blob.type || "application/octet-stream" });
      try{
        for(let off = 0; off < blob.size; off += CHUNK) await F.append({ id, data: await b64Of(blob.slice(off, off + CHUNK)) });
        const r = await F.finish({ id });
        okMsg("Saved to Downloads/COINTEL-FLO: " + name + " (" + Math.round(((r && r.bytes) || blob.size) / 1024) + " KB)");
      }catch(e){ F.abort({ id }).catch(() => {}); throw e; }
    }).catch(e => alertMsg("Could not save " + name + ": " + ((e && e.message) || e)));
    saveTail = run;
  }
  function downloadOf(a){
    if(!a || !a.hasAttribute || !a.hasAttribute("download")) return null;
    const b = blobs.get(a.href);
    return b ? { blob: b, name: a.getAttribute("download") || "download.bin" } : null;
  }
  const origClick = HTMLAnchorElement.prototype.click;
  HTMLAnchorElement.prototype.click = function(){
    const d = downloadOf(this);
    if(d){ saveFile(d.blob, d.name); return; }
    return origClick.call(this);
  };
  document.addEventListener("click", ev => {   // a download link the user taps (none today, but cheap to cover)
    const a = ev.target && ev.target.closest && ev.target.closest("a[download]");
    const d = downloadOf(a);
    if(d){ ev.preventDefault(); saveFile(d.blob, d.name); }
  }, true);

  // ---------- notifications (features/notify.js) -> native local notifications ----------
  // The web view can't show web notifications. notify.js shows them through a service-worker registration
  // (reg.showNotification) and listens for a "cflo-notify-click" message when one is tapped; give it a stand-in
  // registration backed by @capacitor/local-notifications, and post that message on a tap.
  const LN = P.LocalNotifications;
  if(LN){
    const PERM_KEY = "cflo.app.notifPerm";
    const mapPerm = d => d === "granted" ? "granted" : d === "denied" ? "denied" : "default";
    let perm = "default";
    try{ perm = localStorage.getItem(PERM_KEY) || "default"; }catch(e){}
    const setPerm = p => { perm = p; try{ localStorage.setItem(PERM_KEY, p); }catch(e){} };
    const refreshPerm = () => LN.checkPermissions().then(r => setPerm(mapPerm(r && r.display))).catch(() => {});
    refreshPerm();
    document.addEventListener("visibilitychange", () => { if(document.visibilityState === "visible") refreshPerm(); });

    // A tag (notify.js: one per kind / device) maps to a stable id, so a newer one replaces the older like the web.
    // Style: notify.js passes opts.cfloStyle = {kind, name, sound, vibration, lockscreen} (Settings -> Notifications,
    // "Alert style"). Android fixes a channel's sound / vibration / lock-screen visibility when it is created, so each
    // distinct style is its own channel "cflo-<kind>-<hash>" (CfloNotifyPlugin.java makes it and deletes the kind's
    // older ones). Lock screen is off unless the style turns it on (bystanders' MACs / names; SEC-A1).
    const NP = P.CfloNotify;
    const hashOf = s => { let h = 5381; for(const ch of s) h = (h * 33 + ch.charCodeAt(0)) | 0; return (h >>> 0).toString(36); };
    const idOf = tag => { let h = 7; for(const ch of String(tag || "cflo")) h = (h * 31 + ch.charCodeAt(0)) | 0; return Math.abs(h) % 2000000000 || 1; };
    const KIND_NAME = { threat:"Threats", watch:"Watched devices", follow:"Possible followers", rid:"Drones (Remote-ID)",
      deauth:"Deauth floods", evil:"Evil-twin access points", general:"Scanner alerts" };
    // No cache on purpose: making a style deletes the kind's other channels, so a remembered id could point at a
    // deleted channel (Android then drops the notification without an error). ensureChannel is cheap when it exists.
    function channelFor(st){
      st = st || {};
      const kind = /^[a-z]{1,16}$/.test(st.kind || "") ? st.kind : "general";
      const sound = typeof st.sound === "string" ? st.sound : "default";
      const vibration = Array.isArray(st.vibration) ? st.vibration.slice(0, 48).map(v => Math.max(0, Math.min(10000, Math.round(+v) || 0))) : null;
      const lockscreen = !!st.lockscreen;
      const id = "cflo-" + kind + "-" + hashOf(JSON.stringify([sound, vibration, lockscreen]));
      if(!NP) return Promise.resolve(id);
      return NP.ensureChannel({ id, kind, name: st.name || KIND_NAME[kind] || "Scanner alerts",
        description: "COINTEL-FLO scanner alerts", sound, vibration, lockscreen }).then(() => id);
    }
    async function notify(title, opts){
      opts = opts || {};
      if(perm !== "granted") throw new Error("notifications not allowed");
      const channelId = await channelFor(opts.cfloStyle);
      const id = idOf(opts.tag);
      // The plugin posts with "alert only once": an update of a notification still in the shade is silent. renotify
      // (notify.js always asks for it) = take the old one away first, so the new one sounds / vibrates again.
      if(opts.renotify && LN.removeDeliveredNotifications){
        try{ await LN.removeDeliveredNotifications({ notifications: [{ id, title: "", body: "" }] }); }catch(e){}
      }
      await LN.schedule({ notifications: [{
        id, title: String(title || "COINTEL-FLO"), body: String(opts.body || ""),
        extra: opts.data || {}, smallIcon: "ic_stat_cflo", iconColor: "#AA00AA", channelId,
      }] });
    }
    // For notify.js's Alert style controls: the phone's sound picker and a vibration preview (app only).
    if(NP){
      window.__cfloApp = Object.assign(window.__cfloApp || {}, { notifyStyle: {
        pickSound: (current, title) => NP.pickSound({ current: current == null ? "default" : String(current), title: title || "Alert sound" }),
        soundTitle: uri => NP.soundTitle({ uri: uri == null ? "default" : String(uri) }).then(r => r.title),
        vibrate: pattern => NP.vibrate({ pattern: Array.isArray(pattern) ? pattern : [] }).catch(() => {}),
        stopVibrate: () => NP.stopVibrate().catch(() => {}),
        lockscreen: true,
      } });
    }
    // Counted for the background log (native/shim-background.js).
    const diag = (window.__cfloApp = window.__cfloApp || {}).diag = Object.assign({ ble:0, dets:0, shown:0, failed:0 }, window.__cfloApp.diag);
    const origNotify = notify;
    notify = async (title, opts) => {
      try{ const r = await origNotify(title, opts); diag.shown++; return r; }
      catch(e){
        diag.failed++;
        try{ if(P.CfloBg) P.CfloBg.diagNote({ m: "alert failed: " + String((e && e.message) || e).slice(0, 80) }); }catch(x){}
        throw e;
      }
    };
    class AppNotification {
      constructor(title, opts){ notify(title, opts).catch(e => console.warn("notification:", e.message)); }
      static get permission(){ return perm; }
      static async requestPermission(){
        try{ setPerm(mapPerm((await LN.requestPermissions()).display)); }catch(e){}
        return perm;
      }
    }
    window.Notification = AppNotification;

    const sw = new EventTarget();
    const reg = { scope: location.href, showNotification: (title, opts) => notify(title, opts) };
    sw.register = async () => reg;
    sw.ready = Promise.resolve(reg);
    sw.getRegistration = async () => reg;
    try{ Object.defineProperty(navigator, "serviceWorker", { value: sw, configurable: true }); }catch(e){}

    C.addListener("LocalNotifications", "localNotificationActionPerformed", a => {
      const x = (a && a.notification && a.notification.extra) || {};
      const fire = () => sw.dispatchEvent(new MessageEvent("message", { data: { type: "cflo-notify-click", mac: x.mac || "", kind: x.kind || "" } }));
      if(document.readyState === "complete") fire(); else window.addEventListener("load", fire, { once: true });
    });
  }

  // ---------- Android back gesture ----------
  // An open dialog closes first (its own "cancel" handling runs, like Esc in a browser); otherwise back walks the
  // page history (Settings subpages push entries); at the top the app goes to the background instead of closing,
  // so the scanner link stays up.
  if(P.App){
    C.addListener("App", "backButton", ev => {
      const open = [...document.querySelectorAll("dialog[open]")];
      const top = open[open.length - 1];
      if(top){
        const c = new Event("cancel", { cancelable: true });
        if(top.dispatchEvent(c)) top.close();
        return;
      }
      if(ev && ev.canGoBack) history.back();
      else P.App.minimizeApp().catch(() => {});
    });
  }

  // ---------- outside pages open in the phone's browser ----------
  const origOpen = window.open.bind(window);
  window.open = function(url, target, features){
    try{
      const u = new URL(String(url), location.href);
      if((u.protocol === "http:" || u.protocol === "https:") && u.host !== location.host && P.Browser){
        P.Browser.open({ url:u.href }).catch(() => {});
        return null;
      }
    }catch(e){}
    return origOpen(url, target, features);
  };

  if(document.readyState === "loading") document.addEventListener("DOMContentLoaded", wireQr);
  else wireQr();
  window.__cfloApp = Object.assign(window.__cfloApp || {}, { platform:true });
})();
