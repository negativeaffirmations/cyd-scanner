// Explore Scan: pick a past session, browse it in the Scan Viewer, tap a row for details.
import { state } from '../state.js';
import { $ } from '../util/dom.js';
import { tierNum, fmtEpoch, hasGps } from '../util/format.js';
import { refreshSessions, setExploreLoader } from './sessions.js';
import { parseScanLog } from '../data/parse.js';
import { renderDetList, setScanMode } from './detections.js';
import { openDetail } from './detail.js';

export function openExplore(){
  $("exploreView").style.display="";
  if(!state.connected){ $("expMsg").textContent="Connect to the scanner first."; return; }
  state.exploreWant = true;                  // populateSessions will auto-load the newest
  $("expMsg").textContent="Loading session list…";
  refreshSessions();
}
setExploreLoader((text, name)=>loadedExplore(parseScanLog(text), name));   // sessions.js fetches + joins a scan's parts
let expName="";                              // name of the loaded session (state.expAll holds the rows)
const TYPE_ORDER={"2.4":0,"5G":1,"BLE":2,"PRB":3,"154":4};
export function loadedExplore(rows, name){
  state.expAll = rows.map((r,i)=>(r.idx=i, r)); expName = name;   // idx = file order (chronological)
  // New session: reset only the time window to its own extent; other filters stay as the user set them.
  const ep = rows.map(r=>r.epoch).filter(e=>e>0);
  const t0=$("efT0"), t1=$("efT1");
  const lo = ep.length ? Math.min(...ep) : 0, hi = ep.length ? Math.max(...ep) : 0;
  [t0,t1].forEach(t=>{ t.min=lo; t.max=hi; t.disabled=!ep.length; });
  t0.value=lo; t1.value=hi;
  applyExplore();
}
// The log re-logs a device every few minutes while it stays in range (fresh position/RSSI), so one
// device can own many rows. Show ONE row per device (source+MAC): the latest row's fields, the
// strongest RSSI, the highest-scoring tier/signature, first-seen order, and every GPS'd sighting
// (r.sight, plotted as a trail by the detail modal).
function collapseDevices(rows){
  const m=new Map();
  for(const r of rows){
    const k=r.source+"|"+r.mac;
    let g=m.get(k);
    if(!g){ g={...r, nSeen:0, sight:[], rssiMax:r.rssi, best:r}; m.set(k,g); }
    else Object.assign(g, r, {idx:g.idx, nSeen:g.nSeen, sight:g.sight, rssiMax:Math.max(g.rssiMax,r.rssi),
                              best:(r.score>g.best.score ? r : g.best), firstEpoch:g.firstEpoch});
    if(g.firstEpoch==null) g.firstEpoch=r.epoch;
    g.nSeen++;
    if(hasGps(r)) g.sight.push({lat:r.lat, lon:r.lon, tier:tierNum(r.tier), rssi:r.rssi, t:r.epoch>0?r.epoch*1000:0});
  }
  return [...m.values()].map(g=>{
    g.rssi=g.rssiMax; g.tier=g.best.tier; g.score=g.best.score; g.signature=g.best.signature;
    delete g.best; delete g.rssiMax;
    return g;
  });
}
function distLabel(v){
  return (v<=-100 ? "All" : v<=-80 ? "Far" : v<=-60 ? "Mid" : "Near")+" (≥ "+v+" dBm)";
}
export function applyExplore(){   // filter -> sort -> render (no re-download)
  const on={"2.4":$("ef24").checked,"5G":$("ef5").checked,"BLE":$("efble").checked,"PRB":$("efprb").checked,"154":$("ef154").checked};
  const thr=$("efThreats").checked, minR=+$("efDist").value;
  const t0=+$("efT0").value, t1=+$("efT1").value;
  const full = t0===+$("efT0").min && t1===+$("efT0").max;   // unsynced (epoch 0) rows pass only at full extent
  const q=$("efText").value.trim().toLowerCase();
  $("efDistVal").textContent=distLabel(minR);
  $("efTVal").textContent = $("efT0").disabled ? "no synced timestamps" : fmtEpoch(t0).slice(11)+" – "+fmtEpoch(t1).slice(11);
  let rows=state.expAll.filter(r=>
    on[r.source]!==false && (!thr || tierNum(r.tier)>0) && r.rssi>=minR &&
    (r.epoch>0 ? (r.epoch>=t0 && r.epoch<=t1) : full) &&
    (!q || (r.name||"").toLowerCase().includes(q) || (r.mac||"").toLowerCase().includes(q)));
  const nRows=rows.length;
  rows=collapseDevices(rows);
  const ty=r=>TYPE_ORDER[r.source]??9, byIdx=(a,b)=>a.idx-b.idx;
  const cmp={
    old:byIdx, new:(a,b)=>b.idx-a.idx,
    typeNew:(a,b)=>ty(a)-ty(b)||b.idx-a.idx, typeOld:(a,b)=>ty(a)-ty(b)||a.idx-b.idx,
    close:(a,b)=>b.rssi-a.rssi||a.idx-b.idx, far:(a,b)=>a.rssi-b.rssi||a.idx-b.idx,
    az:(a,b)=>(a.name||"").localeCompare(b.name||"")||a.idx-b.idx,
    za:(a,b)=>(b.name||"").localeCompare(a.name||"")||a.idx-b.idx
  }[$("expSort").value]||byIdx;
  rows=rows.slice().sort(cmp);
  $("expMsg").textContent = expName ? expName+": "+rows.length+" device(s) ("+nRows+" of "+state.expAll.length+" rows). Tap a row for details." : "";
  setScanMode(false);   // viewing a loaded log, not the live scan
  renderDetList($("scanViewer"), rows, {mode:"viewer", empty:"No detections match.", onRowClick:openDetail});
}
