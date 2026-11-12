// .cflo scan archive (docs/cflo-format.md v1): one ZIP per scan = manifest.json (STORED, first entry) + each log file
// byte-exact under logs/<scanner_id>/<file_id>.jsonl (DEFLATE). Built here from files the app already fetched; the
// scanners keep writing plain NDJSON. Also reads one back (Map "Load file").
// No zip library: a small PKZIP 2.0 writer/reader over the browser's CompressionStream / DecompressionStream.

const te = new TextEncoder(), td = new TextDecoder();
export const CFLO_TYPE = "application/vnd.cointelflo+zip";
const MAX_ENTRY = 256*1024*1024, MAX_TOTAL = 1024*1024*1024;   // inflate caps (spec §1)

let crcTab = null;
function crc32(u8){
  if(!crcTab){
    crcTab = new Uint32Array(256);
    for(let n=0;n<256;n++){ let c=n; for(let k=0;k<8;k++) c = c&1 ? 0xEDB88320^(c>>>1) : c>>>1; crcTab[n]=c>>>0; }
  }
  let c = 0xFFFFFFFF;
  for(let i=0;i<u8.length;i++) c = crcTab[(c^u8[i])&0xFF]^(c>>>8);
  return (c^0xFFFFFFFF)>>>0;
}
async function pipe(u8, stream){ return new Uint8Array(await new Response(new Blob([u8]).stream().pipeThrough(stream)).arrayBuffer()); }
async function sha256hex(u8){
  const h = new Uint8Array(await crypto.subtle.digest("SHA-256", u8));
  return Array.from(h, b=>b.toString(16).padStart(2,"0")).join("");
}
export const cfloSupported = ()=> typeof CompressionStream==="function" && typeof DecompressionStream==="function" && !!(crypto && crypto.subtle);

// First line of a log file as an object when it is the file's {"evt":"meta"} line, else null.
function metaOf(u8){
  let end = u8.indexOf(10); if(end<0) end = u8.length;
  try{ const o = JSON.parse(td.decode(u8.subarray(0, Math.min(end, 4096)))); return o && o.evt==="meta" ? o : null; }catch(e){ return null; }
}
// [first, last] nonzero row epoch in a file (head 64 KB / tail 64 KB), for a file the session list gave no span.
function epochSpan(u8){
  const re = /"epoch":([1-9]\d{8,})/g, head = td.decode(u8.subarray(0, 65536)), tail = td.decode(u8.subarray(Math.max(0, u8.length-65536)));
  const h = re.exec(head); let m, last = 0; re.lastIndex = 0;
  while((m = re.exec(tail))) last = +m[1];
  return [h ? +h[1] : 0, last];
}
const streamOf = base => base.startsWith("bg-") ? "bg" : base.startsWith("fleet-") ? "fleet" : "fg";
const HEX = /^[0-9a-f]+$/;

// Forwarded member meta lines in a fleet file ("ofile" + "evt":"meta") -> [{scanner_id, scanner_name, fw, db_gen}].
function memberMetas(text){
  const out = [];
  let i = 0;
  while((i = text.indexOf('"evt":"meta"', i)) >= 0){
    const a = text.lastIndexOf("\n", i)+1, b = text.indexOf("\n", i);
    i = b<0 ? text.length : b;
    const ln = text.slice(a, b<0 ? text.length : b);
    if(!ln.includes('"ofile"')) continue;
    try{ const o = JSON.parse(ln); if(o.scanner_id) out.push(o); }catch(e){}
  }
  return out;
}

