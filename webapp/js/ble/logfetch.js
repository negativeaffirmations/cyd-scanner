// Fetching a scanner log file through the phone-side cache (data/logcache.js).
//   fetchLog(path, {size, fileId, name, gen, trim})  -> {text, blob, how}   how: "hit" (no transfer) | "tail" | "full"
//   fetchCurrent(name, opts)                         -> same, for the device's CURRENT session; null = not applicable
//   cacheStatus(path, size, fileId)                  -> {st:"none"|"part"|"full"|"path", fresh} for the Data page tags
// Every caller gets the WHOLE file (uploads must carry line 1's meta + the timesync anchors), whether it
// came from the cache, from the device, or from a cached prefix plus the device's new tail.
// Raw fetch: like ble/transfer.js bleFetch but resolves with the exact bytes (a Blob), because the cache
// must hold byte-identical copies (a text round trip would change the size of a torn UTF-8 tail).
// download.js hands a {kind,blob,...} object to a request marked state.dl.raw (an older cached download.js
// resolves plain text instead -> still works, just uncached).
// Safety: one request in flight (ble/xfer.js reqBusy); the connection generation + scanner id are captured
// at the start and re-checked before every request and before anything is cached, so a Switch scanner
// mid-fetch aborts instead of mixing two scanners' bytes; a full fetch is cached only if it is a "file"
// reply whose file_id matches the one the list promised.
// NEW file: other modules only import it, so a stale cached copy of an old module can't miss a name.
import { state } from '../state.js';
import { enc } from '../config.js';
import { log } from '../util/dom.js';
import { resetDl } from './transfer.js';
import { connGen, reqBusy } from './xfer.js';
import { ready, cacheUsable, find, getBlob, put, fileIdOf } from '../data/logcache.js';

const IDLE_MS = 20000;

export function fwAtLeast(fw, maj, min, pat){
  const v = (fw || "").split(".").map(n=>parseInt(n, 10) || 0);
  const want = [maj, min, pat || 0];
  for(let i = 0; i < 3; i++){ if((v[i] || 0) !== want[i]) return (v[i] || 0) > want[i]; }
  return true;
}
// fw >= 0.26.0 answers "F:<path>@<offset>:<file_id>:<crc>" with only the bytes after <offset> (TAIL=<n>@<offset>).
export const canTail = ()=>fwAtLeast(state.scannerFw, 0, 26, 0);

// CRC-32 (IEEE) of the 64 bytes before the join point: the device recomputes it from its file, so a file that was
// replaced under the same line 1 is refused instead of having unrelated bytes appended.
let crcTable = null;
function crc32hex(bytes){
  if(!crcTable){ crcTable = new Uint32Array(256); for(let n = 0; n < 256; n++){ let c = n; for(let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1); crcTable[n] = c >>> 0; } }
  let c = 0xFFFFFFFF;
  for(let i = 0; i < bytes.length; i++) c = crcTable[(c ^ bytes[i]) & 255] ^ (c >>> 8);
  return ((c ^ 0xFFFFFFFF) >>> 0).toString(16).padStart(8, "0");
}

function rawFetch(cmd, name, progress){
  return new Promise((resolve, reject)=>{
    if(!state.connected || !state.chars.cmd){ reject(new Error("not connected")); return; }
    if(reqBusy()){ reject(new Error("another transfer is in progress")); return; }
    let timer = null;
    const finish = (fn, v)=>{ clearInterval(timer); fn(v); };
    state.dl = { mode:null, expect:0, got:0, chunks:[], name:name||"file", sink:"promise", raw:true, waiting:true, lastRx:Date.now(),
                 progress, resolve:v=>finish(resolve, v), reject:e=>finish(reject, e) };
    const mine = state.dl;
    timer = setInterval(()=>{
      if(state.dl!==mine){ if(!mine.completing) finish(reject, new Error("transfer superseded")); return; }
      if(Date.now()-mine.lastRx > IDLE_MS){ resetDl(); finish(reject, new Error("transfer timed out")); }
    }, 1000);
    state.chars.cmd.writeValue(enc.encode(cmd)).catch(e=>{ if(state.dl===mine) resetDl(); finish(reject, e); });
  });
}
async function waitIdle(){   // after a failed request: let the device's late bytes drain before the next request
  for(let i = 0; i < 25 && reqBusy(); i++) await new Promise(r=>setTimeout(r, 200));
  await new Promise(r=>setTimeout(r, 800));
}

