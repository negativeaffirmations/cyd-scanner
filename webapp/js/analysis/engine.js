// Phone-side analysis engine, increment 3a (docs/phone-analysis.md phase 3).
//
// Builds one record per device from the scanner's log lines, for the WHOLE current scan and every fleet unit (a
// leader's fleet file carries the members' rows, tagged "scanner"). The lines come from the live copy
// (features/livesync.js): every new piece of a mirrored file, pushed or fetched, is a window "cflo-livelog" event
// {sid, path, fid, off, bytes}. Before a file's first piece arrives here, the start of the file is read from the
// phone's saved copy, so a scan that began before this page loaded is complete too.
//
// Analysis (all receive-only, on data the scanners already logged):
//  - follow over the whole scan: a device's sightings with a position are grouped into places (PLACE_M apart);
//    seen at >= FOLLOW_PLACES places, spread over >= FOLLOW_SPREAD_M, across >= FOLLOW_SPAN_S -> "following".
//    A scanner only keeps the last few minutes; the phone keeps the drive.
//  - your own movement (every row's position is the scanner's) to tell "it stayed put while you drove around" apart.
// Public: analysisFor(mac) -> summary or null; engineStats(); onEngineChange(fn). NEW file (stale-module rule).
import { state } from '../state.js';
import { connGen } from '../ble/xfer.js';
import { find, getBlob } from '../data/logcache.js';
import { onUnitReset } from '../unitreset.js';

const PLACE_M = 300, FOLLOW_PLACES = 3, FOLLOW_SPREAD_M = 1000, FOLLOW_SPAN_S = 600;
const MAX_SIGHT = 2000;          // per device (oldest half thinned when full)
const MAX_TRACK = 5000;          // own-track points (one per TRACK_STEP_S)
const TRACK_STEP_S = 20;
const MOVE_STEP_M = 60;         // movement counts only past this, so GPS jitter while parked adds nothing

let gen = -1, sid = "";
let devices = new Map();         // MAC -> record
let files = new Map();           // fid -> { path, parsed, dec, anchor:{epoch,ms}|null, pending:[], chain:Promise }
let track = [];                  // [{t, lat, lon}] the scanner's own position over time (sorted by t)
let events = [];                 // {t, evt, ...} deauth / eviltwin / rid / follow lines
let lines = 0;
const listeners = new Set();
let changeTimer = null;

export function onEngineChange(fn){ listeners.add(fn); }
function changed(){
  if(changeTimer) return;
  changeTimer = setTimeout(()=>{ changeTimer = null; listeners.forEach(fn=>{ try{ fn(); }catch(e){} }); }, 500);
}

function reset(){
  devices = new Map(); files = new Map(); track = []; events = []; lines = 0;
  gen = connGen(); sid = state.scannerId || "";
  changed();
}

// ---------- ingest ----------
const okLL = (a, b)=>typeof a === "number" && typeof b === "number" && isFinite(a) && isFinite(b) && !(a === 0 && b === 0);

// A row's time in local epoch seconds: its own epoch, else (pre-sync rows) from the file's timesync anchor.
function rowTime(o, f){
  if(o.epoch > 0) return o.epoch;
  if(f.anchor && typeof o.ms === "number") return f.anchor.epoch + (o.ms - f.anchor.ms) / 1000;
  return 0;
}

function addTrack(t, lat, lon){
  if(!t || !okLL(lat, lon)) return;
  const last = track[track.length - 1];
  if(last && t - last.t < TRACK_STEP_S && t >= last.t) return;
  if(last && t < last.t){   // out of order (two files): insert in place
    let i = track.length - 1;
    while(i > 0 && track[i - 1].t > t) i--;
    track.splice(i, 0, { t, lat, lon });
  } else track.push({ t, lat, lon });
  if(track.length > MAX_TRACK) track = track.filter((_, i)=>i % 2 === 0);
}

