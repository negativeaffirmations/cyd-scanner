// Phone analysis, increment 3b: history from every scan log saved on this phone (docs/phone-analysis.md phase 3).
//
// Reads the phone-side log cache (data/logcache.js, IndexedDB "cflo.logcache") once per page load, in the background,
// one file at a time with pauses, and keeps per device: the days it was seen, the places (PLACE_M apart), first / last
// seen, names, and which alert events (evil twin, deauth) named it. All scanners' saved files count (fleet files carry
// the members' rows), so a device's history spans every unit. Read-only: nothing is written back.
// Public through state.history (NOT by importing this module under another URL than main.js's ?v= one, which would
// load a second, empty instance): historyFor(mac), historyStatus(), nearBefore(mac, lat, lon).
// NEW file (stale-module rule).
import { state } from '../state.js';
import { ready, getBlob } from '../data/logcache.js';

const PLACE_M = 300, MAX_PLACES = 12, MAX_EVENTS = 20, MAX_FILE = 30 * 1024 * 1024;
const START_DELAY_MS = 4000, PAUSE_MS = 30;   // let the page settle; yield between chunks

const devices = new Map();     // MAC -> { days:Set, places:[{lat,lon,days:Set,n}], first, last, n, names:Set, events:[] }
let status = { state:"waiting", files:0, done:0, lines:0, bytes:0 };
const listeners = new Set();

const R = 6371000, rad = x=>x * Math.PI / 180;
function distM(a, b){
  const dLat = rad(b.lat - a.lat), dLon = rad(b.lon - a.lon);
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
}
const okLL = (a, b)=>typeof a === "number" && typeof b === "number" && isFinite(a) && isFinite(b) && !(a === 0 && b === 0);
const day = t=>new Date(t * 1000).toISOString().slice(0, 10);   // local epoch -> the scanner's calendar day
const MACRE = /^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/;

function rec(mac){
  let d = devices.get(mac);
  if(!d){ d = { days:new Set(), places:[], first:0, last:0, n:0, names:new Set(), events:[] }; devices.set(mac, d); }
  return d;
}

function addRow(o, t){
  const mac = String(o.mac).toUpperCase();
  if(!MACRE.test(mac)) return;
  const d = rec(mac);
  d.n++;
  if(o.name && d.names.size < 5) d.names.add(o.name);
  if(!t) return;
  const dy = day(t);
  d.days.add(dy);
  if(!d.first || t < d.first) d.first = t;
  if(t > d.last) d.last = t;
  if(okLL(o.lat, o.lon) && typeof o.fix_age !== "number"){
    const p = d.places.find(p=>distM(p, o) < PLACE_M);
    if(p){ p.n++; p.days.add(dy); }
    else if(d.places.length < MAX_PLACES) d.places.push({ lat:o.lat, lon:o.lon, days:new Set([dy]), n:1 });
  }
}

// An alert line names devices: an evil twin its BSSIDs ("macs"), a deauth its sender / target / BSSID.
function addEvent(o, t){
  const macs = new Set();
  if(o.evt === "eviltwin" && o.macs) String(o.macs).split(",").forEach(m=>macs.add(m.trim().toUpperCase()));
  if(o.evt === "deauth") ["from", "to", "bssid"].forEach(k=>o[k] && macs.add(String(o[k]).toUpperCase()));
  const ev = { t, evt:o.evt, ssid:o.ssid || "", lat:o.lat, lon:o.lon, scanner:o.scanner || "" };
  for(const m of macs){
    if(!MACRE.test(m)) continue;
    const d = rec(m);
    if(d.events.length < MAX_EVENTS && !d.events.some(e=>e.t === t && e.evt === ev.evt)) d.events.push(ev);
  }
}

async function readFile(meta){
  const blob = await getBlob(meta);
  if(!blob || blob.size > MAX_FILE) return;
  const text = await blob.text();
  let anchor = null, i = 0, n = 0;
  // Pre-sync rows (epoch 0) are dated from the file's timesync anchor, which comes AFTER them: two passes.
  const lines = text.split("\n");
  for(const ln of lines){
    if(ln.indexOf('"timesync"') < 0) continue;
    try{ const o = JSON.parse(ln); if(o.evt === "timesync" && o.epoch > 0){ anchor = o; break; } }catch(e){}
  }
  for(const ln of lines){
    if(++i % 2000 === 0) await new Promise(r=>setTimeout(r, PAUSE_MS));   // stay responsive on a big file
    if(ln.charCodeAt(0) !== 123) continue;
    let o;
    try{ o = JSON.parse(ln); }catch(e){ continue; }
    n++;
    const t = o.epoch > 0 ? o.epoch : anchor && typeof o.ms === "number" ? anchor.epoch + (o.ms - anchor.ms) / 1000 : 0;
    if(o.evt){ if(o.evt === "eviltwin" || o.evt === "deauth") addEvent(o, t); continue; }
    if(o.mac) addRow(o, t);
  }
  status.lines += n;
  status.bytes += blob.size;
}

async function build(){
  await ready;
  let metas = [];
  try{
    metas = await new Promise((res, rej)=>{
      const r = indexedDB.open("cflo.logcache");
      r.onsuccess = ()=>{
        try{ const q = r.result.transaction("meta").objectStore("meta").getAll(); q.onsuccess = ()=>res(q.result || []); q.onerror = ()=>rej(q.error); }
        catch(e){ rej(e); }
      };
      r.onerror = ()=>rej(r.error);
    });
  }catch(e){ status.state = "unavailable"; changed(); return; }
  metas.sort((a, b)=>(a.savedAt || 0) - (b.savedAt || 0));
  status = { state:"reading", files:metas.length, done:0, lines:0, bytes:0 };
  changed();
  for(const m of metas){
    try{ await readFile(m); }catch(e){}
    status.done++;
    if(status.done % 5 === 0) changed();
    await new Promise(r=>setTimeout(r, PAUSE_MS));
  }
  status.state = "ready";
  changed();
}

function changed(){ listeners.forEach(fn=>{ try{ fn(); }catch(e){} }); }

export function historyFor(macIn){
  const d = devices.get(String(macIn || "").toUpperCase());
  if(!d) return null;
  return {
    days:[...d.days].sort(), first:d.first, last:d.last, n:d.n, names:[...d.names],
    places:d.places.map(p=>({ lat:p.lat, lon:p.lon, days:p.days.size, n:p.n })),
    events:d.events.slice().sort((a, b)=>a.t - b.t),
  };
}
// Earlier days this device was seen within PLACE_M of (lat, lon).
export function nearBefore(mac, lat, lon){
  const d = devices.get(String(mac || "").toUpperCase());
  if(!d || !okLL(lat, lon)) return 0;
  const p = d.places.find(p=>distM(p, { lat, lon }) < PLACE_M);
  return p ? p.days.size : 0;
}
export const historyStatus = ()=>({ ...status, devices:devices.size });

export function initHistory(){
  state.history = { historyFor, historyStatus, nearBefore, onHistoryChange:fn=>listeners.add(fn) };
  setTimeout(()=>{ build().catch(()=>{ status.state = "unavailable"; changed(); }); }, START_DELAY_MS);
}
