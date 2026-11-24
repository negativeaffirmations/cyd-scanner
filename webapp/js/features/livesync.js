// Live mirror of the scanner's current log on this phone (docs/phone-analysis.md, phase 1). No firmware change:
// while a scanner is connected, this keeps the phone-side log cache (data/logcache.js, the one Download / Upload /
// Map / Explore read) up to date with the scanner's current session file and, on a fleet LEADER, its live fleet
// file (the members' lines). Each pass fetches only the NEW bytes ("F:<path>@<offset>:<file_id>:<crc>", fw >= 0.26.0).
//
// Its own requests, never the general fetch path (that one falls back to a whole-file download on any problem):
//  - which file is which (path, file_id, size) comes from a QUIET session list (Q; download.js leaves the Data and
//    Explore UI alone). Q is blocking on the scanner (it reads every log's head and tail), so it is asked only on a
//    real change: the first pass, a current file name not in the last list, a refused update, the fleet's member
//    count changing; a file that is still missing is re-checked with backoff (20 s doubling to 5 min);
//  - a whole-file copy happens once per file, automatically only up to AUTO_FULL_MAX (size re-checked right before),
//    beyond that only when the user taps "Copy now"; never again automatically after the copy was lost (Clear saved
//    logs, eviction) in this connection;
//  - any refusal or odd reply ends the pass (nothing else is tried) and the next one waits longer;
//  - it yields: before every request it stops if someone is waiting (ble/syncwait.js) or a user transfer started;
//  - a copy that couldn't be saved (phone storage full) stops the mirror for this connection.
// NEW file, wired from main.js (stale-module rule).
import { state } from '../state.js';
import { enc } from '../config.js';
import { $ } from '../util/dom.js';
import { canTail } from '../ble/logfetch.js';
import { resetDl } from '../ble/transfer.js';
import { reqBusy, connGen } from '../ble/xfer.js';
import { gattWrite } from '../ble/gattq.js';
import { ready, cacheUsable, find, getBlob, put, fileIdOf } from '../data/logcache.js';
import { onUnitReset } from '../unitreset.js';

const KEY = "cflo.livesync";
const AUTO_FULL_MAX = 1024 * 1024;       // first copy fetched without asking (~80 s over BLE; the scanner can't abort one)
const MAX_MIRROR = 20 * 1024 * 1024;     // bigger files aren't kept live (each pass re-saves the copy until phase 2)
const RETRY_MIN = 20000, RETRY_MAX = 5 * 60000;
const FLEET_FRESH_S = 15 * 60;
const FILE_IDLE_MS = 20000, LIST_IDLE_MS = 45000;   // reply gaps: a big card can take a while to list
// Pass interval by file size: each pass re-saves the whole copy.
const syncMs = size=>size < 2097152 ? 20000 : size < 10485760 ? 60000 : 5 * 60000;

let enabled = true;
try{ enabled = localStorage.getItem(KEY) !== "0"; }catch(e){}

let running = false, nextAt = 0;
let cur = null;   // per connection, see fresh()
function fresh(gen, sid){
  return { gen, sid, files:{}, known:{}, fleet:null, listAt:0, listSess:"", listFlt:"", relist:false, missN:0, missAt:0,
           err:"", errN:0, stop:"", allowFull:new Set(), hadCopy:new Set(), tried:new Set() };
}
// files[label] = { path, fid, size, at, need, big, got, expect }   (label "own" / "fleet")
// known[path]  = { path, fid, size, last }                            from the last session list

export const liveSyncOn = ()=>enabled;

function canRun(){
  return enabled && cacheUsable() && state.connected && !!state.scannerId && canTail() && !state.wifiOn && !state.ap &&
         !!(state.status && /^[A-Za-z0-9_-]{1,40}$/.test(state.status.sess || ""));
}

// Thrown to end a pass quietly: someone is waiting for LOGDATA, or the scanner changed.
class Yield extends Error {}
function gate(gen, sid){
  if(!cur || cur.gen !== gen || connGen() !== gen || state.scannerId !== sid) throw new Yield("the scanner changed");
  if((state.liveSyncWaiters || 0) > 0 || (state.xferBusy || 0) > 0 || reqBusy()) throw new Yield("yielded");
}