function line(text, f){
  if(!text || text.charCodeAt(0) !== 123) return;   // '{'
  let o;
  try{ o = JSON.parse(text); }catch(e){ return; }
  lines++;
  if(o.evt === "timesync"){ if(o.epoch > 0 && typeof o.ms === "number") f.anchor = { epoch:o.epoch, ms:o.ms }; return; }
  if(o.evt === "meta") return;
  const t = rowTime(o, f);
  addTrack(t, o.lat, o.lon);
  if(o.evt){
    if(o.evt === "lastseen" && o.mac){
      const d = devices.get(String(o.mac).toUpperCase());
      if(d && typeof o.last_ms === "number" && typeof o.ms === "number" && t){
        const last = t - (o.ms - o.last_ms) / 1000;
        if(last > d.last) d.last = last;
      }
    } else if(o.evt !== "fleet" && o.evt !== "phonecfg" && o.evt !== "devpos") events.push({ t, ...o });
    return;
  }
  if(!o.mac) return;
  const mac = String(o.mac).toUpperCase();
  let d = devices.get(mac);
  if(!d){
    d = { mac, first:t || 0, last:t || 0, n:0, src:new Set(), names:new Set(), scanners:new Set(), sight:[],
          tier:"", score:0, sig:"", flags:new Set(), rssiMax:-999, wl:false };
    devices.set(mac, d);
  }
  d.n++;
  if(t){ if(!d.first || t < d.first) d.first = t; if(t > d.last) d.last = t; }
  if(o.src) d.src.add(o.src);
  if(o.name) d.names.add(o.name);
  d.scanners.add(o.scanner || sid);
  if(o.tier && tierRank(o.tier) >= tierRank(d.tier)){ d.tier = o.tier; d.sig = o.sig || d.sig; }
  if(typeof o.score === "number" && o.score > d.score) d.score = o.score;
  if(o.flags) String(o.flags).split("+").forEach(x=>x && d.flags.add(x));
  d.wl = o.wl === 1;   // the scanner's whitelist verdict on its latest row
  if(typeof o.rssi === "number" && o.rssi > d.rssiMax) d.rssiMax = o.rssi;
  if(t && okLL(o.lat, o.lon)){
    d.sight.push({ t, lat:o.lat, lon:o.lon, rssi:o.rssi, sc:o.scanner || sid, aged:typeof o.fix_age === "number" });
    if(d.sight.length > MAX_SIGHT){ const keep = d.sight.slice(MAX_SIGHT / 2); d.sight = d.sight.slice(0, MAX_SIGHT / 2).filter((_, i)=>i % 2 === 0).concat(keep); }
  }
}
const tierRank = t=>t === "confirmed" ? 3 : t === "likely" ? 2 : t === "suspect" ? 1 : 0;

function feed(f, bytes){
  const text = f.rem + f.dec.decode(bytes, { stream:true });
  const parts = text.split("\n");
  f.rem = parts.pop();
  for(const p of parts) line(p, f);
}

// One file's pieces, strictly in order: overlaps are trimmed, a gap is filled from the saved copy.
async function take(d){
  let f = files.get(d.fid);
  if(!f){
    f = { path:d.path, parsed:0, dec:new TextDecoder(), rem:"", anchor:null };
    files.set(d.fid, f);
  }
  f.path = d.path;
  if(d.off > f.parsed){   // the start (or a gap): read it from the saved copy
    const meta = find(sid, d.path, d.fid);
    const blob = meta && meta.fileId === d.fid ? await getBlob(meta) : null;
    if(!blob || blob.size < d.off) return false;   // can't fill it: skip this file until a later piece lines up
    feed(f, new Uint8Array(await blob.slice(f.parsed, d.off).arrayBuffer()));
    f.parsed = d.off;
  }
  const skip = f.parsed - d.off;
  if(skip >= d.bytes.length) return true;
  feed(f, skip ? d.bytes.subarray(skip) : d.bytes);
  f.parsed = d.off + d.bytes.length;
  return true;
}

let chain = Promise.resolve();
function onPiece(e){
  const d = e.detail;
  if(!d || !d.fid || !(d.bytes instanceof Uint8Array)) return;
  if(gen !== connGen() || sid !== (state.scannerId || "")) reset();
  const g = gen;
  chain = chain.then(async ()=>{
    if(g !== gen) return;
    try{ if(await take(d)) changed(); }catch(err){}
  });
}

// ---------- analysis ----------
const R = 6371000, rad = x=>x * Math.PI / 180;
export function distM(a, b){
  const dLat = rad(b.lat - a.lat), dLon = rad(b.lon - a.lon);
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
}
// Is this a BLE resolvable private address (rotates every ~15 min, so one device shows up under several MACs)?
const isRpa = mac=>(parseInt(mac.slice(0, 2), 16) & 0xC0) === 0x40;
const isLocal = mac=>(parseInt(mac.slice(0, 2), 16) & 0x02) !== 0;   // Wi-Fi: a randomized (locally set) address

