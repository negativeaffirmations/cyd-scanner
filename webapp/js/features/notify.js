// Phone notifications for scanner events (Settings -> Notifications). LOCAL notifications raised by this page from
// the data it already receives over BLE: no push server. Android Chrome only shows them through a service worker
// (../notify-sw.js, which only shows / focuses and caches nothing).
//
// What can alert (each a rule, saved on this phone in localStorage "cflo.notify"):
//   - a threat CATEGORY (Flock, Cam, ...): a row of that category at tier >= suspect, not whitelisted
//   - a threat TIER floor (suspect / likely / confirmed or worse), any category
//   - a WATCHED device (by MAC, added from the device popup), alerts even when whitelisted
//   - a possible FOLLOWER (follow tier 2), a drone Remote-ID emitter (flag bit 128)
//   - a deauth flood / evil-twin AP (STATUS deauth= / evil=, the same thresholds as the red banner)
// Cooldown: a device alerts when it APPEARS: first seen, or back after being gone for the quiet time. A device that
// keeps dropping in and out (a camera probing every 5 min) alerts once, not every burst. One notification per
// snapshot: several new devices at once are folded into one.
//
// Hooks: none in the old modules. This polls state.lastDetRows (replaced on every DETS snapshot) and state.status
// (merged STATUS keys) once a second, and watches #detInfo for the device popup. So a phone holding stale cached
// copies of detections.js / status.js / categories.js still gets notifications (the stale-module rule).
// Alert style (details / lock screen / sound / vibration, for all alerts and per type): notifystyle.js.
// Limit: only while this page runs. Android freezes a screen-off / long-backgrounded tab, and then nothing fires.
import { state } from '../state.js';
import { $, esc, log, toast } from '../util/dom.js';
import { CAT_COL, TIER_NAMES, DEAUTH_ALERT } from '../config.js';
import { openLiveDetail } from './categories.js';
import { showPage } from '../nav.js';
import { cleanStyle, styleFor, plainTitle, PLAIN_BODY, initStyleUI } from './notifystyle.js';
import { initRules, evalRules, coveredKinds, coveredEvent, tickRules } from './notifyrules.js';   // custom notifications

// DETS `cat` id -> name (CYD Category enum order). Local copy on purpose: categories.js keeps its own private.
const CAT_NAMES = ["","Flock","Axon","ALPR","Cam","Ring","Raven","Glass","Tracker","Drone","Deauth","Flipper","Skim","Other"];
const CAT_SHOWN = 12;
const RID_FLAG = 128;
const KEY = "cflo.notify";
const WATCH_MAX = 50;
const DEFAULTS = { on:false, cats:[1,2,3,6], tierMin:3, follow:true, rid:true, deauth:true, evil:true, quietMin:15,
                   vibrate:true, screen:true, watch:[] };

let cfg = load();
let swReg = null;          // ServiceWorkerRegistration once registered, else null (desktop falls back to new Notification)
let lastRows = null;       // the state.lastDetRows array last evaluated (a new snapshot replaces the array)
const seen = new Map();    // "mac|kind" -> last ms that rule matched that device
let evState = { deauth:false, evil:false, deauthAt:0, evilAt:0 };

function load(){
  try{
    const o = JSON.parse(localStorage.getItem(KEY) || "null");
    if(o && typeof o === "object") return cleanStyle({ ...DEFAULTS, ...o, watch: Array.isArray(o.watch) ? o.watch : [] });
  }catch(_){}
  return cleanStyle({ ...DEFAULTS, watch:[] });
}
function save(){ try{ localStorage.setItem(KEY, JSON.stringify(cfg)); }catch(_){} }
const supported = () => "Notification" in window;
const normMac = m => String(m || "").trim().toUpperCase();
const quietMs = () => Math.max(1, +cfg.quietMin || 15) * 60000;