// One LOGDATA request of our own. raw = file/tail replies as bytes (download.js state.dl.raw); quiet = a session
// list reply (ours or a stray one) leaves the Data / Explore UI alone.
function req(cmd, raw, idleMs, progress){
  return new Promise((resolve, reject)=>{
    if(!state.connected || !state.chars.cmd){ reject(new Error("not connected")); return; }
    let timer = null;
    const finish = (fn, v)=>{ clearInterval(timer); fn(v); };
    state.dl = { mode:null, expect:0, got:0, chunks:[], name:"live copy", sink:"promise", raw, quiet:true, waiting:true,
                 lastRx:Date.now(), progress, resolve:v=>finish(resolve, v), reject:e=>finish(reject, e) };
    const mine = state.dl;
    timer = setInterval(()=>{
      if(state.dl !== mine){ if(!mine.completing) finish(reject, new Error("superseded")); return; }
      if(Date.now() - mine.lastRx > idleMs){ resetDl(); finish(reject, new Error("no reply from the scanner")); }
    }, 1000);
    gattWrite(state.chars.cmd, enc.encode(cmd)).catch(e=>{ if(state.dl === mine) resetDl(); finish(reject, e); });
  });
}
// After a reply that wasn't ours: let the real one drain before anyone asks again.
async function drain(){
  for(let i = 0; i < 50 && reqBusy(); i++) await new Promise(r=>setTimeout(r, 200));
  await new Promise(r=>setTimeout(r, 2000));
}

// Quiet session list -> known files + the leader's live fleet file: the latest dated end; with the scanner's clock
// not set, the highest undated name.
async function list(gen, sid){
  gate(gen, sid);
  const text = await req("Q", false, LIST_IDLE_MS);
  if(typeof text !== "string" || (text !== "" && !text.startsWith("/logs/"))){ await drain(); throw new Error("unexpected reply to the session list"); }
  gate(gen, sid);
  const known = {};
  let best = null;
  // The scanner's epochs are LOCAL seconds; a dated fleet file counts only if it was written in the last FLEET_FRESH_S
  // (older ones are earlier sessions left on the card). Else the highest-named undated one (this boot, before a
  // member row with a time arrived).
  const nowLocal = Math.floor(Date.now() / 1000) - new Date().getTimezoneOffset() * 60;
  let undated = null;
  for(const ln of text.split("\n")){
    const f = ln.split("\t");
    if(f.length < 5 || !/^\/logs\/[^\/]+\.jsonl$/.test(f[0])) continue;
    const e = { path:f[0], size:parseInt(f[1], 10) || 0, last:parseInt(f[4], 10) || 0, fid:/^[0-9a-f]{8}$/.test(f[5] || "") ? f[5] : "" };
    known[e.path] = e;
    if(!/^\/logs\/fleet-/.test(e.path)) continue;
    if(e.last === 0){ if(!undated || e.path > undated.path) undated = e; }
    else if(e.last >= nowLocal - FLEET_FRESH_S && (!best || e.last > best.last)) best = e;
  }
  best = best || undated;
  cur.known = known;
  cur.fleet = best;
  cur.listAt = Date.now();
  cur.listSess = state.status.sess;
  cur.listFlt = state.status.flt || "";
  cur.relist = false;
}

// CRC-32 (IEEE) of the up-to-64 bytes before the join point (the device checks it; same as ble/logfetch.js).
let crcTable = null;
function crc32hex(bytes){
  if(!crcTable){ crcTable = new Uint32Array(256); for(let n = 0; n < 256; n++){ let c = n; for(let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1); crcTable[n] = c >>> 0; } }
  let c = 0xFFFFFFFF;
  for(let i = 0; i < bytes.length; i++) c = crcTable[(c ^ bytes[i]) & 255] ^ (c >>> 8);
  return ((c ^ 0xFFFFFFFF) >>> 0).toString(16).padStart(8, "0");
}

async function save(sid, path, blob, fid){
  const rec = await put(sid, path, blob);
  if(!rec || rec.size !== blob.size){ cur.stop = "the phone couldn't save the copy (storage full?)"; throw new Error(cur.stop); }
  cur.hadCopy.add(fid);
}