// How a listed file relates to its cached copy: "full" = nothing new on the device, "part" = the device copy
// grew (fresh = new bytes; only when both file_ids are known and equal), "path" = a copy with that name exists but
// it can't be shown to be the same file, "none" = not cached. With file_ids on both sides a cached copy at least
// as big is current (files only grow); without, only an equal size counts.
export function cacheStatus(path, size, fileId){
  if(!cacheUsable() || !state.scannerId) return { st:"none", fresh:0 };
  const m = find(state.scannerId, path, fileId);
  if(!m) return { st:"none", fresh:0 };
  const sameId = !!fileId && m.fileId === fileId;
  if(size > 0 && (sameId ? m.size >= size : m.size === size)) return { st:"full", fresh:0 };
  if(sameId && size > m.size) return { st:"part", fresh:size - m.size };
  return { st:"path", fresh:0 };
}

export async function fetchLog(path, o){
  o = o || {};
  state.xferBusy = (state.xferBusy || 0) + 1;
  try{
    const r = await fetchLogInner(path, o);
    if(o.trim && r.text && !r.text.endsWith("\n")){   // an upload never carries a torn last row of a live file
      const i = r.text.lastIndexOf("\n");
      r.text = i >= 0 ? r.text.slice(0, i + 1) : "";
    }
    return r;
  }
  finally{ state.xferBusy--; }
}
async function fetchLogInner(path, o){
  const size = o.size || 0, fileId = o.fileId || "", name = o.name || path.split("/").pop();
  const gen = o.gen !== undefined ? o.gen : connGen(), sid = state.scannerId;
  const alive = ()=>{ if(connGen() !== gen || (sid && state.scannerId !== sid)) throw new Error("the scanner changed during the transfer"); };
  alive();
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
  // A saved prefix of an append-only file + the device's new tail = the whole file again. The device checks
  // line 1's file_id, the offset and the CRC of the 64 bytes before it; any refusal, size mismatch or failed
  // request just falls through to the full fetch.
  if(meta && meta.fileId && canTail() && (size === 0 || meta.size < size)){
    const base = await getBlob(meta);
    if(base && base.size === meta.size){
      try{
        alive();
        const crc = crc32hex(new Uint8Array(await base.slice(Math.max(0, meta.size - 64), meta.size).arrayBuffer()));
        const t = await rawFetch("F:"+path+"@"+meta.size+":"+meta.fileId+":"+crc, name, o.progress);
        alive();
        if(t && t.kind === "tail" && t.off === meta.size && t.blob.size === t.n){
          if(t.n === 0){
            log("Saved copy of "+name+" is current — no transfer.");
            return { text:await base.text(), blob:base, how:"hit" };
          }
          const merged = new Blob([base, t.blob]);
          if(await fileIdOf(merged) === meta.fileId){
            log("Fetched only the new "+t.n+" bytes of "+name+" (saved copy was "+base.size+").");
            await put(sid, path, merged);
            return { text:await merged.text(), blob:merged, how:"tail" };
          }
        }
        log("Tail of "+name+" not available ("+(t && t.why ? t.why : "unexpected reply")+") — fetching the whole file.");
      }catch(err){
        if(connGen() !== gen) throw err;
        log("Tail of "+name+" failed ("+err.message+") — fetching the whole file.");
        await waitIdle();
      }
    }
  }
  alive();
  const r = await rawFetch("F:"+path, name, o.progress);
  alive();
  if(typeof r === "string") return { text:r, blob:null, how:"full" };   // empty file, or an older download.js (text only)
  if(r.kind !== "file") throw new Error("unexpected reply from the scanner ("+r.kind+")");
  if(r.blob.size !== r.n) throw new Error("short transfer ("+r.blob.size+" of "+r.n+" bytes)");
  if(fileId && await fileIdOf(r.blob) !== fileId) throw new Error("the file on the scanner changed — refresh the session list");
  if(useCache) await put(sid, path, r.blob);
  return { text:await r.blob.text(), blob:r.blob, how:"full" };
}

// The device's current session as a cacheable file. STATUS sess= (this connection's, cleared on disconnect) is
// the active stream's file name without its extension, under /logs/. Null when that can't be trusted (no STATUS yet
// on this connection, no scanner id, odd name) -> the caller keeps using the plain "L" command.
export async function fetchCurrent(name, o){
  const sess = state.status && state.status.sess;
  if(!state.scannerId || !sess || !/^[A-Za-z0-9_-]{1,40}$/.test(sess)) return null;
  return fetchLog("/logs/"+sess+".jsonl", Object.assign({ name:name || sess+".jsonl" }, o || {}));
}
