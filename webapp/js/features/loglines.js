// Live upload of the FULL log ("loglines" v1, agreed with scan-analyzer 2026-11-24). Replaces the top-12 DETS rows
// (features/remote.js) while the server advertises it (GET /api/v1/whoami "loglines":1) and the phone's live copy of
// the scan (features/livesync.js) is complete: the copy's own bytes go out, so the server gets every line the scanners
// logged, a leader's fleet file included.
//   POST /api/v1/loglines  Bearer; raw bytes of ONE file from X-Log-Offset, <= 256 KB, ending on a newline
//   reply 200 / 409 {"ack":N,"sha256":<hash of the 4 KB before N>}   (409 = our offset was wrong: continue from N)
// An empty body is a query. The server keeps the progress per key + scanner + file, so a lost reply, an outage, a page
// reload or a reconnect all just continue. Log files are append-only under a fixed file_id, which makes that safe; the
// hash proves both sides mean the same bytes.
// Never both modes: active() is true only once the own file (and, on a leader, the fleet file) is queried and fine;
// until then, or after a refusal, the DETS rows go out as before. Server trouble (5xx / 429 / no network) keeps the
// mode: the bytes wait in the saved copy. After a disconnect the saved rest is still sent.
// NEW file, reached through state hooks (stale-module rule).
import { state } from '../state.js';
import { log } from '../util/dom.js';
import { connGen } from '../ble/xfer.js';
import { find, getBlob } from '../data/logcache.js';
import { isPaired, serverUrl } from './remote.js';

const MAX_POST = 256 * 1024, HASH_N = 4096, TICK_MS = 5000, TIMEOUT_MS = 30000, CAP_OK_MS = 5 * 60000;
const BACKOFF = [5000, 10000, 20000];
const ls = k=>{ try{ return localStorage.getItem(k) || ""; }catch(e){ return ""; } };
const base = ()=>serverUrl().replace(/\/+$/, "");
const wanted = ()=>isPaired() && ls("rs.live") === "1";

let cap = null, capScreen = false, capAt = 0, capFor = "";   // the server's answer to "loglines?", per server + key
let files = new Map();                    // "sid|fid" -> { sid, path, fid, label, gen, ack (-1 = not queried), stop, maxPost }
let busy = false, fails = 0, holdUntil = 0, lastRun = 0, said = "";
let haltKey = "";                         // the server + key that was refused (401 / 403)
const who = ()=>serverUrl() + "|" + ls("rs.key");

async function call(path, opt){
  const ctl = new AbortController(), timer = setTimeout(()=>ctl.abort(), TIMEOUT_MS);
  try{
    const r = await fetch(base() + path, { ...opt, signal:ctl.signal, cache:"no-store",
      headers:{ ...(opt.headers || {}), "Authorization":"Bearer " + ls("rs.key") } });
    let body = null;
    try{ body = await r.json(); }catch(e){}
    return { status:r.status, body };
  }catch(e){ return { status:0, body:null }; }
  finally{ clearTimeout(timer); }
}

async function supported(){
  if(cap !== null && capFor === who() && Date.now() - capAt < CAP_OK_MS) return cap;
  const w = who(), r = await call("/api/v1/whoami", {});
  if(r.status === 0 || r.status >= 500 || r.status === 429) return capFor === w && cap === true;   // unreachable: keep what we knew
  capFor = w; capAt = Date.now();
  cap = r.status === 200 && !!(r.body && +r.body.loglines === 1);
  capScreen = r.status === 200 && !!(r.body && +r.body.screen === 1);
  return cap;
}

async function hashBefore(blob, n){
  const buf = await blob.slice(Math.max(0, n - HASH_N), n).arrayBuffer();
  return [...new Uint8Array(await crypto.subtle.digest("SHA-256", buf))].map(x=>x.toString(16).padStart(2, "0")).join("");
}