// files: [{bytes?: Uint8Array, text?: string, device_path, first, last}] in the order they go in the archive (own files
// first, then fleet). opts: {ownerSid, build}. Returns {blob, manifest, name}.
export async function buildCflo(files, opts){
  const o = opts || {};
  const entries = [], units = new Map(), nofid = new Map();
  let fleetMeta = null, ownFid = "", start = 0, end = 0;
  for(const f of files){
    let u8 = f.bytes || te.encode(f.text || "");
    const nl = u8.lastIndexOf(10), keep = nl+1, trimmed = u8.length-keep;   // a torn last line (no \n) is cut + counted
    if(trimmed) u8 = u8.subarray(0, keep);
    if(!u8.length) continue;
    const meta = metaOf(u8) || {};
    const base = (f.device_path||"").split("/").pop();
    const stream = meta.stream==="fleet" ? "fleet" : streamOf(base);
    const sid = HEX.test(meta.scanner_id||"") ? meta.scanner_id : (o.ownerSid || "unknown");
    let fid = /^[0-9a-f]{8}$/.test(meta.file_id||"") ? meta.file_id : "";
    if(!fid){ const n = (nofid.get(sid)||0)+1; nofid.set(sid, n); fid = "nofid-"+n; }
    let lines = 0; for(let i=0;i<u8.length;i++) if(u8[i]===10) lines++;
    const path = "logs/"+sid+"/"+fid+".jsonl";
    if(entries.some(e=>e.path===path)) continue;   // the same file twice (overlapping selections): keep one
    if(!f.first || !f.last){ const sp = epochSpan(u8); f.first = f.first || sp[0]; f.last = f.last || sp[1]; }   // no listed span
    entries.push({path, u8, info:{path, scanner_id:sid, file_id:fid, stream, device_path:f.device_path||"", bytes:u8.length, lines,
      sha256:await sha256hex(u8), first_epoch:f.first||0, last_epoch:f.last||0, trimmed_tail_bytes:trimmed}});
    if(f.first && (!start || f.first<start)) start = f.first;
    if(f.last && f.last>end) end = f.last;
    if(stream==="fleet"){
      fleetMeta = meta;
      if(!units.has(sid)) units.set(sid, {scanner_id:sid, scanner_name:meta.scanner_name, fw:meta.fw, db_gen:meta.db_gen, role:"leader"});
      else units.get(sid).role = "leader";
      for(const m of memberMetas(td.decode(u8)))
        if(!units.has(m.scanner_id)) units.set(m.scanner_id, {scanner_id:m.scanner_id, scanner_name:m.scanner_name, fw:m.fw, db_gen:m.db_gen, role:"client"});
    } else {
      if(!ownFid) ownFid = fid;
      if(!units.has(sid)) units.set(sid, {scanner_id:sid, scanner_name:meta.scanner_name, fw:meta.fw, db_gen:meta.db_gen,
                                          role:meta.fleet_role==="client" ? "client" : meta.fleet_role==="leader" ? "leader" : "solo"});
    }
  }
  if(!entries.length) return null;
  const owner = o.ownerSid || entries[0].info.scanner_id;
  const scan = {id: owner+"-"+(ownFid || entries[0].info.file_id), kind: fleetMeta ? "fleet" : "solo"};
  if(fleetMeta){ scan.leader = entries.find(e=>e.info.stream==="fleet").info.scanner_id; if(fleetMeta.fleet_id) scan.fleet_id = fleetMeta.fleet_id; }
  scan.start_epoch = start; scan.end_epoch = end;
  try{ const tz = Intl.DateTimeFormat().resolvedOptions().timeZone; if(tz) scan.tz = tz; }catch(e){}
  scan.tz_offset_min = -new Date().getTimezoneOffset();
  const clean = u=>Object.fromEntries(Object.entries(u).filter(([,v])=>v!==undefined && v!==""));
  const manifest = {format:"cflo", version:1, created_utc:new Date().toISOString().replace(/\.\d+Z$/,"Z"),
    created_by:{app:"cyd-scanner-webapp", build:o.build||""}, scan, units:[...units.values()].map(clean), files:entries.map(e=>e.info)};
  const blob = await zip([{path:"manifest.json", u8:te.encode(JSON.stringify(manifest, null, 2)), store:true}, ...entries]);
  return {blob, manifest};
}

// --- ZIP writer (PKZIP 2.0, UTF-8 names, no data descriptors) ---
async function zip(items){
  const d = new Date(), time = (d.getHours()<<11)|(d.getMinutes()<<5)|(d.getSeconds()>>1),
        date = ((Math.max(1980, d.getFullYear())-1980)<<9)|((d.getMonth()+1)<<5)|d.getDate();
  const parts = [], central = [];
  let off = 0;
  for(const it of items){
    const name = te.encode(it.path), crc = crc32(it.u8);
    let data = it.u8, method = 0;
    if(!it.store){ const z = await pipe(it.u8, new CompressionStream("deflate-raw")); if(z.length < it.u8.length){ data = z; method = 8; } }
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true); h.setUint16(8, method, true);
    h.setUint16(10, time, true); h.setUint16(12, date, true); h.setUint32(14, crc, true);
    h.setUint32(18, data.length, true); h.setUint32(22, it.u8.length, true); h.setUint16(26, name.length, true); h.setUint16(28, 0, true);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(8, 0x0800, true);
    c.setUint16(10, method, true); c.setUint16(12, time, true); c.setUint16(14, date, true); c.setUint32(16, crc, true);
    c.setUint32(20, data.length, true); c.setUint32(24, it.u8.length, true); c.setUint16(28, name.length, true);
    c.setUint32(42, off, true);
    parts.push(new Uint8Array(h.buffer), name, data);
    central.push(new Uint8Array(c.buffer), name);
    off += 30 + name.length + data.length;
  }
  const cdSize = central.reduce((s,x)=>s+x.length, 0);
  const e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, items.length, true); e.setUint16(10, items.length, true);
  e.setUint32(12, cdSize, true); e.setUint32(16, off, true);
  if(off + cdSize > 0xFFFFFFFE) throw new Error("archive too big (would need ZIP64)");
  return new Blob([...parts, ...central, new Uint8Array(e.buffer)], {type:CFLO_TYPE});
}