// Bring one file's copy up to date. Returns the number of new bytes.
async function syncFile(label, t, gen, sid){
  const meta = find(sid, t.path, t.fid);
  const base = meta && meta.fileId === t.fid ? await getBlob(meta) : null;
  if(base && base.size === meta.size){
    if(meta.size > MAX_MIRROR){ cur.files[label] = { ...t, size:meta.size, at:Date.now(), big:true }; return 0; }
    const crc = crc32hex(new Uint8Array(await base.slice(Math.max(0, meta.size - 64), meta.size).arrayBuffer()));
    gate(gen, sid);
    const r = await req("F:" + t.path + "@" + meta.size + ":" + t.fid + ":" + crc, true, FILE_IDLE_MS);
    gate(gen, sid);
    if(r && r.kind === "tailerr"){ cur.relist = true; throw new Error("the scanner refused the update (" + r.why + ")"); }
    if(!r || r.kind !== "tail" || r.off !== meta.size || r.blob.size !== r.n){ await drain(); throw new Error("unexpected reply"); }
    if(r.n){
      const merged = new Blob([base, r.blob]);
      if(await fileIdOf(merged) !== t.fid) throw new Error("the copy no longer matches the file");
      gate(gen, sid);
      await save(sid, t.path, merged, t.fid);
    }
    cur.files[label] = { ...t, size:meta.size + r.n, at:Date.now() };
    return r.n;
  }
  // No usable copy: one whole-file fetch. Size checked fresh first (the list may be old); automatic only while small
  // and only if this connection never had a copy of it (Clear saved logs / eviction must not trigger a re-copy).
  const refuse = x=>x.size > MAX_MIRROR ||
    (!cur.allowFull.has(x.fid) && (x.size > AUTO_FULL_MAX || cur.hadCopy.has(x.fid) || cur.tried.has(x.fid)));
  const offer = x=>{ cur.files[label] = { ...x, at:0, need:x.size, big:x.size > MAX_MIRROR }; return 0; };
  if(refuse(t)) return offer(t);                      // decided on the listed size: no session list for a refusal
  if(Date.now() - cur.listAt > 60000){                // about to copy: check the size is still what the list said
    await list(gen, sid);
    const k = cur.known[t.path];
    if(!k || k.fid !== t.fid){ cur.relist = true; throw new Error("the file changed on the scanner"); }
    t = k;
    if(refuse(t)) return offer(t);
  }
  gate(gen, sid);
  cur.files[label] = { ...t, at:0, got:0, expect:t.size };
  state.liveSyncFull = true;
  let r;
  cur.allowFull.delete(t.fid);
  cur.tried.add(t.fid);   // a copy that fails isn't retried automatically: it is offered again (Copy now)
  try{
    r = await req("F:" + t.path, true, FILE_IDLE_MS, (got, expect)=>{
      if(cur && cur.files[label]){ cur.files[label].got = got; cur.files[label].expect = expect; render(); }
    });
  }finally{ state.liveSyncFull = false; }
  gate(gen, sid);
  if(r === ""){ cur.tried.delete(t.fid); cur.files[label] = { ...t, size:0, at:Date.now() }; return 0; }   // empty file
  if(!r || r.kind !== "file"){ await drain(); throw new Error("unexpected reply"); }
  if(r.blob.size !== r.n) throw new Error("short transfer");
  if(await fileIdOf(r.blob) !== t.fid){ cur.relist = true; throw new Error("the file changed on the scanner"); }
  gate(gen, sid);
  await save(sid, t.path, r.blob, t.fid);
  cur.tried.delete(t.fid);
  cur.files[label] = { ...t, size:r.blob.size, at:Date.now() };
  return r.blob.size;
}

async function pass(){
  if(running || !canRun() || reqBusy() || (state.xferBusy || 0) > 0 || (state.liveSyncWaiters || 0) > 0) return;
  running = true;
  const gen = connGen(), sid = state.scannerId;
  let done = ()=>{}, interval = 20000;
  try{
    await ready;   // before liveSyncP: nobody waits on an IndexedDB open
    if(!canRun() || connGen() !== gen || state.scannerId !== sid) return;
    if(!cur || cur.gen !== gen || cur.sid !== sid) cur = fresh(gen, sid);
    if(cur.stop) return;
    state.liveSyncP = new Promise(r=>{ done = r; });
    render();
    const sess = state.status.sess, path = "/logs/" + sess + ".jsonl", leader = state.fleetRole === "leader";
    const own0 = cur.known[path];
    const fm = /^[^,]*,(\d+)\//.exec(state.status.flt || ""), joined = fm ? parseInt(fm[1], 10) : 0;
    const fleetMissing = leader && joined > 0 && !cur.fleet;
    const missing = !own0 || !own0.fid || fleetMissing;
    const want = !cur.listAt || cur.relist ||
                 (cur.listSess !== sess && !own0) ||                                  // a new name: New Session / sync rename / fg-bg
                 (leader && cur.listFlt !== (state.status.flt || "") && Date.now() - cur.listAt > 30000) ||   // members came / went
                 (missing && Date.now() >= cur.missAt);                               // still missing: backoff
    if(want) await list(gen, sid);
    const own = cur.known[path];
    if(!own || !own.fid || (leader && joined > 0 && !cur.fleet)){ cur.missN++; cur.missAt = Date.now() + Math.min(RETRY_MAX, RETRY_MIN * Math.pow(2, cur.missN)); }
    else cur.missN = 0;
    if(own && own.fid){ await syncFile("own", own, gen, sid); interval = syncMs(cur.files.own.size || 0); }
    else cur.files.own = { path, at:0, waiting:true };
    if(leader && cur.fleet && cur.fleet.fid){
      await syncFile("fleet", cur.fleet, gen, sid);
      interval = Math.max(interval, syncMs(cur.files.fleet.size || 0));
    } else delete cur.files.fleet;
    cur.err = ""; cur.errN = 0;
  }catch(e){
    if(e instanceof Yield) interval = 3000;   // try again soon, after the user's request
    else if(cur && cur.gen === gen){
      cur.err = e.message || String(e);
      cur.errN++;
      interval = Math.min(RETRY_MAX, RETRY_MIN * Math.pow(2, cur.errN));
    }
  }finally{
    running = false;
    if(cur && cur.gen === gen) nextAt = Date.now() + interval;   // a unit reset mid-pass keeps its own nextAt
    if(state.liveSyncP){ state.liveSyncP = null; done(); }
    render();
  }
}

