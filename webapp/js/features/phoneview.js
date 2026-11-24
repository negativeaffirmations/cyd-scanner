// "Phone analysis" in the device popup (#detDlg): what the phone's own analysis (analysis/engine.js) makes of this
// device over the whole current scan and every fleet unit, next to what the scanner reported.
// Like serverhist.js / notify.js it hooks nothing in older modules: it watches #detInfo (rewritten on every popup
// open) and reads the MAC from its rows. NEW file (stale-module rule).
// Epochs are the logs' LOCAL seconds, so they are shown with UTC getters (the clock time the scanner showed).
// The engine is reached through state.engine (set by initEngine), never imported here: main.js loads engine.js under a
// ?v= token, and a plain import here would be a SECOND module instance that never receives any log lines.
import { $, esc } from '../util/dom.js';
import { state } from '../state.js';
const analysisFor = mac=>state.engine ? state.engine.analysisFor(mac) : null;
const H = ()=>state.history || null;   // analysis/history.js (saved scans), same reasoning as state.engine

let curMac = "";

function popupMac(){
  for(const row of document.querySelectorAll("#detInfo .kv")){
    const k = row.querySelector(".k"), v = row.querySelector(".vv");
    if(k && v && k.textContent.trim() === "MAC") return v.textContent.trim().toUpperCase();
  }
  return "";
}
function kv(k, v, cls){ return `<div class="kv"><span class="k">${esc(k)}</span><span class="vv${cls ? " " + cls : ""}">${v}</span></div>`; }
const hm = t=>{ if(!t) return "?"; const d = new Date(t * 1000); return String(d.getUTCHours()).padStart(2, "0") + ":" + String(d.getUTCMinutes()).padStart(2, "0"); };
const km = m=>m >= 1000 ? (m / 1000).toFixed(1) + " km" : Math.round(m) + " m";
const ymd = t=>t ? new Date(t * 1000).toISOString().slice(0, 10) : "?";
const EVT_TXT = { eviltwin:"Evil-twin alert", deauth:"Deauth burst" };

// "Saved scans on this phone": every log file in the phone's cache (analysis/history.js).
function historyHtml(mac, a){
  const h = H();
  let out = `<div class="srvsub">Saved scans on this phone</div>`;
  if(!h) return out + `<div class="sub">Not available in this version of the page.</div>`;
  const st = h.historyStatus();
  if(st.state === "unavailable") return out + `<div class="sub">This browser can't read the saved logs.</div>`;
  if(st.state !== "ready") return out + `<div class="sub">Reading the saved logs… (${st.done} of ${st.files} files)</div>`;
  const r = h.historyFor(mac);
  if(!r) return out + `<div class="sub">Not in any of the ${st.files} saved log files.</div>`;
  const days = r.days;
  out += kv("Days seen", days.length + (days.length ? ": " + esc(days.slice(-4).join(", ")) + (days.length > 4 ? ", …" : "") : ""));
  if(r.first) out += kv("First seen", ymd(r.first));
  if(r.places.length) out += kv("Places", r.places.length + (r.places.length >= 12 ? "+" : ""));
  if(a && a.lastPos){
    const k = h.nearBefore(mac, a.lastPos.lat, a.lastPos.lon);
    out += kv("At this place", k > 1 ? "seen here on " + k + " days" : k === 1 ? "only this day" : "not seen here before");
  }
  for(const e of r.events)
    out += kv(EVT_TXT[e.evt] || e.evt, esc(ymd(e.t) + " " + hm(e.t) + (e.ssid ? " · " + e.ssid : "")), "warn");
  return out;
}

function render(){
  const el = $("detPhone");
  if(!el) return;
  const mac = curMac;
  if(!mac || !/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/.test(mac)){ el.innerHTML = ""; return; }
  const head = `<div class="srvhead"><span class="material-symbols-outlined">smartphone</span><strong>Phone analysis</strong><span class="sub">&nbsp;this scan</span></div>`;
  const a = analysisFor(mac);
  if(!a){
    el.innerHTML = head + `<div class="sub">Not in this scan's log on the phone yet (a device is logged when first seen, and the phone copies the log as it grows).</div>`
                 + historyHtml(mac, null);
    return;
  }
  const others = a.scanners.filter(s=>s !== a.ownSid).length;
  const heard = (a.scanners.includes(a.ownSid) ? "this scanner" : "") + (others ? (a.scanners.includes(a.ownSid) ? " + " : "") + others + " fleet unit" + (others === 1 ? "" : "s") : "");
  let h = head;
  h += kv("Verdict", esc(a.verdict), a.level === 2 ? "warn" : "");
  h += kv("Seen", a.n + " time" + (a.n === 1 ? "" : "s") + ", " + hm(a.first) + (a.last > a.first ? " – " + hm(a.last) : ""));
  if(a.places) h += kv("Places", a.places + (a.places > 1 ? ", up to " + km(a.spreadM) + " apart" : ""));
  if(a.span > 0) h += kv("You moved", km(a.youMovedM) + " in that time");
  h += kv("Heard by", esc(heard || "?"));
  if(a.rotating) h += `<div class="sub" style="margin-top:.3em">Random Bluetooth address: the device changes it every few minutes, so this only covers the time it used this one.</div>`;
  else if(a.randomWifi) h += `<div class="sub" style="margin-top:.3em">Randomized Wi-Fi address: phones and laptops make these up and change them, so this only covers the time it used this one.</div>`;
  h += historyHtml(mac, a);
  el.innerHTML = h;
}

function onPopup(){ curMac = popupMac(); render(); }

export function initPhoneView(){
  const info = $("detInfo");
  if(info) new MutationObserver(onPopup).observe(info, { childList:true });
  const dlg = $("detDlg");
  if(dlg) dlg.addEventListener("close", ()=>{ curMac = ""; });
  const again = ()=>{ const dlg2 = $("detDlg"); if(curMac && dlg2 && dlg2.open) render(); };
  if(state.engine) state.engine.onEngineChange(again);
  if(state.history) state.history.onHistoryChange(again);
}
