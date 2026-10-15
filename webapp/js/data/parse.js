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
  const rows=[]; let sync=null;
  for(const ln of (text||"").split(/\r?\n/)){
    if(!ln.trim()) continue;
    let o; try{ o=JSON.parse(ln); }catch(e){ continue; }
    if(o && o.evt==="timesync"){ const e=parseInt(o.epoch,10)||0; if(e>0) sync={epoch:e, ms:parseInt(o.ms,10)||0}; continue; }
    if(!o || typeof o!=="object" || !o.mac || o.evt) continue;   // evt lines (incl. "rid", which carries a mac) are events, not rows
    const s=v=>v==null?"":String(v);
    rows.push({epoch:parseInt(o.epoch,10)||0, ms:parseInt(o.ms,10)||0,
      lat:o.lat==null?NaN:parseFloat(o.lat), lon:o.lon==null?NaN:parseFloat(o.lon),
      source:s(o.src), mac:s(o.mac), rssi:parseInt(o.rssi,10)||0, channel:s(o.ch),
      ie:s(o.ie), cid:s(o.cid), uuid:s(o.uuid), pan:s(o.pan), name:s(o.name),
      score:parseInt(o.score,10)||0, tier:s(o.tier)||"none", signature:s(o.sig),
      flags:s(o.flags), wl:(o.wl==1||o.wl===true)});
  }
  backfillEpoch(rows, sync);   // pre-sync rows (epoch:0) -> real time from their ms + the timesync anchor
  return rows;
}
// Pre-sync rows/events carry epoch:0 + ms-since-boot; the CYD appends one {"evt":"timesync"} anchor
// when the phone sets the clock. Recover their real time in place: epoch = sync.epoch + (ms - sync.ms)/1000
// (the anchor usually appears AFTER the pre-sync rows, so this is applied after the full parse pass).
function backfillEpoch(items, sync){
  if(!sync) return;
  for(const it of items) if(!it.epoch) it.epoch = sync.epoch + Math.round((it.ms - sync.ms)/1000);
}
// Detect the log format: first non-empty char "{" = NDJSON, else legacy CSV.
export function parseScanLog(text){
  const t=(text||"").trimStart();
  return t[0]==="{" ? parseScanNdjson(t) : parseScanCsv(t);
}
// Event lines (NDJSON objects with an "evt" key: deauth / eviltwin / droneid / rid / follow) are skipped by
// the row parser above; this collects the geolocated ones for the map's event pins.
export function parseScanEvents(text){
  const out=[]; let sync=null;
  for(const ln of (text||"").split(/\r?\n/)){
    if(ln[0]!=="{" || !ln.includes('"evt"')) continue;
    let o; try{ o=JSON.parse(ln); }catch(e){ continue; }
    if(!o || !o.evt) continue;
    if(o.evt==="timesync"){ const e=parseInt(o.epoch,10)||0; if(e>0) sync={epoch:e, ms:parseInt(o.ms,10)||0}; continue; }  // anchor, not a map pin
    const ev={evt:String(o.evt), count:parseInt(o.count,10)||0, epoch:parseInt(o.epoch,10)||0,
      ms:parseInt(o.ms,10)||0, lat:o.lat==null?NaN:parseFloat(o.lat), lon:o.lon==null?NaN:parseFloat(o.lon)};
    if(ev.evt==="eviltwin"){   // suspect SSID + its BSSIDs (CYD enriches the line, up to 8 macs)
      ev.ssid=o.ssid==null?"":String(o.ssid);
      ev.macs=o.macs==null?[]:String(o.macs).split(",").filter(Boolean);
    }
    if(ev.evt==="rid"){   // Remote-ID decode: absolute drone/operator fix + telemetry (absent -> NaN / "")
      const f=v=>v==null?NaN:parseFloat(v), n=v=>v==null?NaN:parseInt(v,10), s=v=>v==null?"":String(v);
      Object.assign(ev,{dlat:f(o.dlat), dlon:f(o.dlon), olat:f(o.olat), olon:f(o.olon),
        h:n(o.h), alt:n(o.alt), spd:f(o.spd), hdg:n(o.hdg), uas:s(o.uas), oid:s(o.oid), mac:s(o.mac)});
    }
    if(ev.evt==="follow"){   // FOLLOWING device: the scanner's own fix + who it was (mac/score/name/src/flags)
      Object.assign(ev,{mac:o.mac==null?"":String(o.mac), score:parseInt(o.score,10)||0,
        name:o.name==null?"":String(o.name), src:o.src==null?"":String(o.src), flags:o.flags==null?"":String(o.flags)});
    }
    out.push(ev);
  }
  backfillEpoch(out, sync);   // pre-sync events (epoch:0) -> real time from the timesync anchor
  return out;
}