// ---- showing ----
async function ensureSw(){
  if(swReg || !("serviceWorker" in navigator)) return swReg;
  try{
    swReg = await navigator.serviceWorker.register("notify-sw.js");   // scope = this page's folder
    await navigator.serviceWorker.ready;
  }catch(e){ log("Notifications: service worker failed ("+e.message+")."); swReg = null; }
  return swReg;
}
// kind: a notifystyle.js KINDS id, "many" (kinds = the folded types) or "general"; picks the alert style.
// quiet = no sound, no vibration (an alert the server explains, features/screen.js).
// kind may also be a ready style object (a custom notification's own, features/notifyrules.js).
async function show(title, body, data, tag, kind, kinds, quiet){
  if(!supported() || Notification.permission !== "granted") return false;
  let st = kind && typeof kind === "object" ? kind : styleFor(cfg, kind || "general", kinds);
  if(typeof kind === "object") kind = "general";
  if(quiet) st = { ...st, sound:"", vibration:[] };
  if(!st.details){ title = plainTitle(kind || "general"); body = PLAIN_BODY; }   // nothing about the device in the text
  const opts = { body, tag: tag || "cflo", renotify: true, data: data || {}, cfloStyle: st };
  if(st.vibration && st.vibration.length) opts.vibrate = st.vibration;
  if(st.sound === "") opts.silent = true;
  const reg = await ensureSw();
  try{
    if(reg) await reg.showNotification(title, opts);
    else new Notification(title, opts);   // desktop Chrome without a worker; Android has no constructor
    return true;
  }catch(e){ log("Notification failed: "+e.message); return false; }
}

// ---- matching ----
const tierOf = r => typeof r.tier === "number" ? r.tier : 0;
function reasons(r){
  const out = [], tn = tierOf(r), mac = normMac(r.mac);
  if(cfg.watch.some(w => w.mac === mac)) out.push("watch");
  if(r.muted) return out;                                   // whitelisted: only an explicit watch still alerts
  if(tn >= 1 && cfg.cats.includes(r.cat || 0)) out.push("cat");
  if(cfg.tierMin > 0 && tn >= cfg.tierMin) out.push("tier");
  if(cfg.follow && (r.ftier || 0) === 2) out.push("follow");
  if(cfg.rid && (r.flags & RID_FLAG)) out.push("rid");
  return out;
}
function devLine(r){
  const nm = r.name && normMac(r.name) !== normMac(r.mac) ? r.name : "";
  return [CAT_NAMES[r.cat || 0] && r.cat ? CAT_NAMES[r.cat] : "", nm || r.mac, r.rssi + " dBm"].filter(Boolean).join(" · ");
}
const styleKind = k => k === "cat" || k === "tier" ? "threat" : k;   // reasons() kinds -> alert types
function titleFor(r, kinds){
  if(kinds.includes("watch")){ const w = cfg.watch.find(x => x.mac === normMac(r.mac)); return "Watched device: " + (w && w.label || r.mac); }
  if(kinds.includes("follow")) return "Possible follower";
  if(kinds.includes("rid")) return "Drone Remote-ID nearby";
  const tn = tierOf(r), cat = CAT_NAMES[r.cat || 0];
  return (r.cat && r.cat <= CAT_SHOWN ? cat : "Threat") + " nearby" + (tn >= 1 ? " (" + TIER_NAMES[tn] + ")" : "");
}