// Greedy places in time order: a new place starts when a sighting is PLACE_M from the current place's first point.
function places(sight){
  const s = sight.filter(x=>!x.aged).sort((a, b)=>a.t - b.t);
  const out = [];
  for(const x of s){
    const p = out[out.length - 1];
    if(p && distM(p, x) < PLACE_M){ p.n++; p.t1 = x.t; continue; }
    out.push({ lat:x.lat, lon:x.lon, t0:x.t, t1:x.t, n:1 });
  }
  return out;
}

// How far the scanner itself moved between two times (sum of track steps).
function movedM(t0, t1){
  let m = 0, anchor = null;
  for(const p of track){
    if(p.t < t0){ anchor = p; continue; }
    if(p.t > t1) break;
    if(!anchor){ anchor = p; continue; }
    const step = distM(anchor, p);
    if(step >= MOVE_STEP_M){ m += step; anchor = p; }
  }
  return m;
}

export function analysisFor(macIn){
  const mac = String(macIn || "").toUpperCase();
  const d = devices.get(mac);
  if(!d) return null;
  const pl = places(d.sight);
  let spread = 0;
  for(let i = 0; i < pl.length; i++) for(let j = i + 1; j < pl.length; j++) spread = Math.max(spread, distM(pl[i], pl[j]));
  const span = d.last && d.first ? d.last - d.first : 0;
  const youMoved = span > 0 ? movedM(d.first, d.last) : 0;
  let verdict, level;   // level 2 following / 1 notable / 0 nothing
  if(!d.sight.length){ verdict = "No GPS positions for it yet, so no follow analysis."; level = 0; }
  else if(pl.length >= FOLLOW_PLACES && spread >= FOLLOW_SPREAD_M && span >= FOLLOW_SPAN_S){
    verdict = "Following: seen at " + pl.length + " places up to " + km(spread) + " apart over " + mins(span) + ".";
    level = 2;
  } else if(pl.length >= 2){
    verdict = "Seen at " + pl.length + " places (" + km(spread) + " apart) over " + mins(span) + ": not enough to call it following.";
    level = 1;
  } else if(youMoved >= FOLLOW_SPREAD_M){
    verdict = "Stayed in one place while you moved " + km(youMoved) + ": not following.";
    level = 0;
  } else {
    verdict = "Seen in one place only.";
    level = 0;
  }
  return {
    mac, n:d.n, first:d.first, last:d.last, span, places:pl.length, spreadM:spread, youMovedM:youMoved,
    names:[...d.names], src:[...d.src], scanners:[...d.scanners], ownSid:sid, tier:d.tier, sig:d.sig, score:d.score,
    flags:[...d.flags], rssiMax:d.rssiMax, verdict, level,
    wl:d.wl,
    lastPos:d.sight.length ? { lat:d.sight[d.sight.length - 1].lat, lon:d.sight[d.sight.length - 1].lon } : null,
    rotating:d.src.has("BLE") && isRpa(mac),
    randomWifi:!d.src.has("BLE") && !d.src.has("154") && isLocal(mac),
  };
}
const km = m=>m >= 1000 ? (m / 1000).toFixed(1) + " km" : Math.round(m) + " m";
const mins = s=>s >= 3600 ? (s / 3600).toFixed(1) + " h" : Math.max(1, Math.round(s / 60)) + " min";

// MACs the whole-scan analysis currently calls following (analysis/alerts.js).
export function followers(){
  const out = [];
  for(const mac of devices.keys()){ const a = analysisFor(mac); if(a && a.level === 2) out.push(a); }
  return out;
}
// Alert lines (deauth / eviltwin / rid / follow) of this scan from index i on: {next, list}.
export function eventsFrom(i){ return { next:events.length, list:events.slice(i) }; }
export const engineGen = ()=>gen;

export function engineStats(){
  let following = 0;
  for(const mac of devices.keys()){ const a = analysisFor(mac); if(a && a.level === 2) following++; }
  return { devices:devices.size, lines, files:files.size, trackPoints:track.length, events:events.length, following };
}

export function initEngine(){
  window.addEventListener("cflo-livelog", onPiece);
  onUnitReset(reset);
  // The one way other modules reach the engine (features/phoneview.js): importing engine.js under a different URL
  // than main.js's ?v= one would load a second, empty instance. Also handy over DevTools.
  state.engine = { analysisFor, engineStats, onEngineChange, followers, eventsFrom, engineGen };
}
