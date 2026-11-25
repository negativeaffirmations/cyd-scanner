// Alert screening with the paired server (scan-analyzer read API, docs/api-contract.md there: GET /devices/{mac}).
// Before a WEAK alert is shown, ask what the server already knows. When that explains it, the alert is still shown, but
// silently and marked "Probably a false alarm" with the reason (owner decision: label or lower, never hide):
//  - Flock / ALPR at tier suspect (one weak signature) on a device that is a client of an ordinary access point the
//    server has known on >= AP_DAYS earlier days: a laptop / TV in that house, not a camera (2026-11-24 drive).
//  - Evil twin: EVERY BSSID of EVERY flagged network was already there on >= AP_DAYS earlier days and none is open
//    (WEP vs WPA: an old router beside a new one). An open twin always alerts, even under a copied address, and so
//    does any access point that isn't in the history.
//  - Phone-analysis follower: the server has it fixed in place (history level confirmed).
// Likely / confirmed tiers, watched devices, scanner follow / Remote-ID / deauth alerts are never screened.
// Fails open: not paired, no read permission, no answer in time, a device the server doesn't know, anything unclear
// -> the normal alert. Settings -> Notifications "Check weak alerts with the paired server first".
// Reached through state.screen (stale-module rule).
import { state } from '../state.js';
import { isPaired, serverUrl } from './remote.js';

const TOTAL_MS = 7500, LINE_WAIT_MS = 3000, CACHE_MS = 10 * 60000, AP_DAYS = 2;
const FLAG_DATA = 256;                  // DETS flags: client heard in a data frame
const CAT_FLOCK = 1, CAT_ALPR = 3;
const cache = new Map();                // MAC -> {t, p: Promise<body|null>}

const sleep = ms=>new Promise(r=>setTimeout(r, ms));
const within = (p, ms)=>Promise.race([p.catch(()=>null), sleep(ms).then(()=>null)]);
const key = ()=>{ try{ return localStorage.getItem("rs.key") || ""; }catch(e){ return ""; } };
const isMac = m=>/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/.test(m);
const today = ()=>new Date(Date.now() - new Date().getTimezoneOffset() * 60000).toISOString().slice(0, 10);

// What the server knows about a MAC; null = unknown, refused, unreachable or too slow. Only answers are cached.
function ask(mac){
  const c = cache.get(mac);
  if(c && Date.now() - c.t < CACHE_MS) return c.p;
  const ctl = new AbortController(), timer = setTimeout(()=>ctl.abort(), TOTAL_MS);
  const p = fetch(serverUrl().replace(/\/+$/, "") + "/api/v1/devices/" + encodeURIComponent(mac),
    { headers:{ "Authorization":"Bearer " + key() }, signal:ctl.signal, cache:"no-store" })
    .then(r=>r.ok ? r.json() : null).catch(()=>null)
    .then(b=>{ if(!b || !b.device) cache.delete(mac); return b && b.device ? b : null; })
    .finally(()=>clearTimeout(timer));
  if(cache.size > 200) cache.clear();
  cache.set(mac, { t:Date.now(), p });
  return p;
}
const earlierDays = b=>{ const d = today(); return (b.timeline || []).filter(x=>x && x.day && x.day < d).length; };

// An ordinary access point known for days: heard as a Wi-Fi AP, no signature of its own.
function ordinaryAp(b){
  const dev = b.device, src = String(dev.sources || "");
  return /2\.4|5G/.test(src) && !dev.threat_cat && (!dev.tier || dev.tier === "none") && earlierDays(b) >= AP_DAYS;
}

// Poll the phone's own copy of the log for something it may not have received yet.
async function waitFor(fn, ms){
  const end = Date.now() + ms;
  for(;;){ const v = fn(); if(v) return v; if(Date.now() >= end) return null; await sleep(250); }
}

const ready = ()=>isPaired() && !!key() && !!state.engine;

// Is this DETS row one the screening can judge? Suspect only, a data-frame client, Flock / ALPR.
function wants(r){
  return ready() && r.tier === 1 && (r.flags & FLAG_DATA) && (r.cat === CAT_FLOCK || r.cat === CAT_ALPR) &&
         isMac(String(r.mac || "").toUpperCase());
}

// The server's own verdict (GET /screen, whoami "screen":1). true = it answered, so nothing else is asked.
async function verdict(mac, ap, sig){
  const ctl = new AbortController(), timer = setTimeout(()=>ctl.abort(), 2500);
  try{
    const q = "mac=" + encodeURIComponent(mac) + "&bssid=" + encodeURIComponent(ap) + "&sig=" + encodeURIComponent(sig || "") + "&tier=suspect";
    const r = await fetch(serverUrl().replace(/\/+$/, "") + "/api/v1/screen?" + q,
      { headers:{ "Authorization":"Bearer " + key() }, signal:ctl.signal, cache:"no-store" });
    const b = r.ok ? await r.json() : null;
    if(!b || b.verdict !== "likely_false") return null;
    return { low:true, why:(Array.isArray(b.reasons) ? b.reasons.filter(x=>typeof x === "string").slice(0, 3).join("; ") : "") || "the server thinks so" };
  }catch(e){ return null; }
  finally{ clearTimeout(timer); }
}

function device(r){
  const mac = String(r.mac).toUpperCase();
  return within((async ()=>{
    const ap = await waitFor(()=>{ const a = state.engine.analysisFor(mac); return a && a.bssid; }, LINE_WAIT_MS);
    if(!ap || !isMac(ap)) return null;
    if(state.serverScreens && await within(state.serverScreens(), 1500)) return verdict(mac, ap, r.sig || (state.engine.analysisFor(mac) || {}).sig);
    const b = await ask(ap);
    if(!b || !ordinaryAp(b)) return null;
    return { low:true, why:"it is a client of " + (b.device.name || ap) + ", an ordinary access point the server knows from " +
                            earlierDays(b) + " earlier days" };
  })(), TOTAL_MS);
}

// Evil twin: judged only when the log lines name every flagged network (count = STATUS evil=).
function evil(count){
  if(!ready() || !state.engine.recentEvents) return Promise.resolve(null);
  return within((async ()=>{
    const list = await waitFor(()=>{
      const l = state.engine.recentEvents("eviltwin", 45).filter(e=>e.alert !== 0 && e.ssid && e.macs);
      return new Set(l.map(e=>e.ssid)).size >= count ? l : null;
    }, LINE_WAIT_MS);
    if(!list) return null;
    if(!list.every(e=>e.enc && String(e.enc).split(",").every(x=>x === "wep" || x === "wpa"))) return null;
    const macs = [...new Set(list.flatMap(e=>String(e.macs).toUpperCase().split(",")))];
    if(macs.length < 2 || macs.length > 16 || !macs.every(isMac)) return null;
    const known = await Promise.all(macs.map(ask));
    if(!known.every(b=>b && earlierDays(b) >= AP_DAYS)) return null;
    return { low:true, why:list[0].ssid + ": the server knows all " + macs.length + " access points from earlier days, a fixed setup" };
  })(), TOTAL_MS);
}

function follower(mac){
  if(!ready() || (state.notifyScreenOn && !state.notifyScreenOn())) return Promise.resolve(null);
  return within(ask(String(mac).toUpperCase()).then(b=>
    b && b.history && b.history.level === "confirmed" ? { low:true, why:"the server has it fixed in one place" } : null), TOTAL_MS);
}

export function initScreen(){ state.screen = { wants, device, evil, follower }; }
