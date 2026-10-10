// Scan-log parsers. The CYD logs NDJSON (.jsonl); older sessions are legacy CSV. Both are
// normalized to the SAME row shape so the map and Scan Viewer don't care which they loaded.

// Parse a scanlog CSV into row objects with all 16 columns (shared by the map and Scan Viewer).
// Columns are located by header name, so extra/reordered columns still parse.
// v5 added `pan` after `uuid`; older (v4) logs lack the header name, so pan reads as "".
const CSV_COLS=["epoch","ms_since_boot","lat","lon","source","mac","rssi","channel","ie","cid","uuid","pan","name","score","tier","signature"];
export function parseScanCsv(text){
  const lines=(text||"").split(/\r?\n/);
  if(lines.length<2) return [];
  const h=lines[0].split(","), ix={}; CSV_COLS.forEach(n=>ix[n]=h.indexOf(n));
  if(ix.lat<0||ix.lon<0) return [];
  const rows=[];
  for(let i=1;i<lines.length;i++){
    if(!lines[i]) continue;
    const c=lines[i].split(","); if(c.length<=ix.lon) continue;
    const g=n=>ix[n]>=0?(c[ix[n]]||""):"";
    rows.push({epoch:parseInt(g("epoch"),10)||0, ms:parseInt(g("ms_since_boot"),10)||0,
      lat:parseFloat(g("lat")), lon:parseFloat(g("lon")), source:g("source"), mac:g("mac"),
      rssi:parseInt(g("rssi"),10)||0, channel:g("channel"), ie:g("ie"), cid:g("cid"), uuid:g("uuid"), pan:g("pan"),
      name:g("name"), score:parseInt(g("score"),10)||0, tier:g("tier")||"none", signature:g("signature"), wl:false});
  }
  return rows;
}
// NDJSON log (one JSON object per line, schema keys: epoch ms lat lon src mac rssi ch ie cid
// uuid pan name score tier sig wl; blank/inapplicable keys are omitted). Normalized to the SAME
// row shape parseScanCsv returns. Bad lines are skipped.
export function parseScanNdjson(text){
  const rows=[];
  for(const ln of (text||"").split(/\r?\n/)){
    if(!ln.trim()) continue;
    let o; try{ o=JSON.parse(ln); }catch(e){ continue; }
    if(!o || typeof o!=="object" || !o.mac) continue;
    const s=v=>v==null?"":String(v);
    rows.push({epoch:parseInt(o.epoch,10)||0, ms:parseInt(o.ms,10)||0,
      lat:o.lat==null?NaN:parseFloat(o.lat), lon:o.lon==null?NaN:parseFloat(o.lon),
      source:s(o.src), mac:s(o.mac), rssi:parseInt(o.rssi,10)||0, channel:s(o.ch),
      ie:s(o.ie), cid:s(o.cid), uuid:s(o.uuid), pan:s(o.pan), name:s(o.name),
      score:parseInt(o.score,10)||0, tier:s(o.tier)||"none", signature:s(o.sig),
      flags:s(o.flags), wl:(o.wl==1||o.wl===true)});
  }
  return rows;
}
// Detect the log format: first non-empty char "{" = NDJSON, else legacy CSV.
export function parseScanLog(text){
  const t=(text||"").trimStart();
  return t[0]==="{" ? parseScanNdjson(t) : parseScanCsv(t);
}
// Event lines (NDJSON objects with an "evt" key: deauth / eviltwin / droneid, no "mac") are skipped by
// the row parser above; this collects the geolocated ones for the map's event pins.
export function parseScanEvents(text){
  const out=[];
  for(const ln of (text||"").split(/\r?\n/)){
    if(ln[0]!=="{" || !ln.includes('"evt"')) continue;
    let o; try{ o=JSON.parse(ln); }catch(e){ continue; }
    if(!o || !o.evt) continue;
    out.push({evt:String(o.evt), count:parseInt(o.count,10)||0, epoch:parseInt(o.epoch,10)||0,
      ms:parseInt(o.ms,10)||0, lat:o.lat==null?NaN:parseFloat(o.lat), lon:o.lon==null?NaN:parseFloat(o.lon)});
  }
  return out;
}