function evalSnapshot(rows){
  evalRules(rows);   // custom notifications first: what one of them handles gets no second (quick) alert
  const now = Date.now(), q = quietMs(), fresh = [];
  for(const r of rows){
    const kinds = reasons(r); if(!kinds.length) continue;
    const mac = normMac(r.mac), newKinds = [];
    for(const k of kinds){
      const key = mac + "|" + k, last = seen.get(key);
      if(last === undefined || now - last > q) newKinds.push(k);
      seen.set(key, now);
    }
    const cov = coveredKinds(mac);                           // only the kinds a custom rule handles for this device
    const left = cov ? newKinds.filter(k => !cov.has(k)) : newKinds;
    if(left.length) fresh.push({ r, kinds:left });
  }
  for(const [k, t] of seen) if(now - t > q) seen.delete(k);   // forget long-gone devices
  if(!fresh.length) return;
  fresh.sort((a, b) => tierOf(b.r) - tierOf(a.r) || b.r.rssi - a.r.rssi);
  // Weak signature alerts are checked with the paired server first (features/screen.js); everything else goes out now.
  const held = fresh.filter(f => state.screen && cfg.screen && f.kinds.every(k => k === "cat" || k === "tier") && state.screen.wants(f.r));
  for(const f of held) state.screen.device(f.r).then(v => {
    if(!cfg.on || !state.connected) return;
    if(v && v.low){
      markAlerted(f, true);
      log("Alert (probably false): " + titleFor(f.r, f.kinds) + " - " + devLine(f.r) + " - " + v.why);
      show("Probably a false alarm: " + titleFor(f.r, f.kinds), devLine(f.r) + "\n" + v.why, { mac:f.r.mac, kind:f.kinds[0] },
           "cflo-" + normMac(f.r.mac), "threat", undefined, true);
    } else present([f]);
  });
  const now2 = fresh.filter(f => !held.includes(f));
  if(now2.length) present(now2);
}
// Which devices raised an alert (state.alerted, MAC -> {t, low, kinds}): features/threattag.js puts a bell on their row.
function markAlerted(f, low){
  if(!state.alerted) state.alerted = new Map();
  state.alerted.set(normMac(f.r.mac), { t:Date.now(), low:!!low, kinds:f.kinds.slice() });
  if(state.alerted.size > 300) state.alerted.delete(state.alerted.keys().next().value);
}
function present(fresh){
  fresh.forEach(f => { markAlerted(f, false); log("Alert: " + titleFor(f.r, f.kinds) + " - " + devLine(f.r)); });
  if(fresh.length === 1){
    const { r, kinds } = fresh[0];
    const sk = [...new Set(kinds.map(styleKind))];
    show(titleFor(r, kinds), devLine(r) + (r.mac && devLine(r).indexOf(r.mac) < 0 ? "\n" + r.mac : ""),
         { mac:r.mac, kind:kinds[0] }, "cflo-" + normMac(r.mac), sk.length > 1 ? "many" : sk[0], sk);
  } else {
    const tagOf = k => k.includes("watch") ? "Watched: " : k.includes("follow") ? "Follower: " : k.includes("rid") ? "Drone: " : "";
    show(fresh.length + " new alerts", fresh.slice(0, 5).map(f => tagOf(f.kinds) + devLine(f.r)).join("\n") +
         (fresh.length > 5 ? "\n+" + (fresh.length - 5) + " more" : ""), { kind:"many" }, "cflo-many",
         "many", [...new Set(fresh.flatMap(f => f.kinds.map(styleKind)))]);
  }
}
function evalStatus(){
  const s = state.status || {}, now = Date.now(), q = quietMs();
  const da = +s.deauth || 0, ev = +s.evil || 0;
  const daOn = da >= DEAUTH_ALERT, evOn = ev > 0;
  if(cfg.deauth && daOn && !evState.deauth && now - evState.deauthAt > q && !coveredEvent("deauth")){
    evState.deauthAt = now; log("Alert: deauth flood " + da + " frames/s");
    show("Deauth flood", da + " deauth / disassoc frames per second nearby", { kind:"deauth" }, "cflo-deauth", "deauth");
  }
  if(cfg.evil && evOn && !evState.evil && now - evState.evilAt > q && !coveredEvent("evil")){
    evState.evilAt = now;
    (state.screen && cfg.screen ? state.screen.evil(ev) : Promise.resolve(null)).then(v => {
      if(!cfg.on || !state.connected) return;
      log("Alert: evil twin " + ev + " SSID(s)" + (v && v.low ? " (probably false: " + v.why + ")" : ""));
      if(v && v.low){ show("Probably a false alarm: evil-twin access point", v.why, { kind:"evil" }, "cflo-evil", "evil", undefined, true); return; }
      show("Evil-twin access point", ev + " SSID" + (ev > 1 ? "s" : "") + " broadcast by APs with different security", { kind:"evil" }, "cflo-evil", "evil");
    });
  }
  evState.deauth = daOn; evState.evil = evOn;
}
function tick(){
  if(!cfg.on || !state.connected || !supported() || Notification.permission !== "granted") return;
  const rows = state.lastDetRows;
  if(rows && rows !== lastRows){ lastRows = rows; evalSnapshot(rows); }
  tickRules();       // custom notifications: position samples ("while moving") + deauth / evil-twin triggers
  evalStatus();
}