function tick(){
  if(canRun() && !running && Date.now() >= nextAt) pass();
  render();
}

const kb = b=>b >= 1048576 ? (b / 1048576).toFixed(1) + " MB" : Math.max(b ? 1 : 0, Math.round(b / 1024)) + " KB";
const ago = t=>{ const s = Math.max(0, Math.round((Date.now() - t) / 1000)); return s < 60 ? s + " s ago" : Math.round(s / 60) + " min ago"; };
const mins = b=>Math.max(1, Math.round(b / 13000 / 60));   // ~13 KB/s over BLE

function describe(f, what){
  if(!f) return "";
  if(f.waiting) return what + ": waiting for the scanner to write it";
  if(f.big) return what + " is " + kb(f.need || f.size) + ", too big to keep live (download it when needed)";
  if(f.need) return what + " is " + kb(f.need) + " (about " + mins(f.need) + " min to copy)";
  if(!f.at) return what + ": copying, " + kb(f.got || 0) + " of " + kb(f.expect || f.size);
  return what + " " + kb(f.size) + ", updated " + ago(f.at);
}

function render(){
  const info = $("liveSyncInfo"), box = $("liveSync"), btn = $("liveSyncFullBtn");
  if(!info || !box) return;
  box.checked = enabled;
  let t, need = 0;
  const mine = cur && cur.gen === connGen() ? cur : null;
  if(!enabled) t = "Off. Downloads fetch the scan from the scanner when you ask.";
  else if(!cacheUsable()) t = "Not available: this browser can't save logs.";
  else if(!state.connected) t = "Starts when a scanner is connected.";
  else if(!canTail()) t = "Needs scanner firmware 0.26.0 or newer.";
  else if(state.wifiOn || state.ap) t = "Paused while the Wi-Fi download is on.";
  else if(mine && mine.stop) t = "Stopped: " + mine.stop + ".";
  else if(!mine || (!mine.files.own && !mine.files.fleet)) t = running ? "Checking the scanner's log files…" : "Waiting for the scanner…";
  else {
    t = [describe(mine.files.own, "Current scan"), describe(mine.files.fleet, "Fleet file")].filter(Boolean).join(". ") + ".";
    if(mine.err) t += " Last update failed (" + mine.err + "); trying again shortly.";
    for(const f of [mine.files.own, mine.files.fleet]) if(f && f.need && !f.big) need += f.need;
  }
  info.textContent = t;
  if(btn){
    btn.hidden = !need;
    btn.disabled = running;
    btn.textContent = "Copy now (" + kb(need) + ", about " + mins(need) + " min; other transfers wait)";
  }
}

export function initLiveSync(){
  const box = $("liveSync"), btn = $("liveSyncFullBtn");
  if(box) box.addEventListener("change", ()=>{
    enabled = box.checked;
    try{ localStorage.setItem(KEY, enabled ? "1" : "0"); }catch(e){}
    nextAt = 0;
    tick();
  });
  if(btn) btn.addEventListener("click", ()=>{
    if(!cur || cur.gen !== connGen()) return;
    for(const f of [cur.files.own, cur.files.fleet]) if(f && f.need && !f.big && f.fid) cur.allowFull.add(f.fid);
    nextAt = 0;
    tick();
  });
  onUnitReset(()=>{ cur = null; nextAt = 0; render(); });
  setInterval(tick, 2000);
  window.addEventListener("cflo-bg-tick", tick);   // Android app: hidden-page timers are throttled (shim-background.js)
  render();
}
