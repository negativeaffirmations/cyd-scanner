// Fetching a scanner log file through the phone-side cache (data/logcache.js).
//   fetchLog(path, {size, fileId, name})  -> {text, blob, how}   how: "hit" (no transfer) | "full"
//   fetchCurrent(name)                    -> same, for the device's CURRENT session; null = not applicable
//   cacheStatus(path, size, fileId)       -> {st:"none"|"part"|"full", fresh} for the Data page tags
// Every caller gets the WHOLE file (uploads must carry line 1's meta + the timesync anchors), whether it
// came from the cache, from the device, or from a cached prefix plus the device's new tail.
// Raw fetch: like ble/transfer.js bleFetch but resolves with the exact bytes (a Blob), because the cache
// must hold byte-identical copies (a text round trip would change the size of a torn UTF-8 tail).
// download.js hands a {kind,blob,...} object to a request marked state.dl.raw (an older cached download.js
// resolves plain text instead -> still works, just uncached).
// NEW file: other modules only import it, so a stale cached copy of an old module can't miss a name.
import { state } from '../state.js';
import { enc } from '../config.js';
import { log } from '../util/dom.js';
import { resetDl } from './transfer.js';
import { ready, cacheUsable, find, getBlob, put } from '../data/logcache.js';

const IDLE_MS = 20000;

export function fwAtLeast(fw, maj, min, pat){
  const v = (fw || "").split(".").map(n=>parseInt(n, 10) || 0);
  const want = [maj, min, pat || 0];
  for(let i = 0; i < 3; i++){ if((v[i] || 0) !== want[i]) return (v[i] || 0) > want[i]; }
  return true;
}

function rawFetch(cmd, name, progress){
  return new Promise((resolve, reject)=>{
    if(!state.connected || !state.chars.cmd){ reject(new Error("not connected")); return; }
    if(state.dl.mode!==null){ reject(new Error("another transfer is in progress")); return; }
    let timer = null;
    const finish = (fn, v)=>{ clearInterval(timer); fn(v); };
    state.dl = { mode:null, expect:0, got:0, chunks:[], name:name||"file", sink:"promise", raw:true, lastRx:Date.now(),
                 progress, resolve:v=>finish(resolve, v), reject:e=>finish(reject, e) };
    const mine = state.dl;
    timer = setInterval(()=>{
      if(state.dl!==mine){ clearInterval(timer); return; }
      if(Date.now()-mine.lastRx > IDLE_MS){ resetDl(); finish(reject, new Error("transfer timed out")); }
    }, 1000);
    state.chars.cmd.writeValue(enc.encode(cmd)).catch(e=>{ if(state.dl===mine) resetDl(); finish(reject, e); });
  });
}

// How a listed file relates to its cached copy: "full" = nothing new on the device, "part" = the device copy
// grew (fresh = new bytes), "none" = not cached. With file_ids on both sides a cached copy at least as big is
// current (files only grow); without, only an equal size counts.
export function cacheStatus(path, size, fileId){
  if(!cacheUsable() || !state.scannerId) return { st:"none", fresh:0 };
  const m = find(state.scannerId, path, fileId);
  if(!m) return { st:"none", fresh:0 };
  if(size > 0 && (fileId && m.fileId===fileId ? m.size >= size : m.size === size)) return { st:"full", fresh:0 };
  if(m.fileId && (!fileId || m.fileId===fileId) && size > m.size) return { st:"part", fresh:size - m.size };
  return { st:"none", fresh:0 };
}

export async function fetchLog(path, o){
  o = o || {};
  state.xferBusy = (state.xferBusy || 0) + 1;
  try{ return await fetchLogInner(path, o); }
  finally{ state.xferBusy--; }
}
async function fetchLogInner(path, o){
  const size = o.size || 0, fileId = o.fileId || "", name = o.name || path.split("/").pop();
  const sid = state.scannerId;
  await ready;
  const useCache = cacheUsable() && !!sid;
  const meta = useCache ? find(sid, path, fileId) : null;
  if(meta && cacheStatus(path, size, fileId).st === "full"){
    const cached = await getBlob(meta);
    if(cached && cached.size === meta.size){
      log("Using the saved copy of "+name+" ("+cached.size+" bytes) — no transfer.");
      return { text:await cached.text(), blob:cached, how:"hit" };
    }
  }
  const r = await rawFetch("F:"+path, name, o.progress);
  if(typeof r === "string") return { text:r, blob:null, how:"full" };   // empty file, or an older download.js (text only)
  if(r.blob.size !== r.n) throw new Error("short transfer ("+r.blob.size+" of "+r.n+" bytes)");
  if(useCache) await put(sid, path, r.blob);
  return { text:await r.blob.text(), blob:r.blob, how:"full" };
}

// The device's current session as a cacheable file. STATUS sess= is the active stream's file name without
// its extension, under /logs/. Null when that can't be trusted (no scanner id yet, odd name) -> the caller
// keeps using the plain "L" command.
export async function fetchCurrent(name){
  const sess = ((document.getElementById("sessName").textContent) || "").replace(/^Session:\s*/, "").trim();
  if(!state.scannerId || !/^[A-Za-z0-9_-]{1,40}$/.test(sess)) return null;
  return fetchLog("/logs/"+sess+".jsonl", { name:name || sess+".jsonl" });
}