// True when the bytes start like a .cflo: PK\x03\x04 and a first entry named manifest.json.
export function isCflo(u8){
  if(u8.length < 43 || u8[0]!==0x50 || u8[1]!==0x4b || u8[2]!==3 || u8[3]!==4) return false;
  const n = u8[26] | (u8[27]<<8);
  return n===13 && td.decode(u8.subarray(30, 43))==="manifest.json";
}

// Read a .cflo (ArrayBuffer/Uint8Array) -> {manifest, files:[{path, text, info}]} in manifest order. Throws on anything
// the spec refuses or on a checksum mismatch.
export async function readCflo(buf){
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  if(!isCflo(u8)) throw new Error("not a .cflo archive (no manifest.json first)");
  let eo = -1;
  for(let i=u8.length-22; i>=Math.max(0, u8.length-65557); i--) if(dv.getUint32(i, true)===0x06054b50){ eo = i; break; }
  if(eo<0) throw new Error("damaged archive (no end record)");
  const count = dv.getUint16(eo+10, true), cdOff = dv.getUint32(eo+16, true);
  if(count===0xFFFF || cdOff===0xFFFFFFFF) throw new Error("ZIP64 archives are not supported");
  const ents = new Map();
  let p = cdOff, total = 0;
  for(let k=0;k<count;k++){
    if(p+46>u8.length || dv.getUint32(p, true)!==0x02014b50) throw new Error("damaged archive (central directory)");
    const flags = dv.getUint16(p+8, true), method = dv.getUint16(p+10, true), crc = dv.getUint32(p+16, true);
    const csize = dv.getUint32(p+20, true), usize = dv.getUint32(p+24, true);
    const nl = dv.getUint16(p+28, true), xl = dv.getUint16(p+30, true), cl = dv.getUint16(p+32, true), lo = dv.getUint32(p+42, true);
    const name = td.decode(u8.subarray(p+46, p+46+nl));
    p += 46+nl+xl+cl;
    if(flags & 1) throw new Error("encrypted archives are not supported");
    if(method!==0 && method!==8) throw new Error("unsupported compression in "+name);
    if(csize===0xFFFFFFFF || usize===0xFFFFFFFF || lo===0xFFFFFFFF) throw new Error("ZIP64 archives are not supported");
    if(name.includes("..") || name.startsWith("/") || name.includes("\\")) throw new Error("unsafe entry name "+name);
    if(ents.has(name)) throw new Error("duplicate entry "+name);
    if(usize>MAX_ENTRY || (total += usize)>MAX_TOTAL) throw new Error("archive too big when unpacked");
    ents.set(name, {name, method, crc, csize, usize, lo});
  }
  const get = async e=>{
    if(dv.getUint32(e.lo, true)!==0x04034b50) throw new Error("damaged archive ("+e.name+")");
    const ds = e.lo+30+dv.getUint16(e.lo+26, true)+dv.getUint16(e.lo+28, true);
    const raw = u8.subarray(ds, ds+e.csize);
    const out = e.method===8 ? await pipe(raw, new DecompressionStream("deflate-raw")) : raw;
    if(out.length!==e.usize || crc32(out)!==e.crc) throw new Error("damaged entry "+e.name);
    return out;
  };
  const me = ents.get("manifest.json");
  if(!me || me.lo!==0 || me.method!==0) throw new Error("manifest.json must be the first, uncompressed entry");
  let manifest;
  try{ manifest = JSON.parse(td.decode(await get(me))); }catch(e){ throw new Error("manifest.json is not valid JSON"); }
  if(manifest.format!=="cflo") throw new Error("not a .cflo manifest");
  if(!(manifest.version<=1)) throw new Error(".cflo version "+manifest.version+" is newer than this app");
  const files = [];
  for(const info of manifest.files || []){
    const e = ents.get(info.path);
    if(!e) throw new Error("missing entry "+info.path);
    const out = await get(e);
    if(out.length!==info.bytes || await sha256hex(out)!==info.sha256) throw new Error("checksum mismatch in "+info.path);
    files.push({path:info.path, info, text:td.decode(out)});
  }
  return {manifest, files};
}

// Archive file name: <first own file base>.cflo (e.g. 20261111-171630.cflo).
export function cfloName(firstBase){ return (firstBase||"scan").replace(/\.(jsonl|csv)$/i, "")+".cflo"; }