// ---- Settings card ----
function stateText(){
  if(!supported()) return ["This browser can't show notifications.", "warn"];
  const p = Notification.permission;
  if(p === "denied") return ["Notifications are blocked for this site. Allow them in the browser's site settings, then turn them on here.", "warn"];
  if(!cfg.on) return ["Off.", ""];
  if(p !== "granted") return ["Waiting for permission.", "warn"];
  const a = window.__cfloApp;   // the Android app keeps alerting in the background while its service runs
  const bg = !!(a && a.background && (!a.bgRunning || a.bgRunning()));
  return [state.connected ? (bg ? "On: alerting while a scanner is connected, also with the screen off." : "On: alerting while this page is open.")
                          : "On: alerts start when a scanner is connected.", "ok"];
}
function renderState(){   // the status line + the two buttons (safe to repaint any time)
  const st = $("nfState"); if(!st) return;
  const [t, cls] = stateText();
  st.textContent = t; st.className = "sub keystate " + cls;
  const live = cfg.on && supported() && Notification.permission === "granted";
  $("nfEnableBtn").textContent = live ? "Turn notifications off" : "Turn notifications on";
  $("nfEnableBtn").disabled = !supported();
  $("nfTestBtn").disabled = !live;
}
function renderCard(){
  if(!$("nfState")) return;
  renderState();
  let html = "";
  for(let c = 1; c <= CAT_SHOWN; c++){
    const on = cfg.cats.includes(c), col = CAT_COL[c] || "#8b949e";
    html += `<button class="tcard${on ? " hot" : ""}" data-cat="${c}" aria-pressed="${on}" ` +
      `style="border-color:${col};${on ? `background:${col}22;` : ""}">` +
      `<span class="tn">${esc(CAT_NAMES[c])}</span><span class="tc" style="color:${on ? col : ""}">${on ? "on" : "off"}</span>` +
      `<span class="tmenu material-symbols-outlined" data-nrmenu="${c}" title="Custom notification">more_vert</span></button>`;
  }
  $("nfCats").innerHTML = html;
  $("nfTier").value = String(cfg.tierMin);
  $("nfQuiet").value = String(cfg.quietMin);
  $("nfFol").checked = !!cfg.follow; $("nfRid").checked = !!cfg.rid;
  $("nfDeauth").checked = !!cfg.deauth; $("nfEvil").checked = !!cfg.evil;
  if($("nfScreen")) $("nfScreen").checked = !!cfg.screen;
  $("nfWatch").innerHTML = cfg.watch.length ? cfg.watch.map(w =>
    `<div class="sessrow keyrow"><span class="material-symbols-outlined">notifications_active</span>` +
    `<div class="sessmain"><span class="sessdate">${esc(w.label || w.mac)}</span><span class="sesssub">${esc(w.mac)}</span></div>` +
    `<button class="danger keyforget" data-mac="${esc(w.mac)}">Remove</button></div>`).join("")
    : '<div class="sub" style="padding:6px;margin:0">None. Open a device and tap "Notify me when seen".</div>';
}
async function toggleOn(){
  if(!supported()) return;
  if(cfg.on && Notification.permission === "granted"){ cfg.on = false; save(); renderCard(); return; }
  let p = Notification.permission;
  if(p === "default") p = await Notification.requestPermission();   // needs this tap (a user gesture)
  if(p !== "granted"){ cfg.on = false; save(); renderCard(); toast("Notifications not allowed by the browser.", { err:true }); return; }
  cfg.on = true; save();
  await ensureSw();
  lastRows = null;   // evaluate the current list right away
  renderCard();
  toast("Notifications on.");
}
function onCardChange(){
  cfg.tierMin = +$("nfTier").value || 0;
  cfg.quietMin = +$("nfQuiet").value || 15;
  cfg.follow = $("nfFol").checked; cfg.rid = $("nfRid").checked;
  cfg.deauth = $("nfDeauth").checked; cfg.evil = $("nfEvil").checked;
  if($("nfScreen")) cfg.screen = $("nfScreen").checked;
  save();
}

// ---- device popup button (#detDlg, live and Explore detail) ----
function popupDevice(){
  const out = {};
  document.querySelectorAll("#detInfo .kv").forEach(kv => {
    const k = kv.querySelector(".k"), v = kv.querySelector(".vv");
    if(k && v) out[k.textContent.trim()] = v.textContent.trim();
  });
  return out.MAC ? { mac:normMac(out.MAC), name:out.Name || "" } : null;
}
function renderPopupButton(){
  const el = $("detNotify"); if(!el) return;
  const d = popupDevice();
  if(!d || !supported()){ el.innerHTML = ""; return; }
  const on = cfg.watch.some(w => w.mac === d.mac);
  el.innerHTML = `<button class="secondary" id="detNotifyBtn">${on ? "Stop notifying for this device" : "Notify me when seen"}</button>`;
  $("detNotifyBtn").onclick = async () => {
    if(cfg.watch.some(w => w.mac === d.mac)){
      cfg.watch = cfg.watch.filter(w => w.mac !== d.mac); save();
      toast("No longer watching " + d.mac + ".");
    } else {
      if(cfg.watch.length >= WATCH_MAX){ toast("Watching " + WATCH_MAX + " devices already. Remove one in Settings.", { err:true }); return; }
      const label = d.name && normMac(d.name) !== d.mac ? d.name : "";
      cfg.watch.push({ mac:d.mac, label, added:Date.now() }); save();
      if(!cfg.on || Notification.permission !== "granted") await toggleOn();   // this tap doubles as the permission gesture
      if(cfg.on) toast("You'll be notified when " + (label || d.mac) + " appears.");
      // Already in range: don't fire for it now, only when it next appears.
      if(state.lastDetRows && state.lastDetRows.some(r => normMac(r.mac) === d.mac)) seen.set(d.mac + "|watch", Date.now());
    }
    renderPopupButton(); renderCard();
  };
}