function headers(f, off, hash){
  const h = { "Content-Type":"application/x-ndjson", "X-Scanner-Id":f.sid, "X-File-Id":f.fid, "X-Log-Offset":String(off),
              "X-Session-Name":f.path.replace(/^.*\//, "") };
  if(hash) h["X-Log-Prefix-Sha256"] = hash;
  try{ h["X-Client-Tz"] = Intl.DateTimeFormat().resolvedOptions().timeZone || ""; }catch(e){}
  return h;
}

// Is the server's {ack, sha256} about the bytes we hold? A server ahead of our copy can't be checked yet.
async function accept(f, blob, body){
  const n = body && body.ack;
  if(!Number.isInteger(n) || n < 0) return "odd";
  if(n > blob.size) return "ahead";
  if(body.sha256 && await hashBefore(blob, n) !== String(body.sha256).toLowerCase()) return "differs";
  f.ack = n;
  return "ok";
}

function stop(f, why){ f.stop = why; log("Live upload: " + f.path.replace(/^.*\//, "") + " - " + why + ". Using the short list instead."); }
function trouble(status){
  holdUntil = Date.now() + BACKOFF[Math.min(fails, BACKOFF.length - 1)];
  fails++;
  if(said !== "bad"){ said = "bad"; log("Live upload: the server can't take data now (" + (status || "no answer") + "). It continues from the saved copy."); }
}
function fine(){ fails = 0; holdUntil = 0; if(said === "bad"){ said = ""; log("Live upload: continuing."); } }

// Deal with a reply. true = go on with this file.
async function handle(f, blob, r, posted){
  if(r.status === 200 || r.status === 409){
    const before = f.ack, a = await accept(f, blob, r.body);
    if(a === "ok"){
      fine();
      if(posted && f.ack <= before){ if(++f.stuck >= 3) stop(f, "the server made no progress"); else trouble(r.status); return false; }   // a backward 409 gets another try, not an endless loop
      f.stuck = 0;
      return true;
    }
    if(a === "ahead") return false;   // our copy is behind what the server has: wait for it to grow
    stop(f, a === "differs" ? "the server holds different data for this file" : "unexpected reply");
    return false;
  }
  if(r.status === 401 || r.status === 403){ haltKey = who(); log("Live upload: the server refused this phone's key (" + r.status + ")."); return false; }
  if(r.status === 413 && f.maxPost > 16384){ f.maxPost = f.maxPost / 2; return true; }
  if(r.status === 404 || r.status === 405){ cap = false; capFor = who(); capAt = Date.now(); return false; }   // the server no longer has it
  if(r.status === 0 || r.status === 429 || r.status >= 500){ trouble(r.status); return false; }
  stop(f, "the server refused it (" + r.status + ")");
  return false;
}

async function sendFile(f){
  for(let k = 0; k < 8; k++){          // a few posts per run, then the next file gets its turn
    const meta = find(f.sid, f.path, f.fid), blob = meta && meta.fileId === f.fid ? await getBlob(meta) : null;
    if(!blob) return;
    if(f.ack < 0){
      if(!await handle(f, blob, await call("/api/v1/loglines", { method:"POST", headers:headers(f, 0, ""), body:"" }), false)) return;
      continue;
    }
    if(f.ack >= blob.size) return;
    const piece = new Uint8Array(await blob.slice(f.ack, f.ack + f.maxPost).arrayBuffer());
    const end = piece.lastIndexOf(10) + 1;
    if(!end){ if(piece.length >= f.maxPost) stop(f, "a line is too long"); return; }   // only a partial line so far
    const hash = await hashBefore(blob, f.ack), p0 = performance.now();
    const r = await call("/api/v1/loglines", { method:"POST", headers:headers(f, f.ack, hash), body:piece.slice(0, end) });
    if(state.onLivePost) state.onLivePost(end, performance.now() - p0, r.status === 200);   // timing (livestats.js)
    if(!await handle(f, blob, r, r.status !== 413)) return;
  }
}

// The files of the running scan, from livesync: the own file and, on a leader in a session, the fleet file.
function collect(){
  if(!state.connected || !state.liveCopies) return;
  const gen = connGen();
  for(const c of state.liveCopies()){
    if(!c.fid || !c.sid || !c.ok) continue;
    const key = c.sid + "|" + c.fid;
    const f = files.get(key);
    if(f){ f.path = c.path; f.label = c.label; f.gen = gen; }
    else files.set(key, { sid:c.sid, path:c.path, fid:c.fid, label:c.label, gen, ack:-1, stop:"", stuck:0, maxPost:MAX_POST });
  }
}

// Does the full log cover this connection's live upload right now? (features/remote.js skips its rows while true)
function active(){
  if(!wanted() || cap !== true || capFor !== who() || haltKey === who() || !state.connected || !state.liveCopies) return false;
  const now = state.liveCopies();
  const own = now.find(c=>c.label === "own");
  if(!own) return false;
  return now.every(c=>{
    const f = c.ok && files.get(c.sid + "|" + c.fid);
    return !!f && f.ack >= 0 && !f.stop;
  });
}

async function run(){
  if(busy || Date.now() < holdUntil || Date.now() - lastRun < TICK_MS) return;
  if(!wanted()){ files.clear(); return; }
  if(haltKey && haltKey === who()) return;
  haltKey = "";
  busy = true; lastRun = Date.now();
  try{
    collect();
    if(!files.size || !await supported()) return;
    for(const f of [...files.values()]){
      if(f.stop) continue;
      await sendFile(f);
      if(Date.now() < holdUntil || (haltKey && haltKey === who()) || !cap) break;
    }
    for(const [k, f] of files){        // an older scan's file that is fully sent needs nothing more
      if(f.gen === connGen() && state.connected) continue;
      const m = find(f.sid, f.path, f.fid);
      if(f.stop || !m || (f.ack >= 0 && f.ack >= m.size)) files.delete(k);
    }
  }catch(e){ trouble(e.message); }
  finally{ busy = false; }
}

export function initLogLines(){
  state.logLinesOn = active;
  state.serverScreens = async ()=>{ await supported(); return capScreen && capFor === who(); };   // features/screen.js
  setInterval(run, 1000);
  window.addEventListener("online", ()=>{ holdUntil = 0; });   // the network is back: no need to wait out the backoff
  window.addEventListener("cflo-bg-tick", run);   // Android app: hidden-page timers are throttled
}
export const _test = { run, active, files:()=>files, reset(){ files = new Map(); cap = null; capFor = ""; haltKey = ""; holdUntil = 0; fails = 0; lastRun = 0; } };
