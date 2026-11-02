// Phone-side cache of the scanner's log files (IndexedDB), so a file fetched over BLE (~10 KB/s) is
// never fetched twice: Download, then Upload, then Map reuse one copy, and a growing live file only
// needs its new tail (ble/logfetch.js). Log files are append-only, which is what makes this safe.
//
// Key = scanner sid + file_id (the random id on line 1's meta since fw 0.25.0, fixed for the file's
// life and surviving the rename on time sync); older files without one are keyed sid + path.
// Two stores: "meta" (small records the UI reads synchronously from the in-memory copy `metas`) and
// "data" (the file as a Blob; Chrome keeps Blobs on disk, so a 100 MB cache costs little page memory).
// Capped at CAP_BYTES with least-recently-used eviction; any storage failure (private mode, quota)
// degrades to "no cache" — every caller falls back to a normal fetch.
// NEW file (never served stale next to a fresh main.js); other modules add no names to old modules.

const DB_NAME = "cflo.logcache", STORE_META = "meta", STORE_DATA = "data";
export const CAP_BYTES = 200 * 1024 * 1024;

let metas = [];                  // [{key,sid,path,fileId,size,savedAt,lastUsed}] — mirror of the meta store
let dbp = null;
let usable = true;               // false once IndexedDB can't be opened
const listeners = new Set();

export function onCacheChange(fn){ listeners.add(fn); }
function changed(){ listeners.forEach(fn=>{ try{ fn(); }catch(e){} }); }

function openDb(){
  if(dbp) return dbp;
  dbp = new Promise((resolve, reject)=>{
    if(!window.indexedDB){ reject(new Error("no IndexedDB")); return; }
    const r = indexedDB.open(DB_NAME, 1);
    r.onupgradeneeded = ()=>{ r.result.createObjectStore(STORE_META, { keyPath:"key" }); r.result.createObjectStore(STORE_DATA); };
    r.onsuccess = ()=>resolve(r.result);
    r.onerror = ()=>reject(r.error || new Error("IndexedDB open failed"));
    r.onblocked = ()=>reject(new Error("IndexedDB blocked"));
  });
  return dbp;
}
// Run fn(metaStore, dataStore) in one transaction; resolves with fn's return value (an IDBRequest -> its
// result) once the transaction commits.
async function tx(mode, fn){
  const db = await openDb();
  return new Promise((resolve, reject)=>{
    const t = db.transaction([STORE_META, STORE_DATA], mode);
    let out;
    try{ out = fn(t.objectStore(STORE_META), t.objectStore(STORE_DATA)); }catch(e){ try{ t.abort(); }catch(_){} reject(e); return; }
    t.oncomplete = ()=>resolve(out instanceof IDBRequest ? out.result : out);
    t.onerror = ()=>reject(t.error || new Error("IndexedDB error"));
    t.onabort = ()=>reject(t.error || new Error("IndexedDB aborted"));
  });
}

export const ready = (async ()=>{
  try{
    metas = (await tx("readonly", m=>m.getAll())) || [];
    try{ if(navigator.storage && navigator.storage.persist) navigator.storage.persist(); }catch(e){}   // ask not to be evicted (best effort)
  }catch(e){ usable = false; metas = []; }
  changed();
})();

export const cacheUsable = ()=>usable;

// The 8-hex file_id on line 1's meta ("" when the file has none).
export async function fileIdOf(blob){
  try{
    const head = await blob.slice(0, 1024).text();
    const m = /"file_id":"([0-9a-f]{8})"/.exec(head.split("\n", 1)[0]);
    return m ? m[1] : "";
  }catch(e){ return ""; }
}

// Synchronous lookup against the in-memory index. fileId (from the SESS= list, "" on older firmware)
// finds the entry whatever the file is called now; else match by path (an entry that knows its
// file_id still matches by path when the list can't supply one).
export function find(sid, path, fileId){
  if(!sid) return null;
  if(fileId){ const m = metas.find(x=>x.sid===sid && x.fileId===fileId); if(m) return m; }
  return metas.find(x=>x.sid===sid && x.path===path && (!fileId || x.fileId===fileId)) || null;
}
export function totalBytes(){ return metas.reduce((s, m)=>s + m.size, 0); }
export function entryCount(){ return metas.length; }

// The cached file's bytes (a Blob), or null. Touches the LRU stamp.
export async function getBlob(meta){
  if(!usable || !meta) return null;
  try{
    const b = await tx("readonly", (m, d)=>d.get(meta.key));
    if(!b) return null;
    meta.lastUsed = Date.now();
    tx("readwrite", m=>{ m.put(meta); }).catch(()=>{});
    return b;
  }catch(e){ return null; }
}

// Store (or replace) a file. Returns the new meta, or null when it can't be cached (too big, quota, no IndexedDB).
export async function put(sid, path, blob){
  if(!usable || !sid || !blob || !blob.size || blob.size > CAP_BYTES) return null;
  const fileId = await fileIdOf(blob);
  const key = sid + "|" + (fileId ? "id:" + fileId : "p:" + path);
  const now = Date.now();
  const rec = { key, sid, path, fileId, size:blob.size, savedAt:now, lastUsed:now };
  // The same path cached before the file had a known id is superseded by this copy.
  const stale = metas.filter(m=>m.sid===sid && m.key!==key && m.path===path && !m.fileId);
  for(let attempt = 0; attempt < 2; attempt++){
    const drop = new Set(stale.map(m=>m.key));
    const others = metas.filter(m=>m.key!==key && !drop.has(m.key));
    let total = others.reduce((s, m)=>s + m.size, 0);
    // First try: just stay under the cap. Retry after a quota error: free about half of what we hold.
    const budget = attempt ? Math.floor(total / 2) : CAP_BYTES - blob.size;
    for(const m of others.sort((a, b)=>a.lastUsed - b.lastUsed)){ if(total <= budget) break; drop.add(m.key); total -= m.size; }
    try{
      await tx("readwrite", (m, d)=>{
        drop.forEach(k=>{ m.delete(k); d.delete(k); });
        d.put(blob, key); m.put(rec);
      });
      metas = metas.filter(x=>!drop.has(x.key) && x.key!==key); metas.push(rec);
      changed();
      return rec;
    }catch(e){
      if(!(e && (e.name === "QuotaExceededError" || /quota/i.test(e.message || "")))) return null;   // any other failure: just don't cache
    }
  }
  return null;
}

export async function clearAll(){
  try{ await tx("readwrite", (m, d)=>{ m.clear(); d.clear(); }); }catch(e){}
  metas = []; changed();
}