// A test with one alert type's style (Alert style -> a type -> Send a test).
const TEST_TEXT = { threat:["Flock nearby (Likely)", "Flock · Example camera · -62 dBm\nAA:BB:CC:00:11:22"],
  watch:["Watched device: Example", "Example · -70 dBm\nAA:BB:CC:00:11:22"], follow:["Possible follower", "Example tracker · -58 dBm"],
  rid:["Drone Remote-ID nearby", "Drone · -75 dBm"], deauth:["Deauth flood", "40 deauth / disassoc frames per second nearby"],
  evil:["Evil-twin access point", "1 SSID broadcast by APs with different security"] };
async function testKind(kind){
  if(!cfg.on || !supported() || Notification.permission !== "granted"){ toast("Turn notifications on first.", { err:true }); return; }
  const [t, b] = TEST_TEXT[kind] || ["Test notification", "Scanner alerts will look like this."];
  if(await show("Test: " + t, b + "\n(This is only a test.)", { kind:"test" }, "cflo-test-" + kind, kind)) toast("Test sent.");
}

// ---- tapping a notification ----
function onSwMessage(e){
  const m = e.data || {};
  if(m.type !== "cflo-notify-click") return;
  if(m.mac){
    const r = (state.lastDetRows || []).find(x => normMac(x.mac) === normMac(m.mac));
    showPage("scanner");
    if(r) openLiveDetail(r);
    else toast(m.mac + " is out of range now.");
  } else showPage(m.kind === "many" ? "scanner" : "home");
}

export function initNotify(){
  const card = $("nfCard"); if(!card) return;
  // For the phone's own analysis alerts (analysis/alerts.js): the same switches, permission, style and tap handling.
  // A state hook, not an export (stale-module rule). rule = a cfg switch ("follow" / "evil" / "deauth") or "".
  state.notifyShow = (rule, title, body, data, tag, kind, quiet) => {
    if(!cfg.on || !supported() || Notification.permission !== "granted" || !state.connected) return Promise.resolve(false);
    if(rule && !cfg[rule]) return Promise.resolve(false);
    log("Alert: " + title + " - " + String(body).split("\n")[0]);
    return show(title, body, data, tag, kind, undefined, quiet);
  };
  state.notifyQuietMs = quietMs;
  state.notifyScreenOn = () => !!cfg.screen;
  $("nfEnableBtn").onclick = toggleOn;
  $("nfTestBtn").onclick = async () => {
    if(await show("Test notification", "Scanner alerts will look like this.", { kind:"test" }, "cflo-test", "general")) toast("Test sent.");
  };
  $("nfCats").addEventListener("click", e => {
    const b = e.target.closest(".tcard"); if(!b) return;
    const c = +b.dataset.cat;
    cfg.cats = cfg.cats.includes(c) ? cfg.cats.filter(x => x !== c) : cfg.cats.concat(c);
    save(); renderCard();
  });
  ["nfTier","nfQuiet","nfFol","nfRid","nfDeauth","nfEvil","nfScreen"].forEach(id => $(id) && $(id).addEventListener("change", onCardChange));
  initStyleUI(cfg, save, testKind);
  initRules(cfg, save, { show: (t, b, d, tag, st, quiet) => show(t, b, d, tag, st, undefined, quiet), quietMs,
    ensureOn: async () => { if(!(cfg.on && supported() && Notification.permission === "granted")) await toggleOn();
                            return cfg.on && supported() && Notification.permission === "granted"; } });
  $("nfWatch").addEventListener("click", e => {
    const b = e.target.closest(".keyforget"); if(!b) return;
    cfg.watch = cfg.watch.filter(w => w.mac !== b.dataset.mac); save(); renderCard();
  });
  const info = $("detInfo");
  if(info) new MutationObserver(renderPopupButton).observe(info, { childList:true });
  if("serviceWorker" in navigator) navigator.serviceWorker.addEventListener("message", onSwMessage);
  if(cfg.on && supported() && Notification.permission === "granted") ensureSw();
  document.addEventListener("visibilitychange", renderState);   // permission may have changed in site settings
  renderCard();
  setInterval(tick, 1000);
  // The Android app (android-app/native/shim-background.js) sends this every second while it runs in the background,
  // where the hidden page's own timers are throttled; tick() is safe to run twice.
  window.addEventListener("cflo-bg-tick", tick);
  setInterval(renderState, 5000);   // the connected / not-connected line only (never the selects mid-edit)
}
