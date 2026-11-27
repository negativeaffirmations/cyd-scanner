// "Also in range": the live list past the scanner's top 12. The DETS stream carries only the 12 rows the scanner shows
// first (threats, then strongest); the rest come from the phone's live copy of the log (analysis/engine.js
// presentDevices(): logged and no "lastseen" line since). Those rows change only when the scanner logs a row (first
// sight, then every 5 min while in range), so their RSSI can be a few minutes old.
// Names: the logged advert name, else the GATT read (data/devinfo.js). Tap -> the same device popup as a live row.
// Hooks: none in old modules (stale-module rule). It polls state.lastDetRows (a new array per DETS snapshot) and
// listens to the engine / devinfo changes. NEW file.
import { $ } from '../util/dom.js';
import { state } from '../state.js';
import { tierNum } from '../util/format.js';
import { renderDetList } from './detections.js';
import { openLiveDetail } from './categories.js';
import { preferName, onDevinfo } from '../data/devinfo.js';

const MAX_AGE_S = 7 * 60;   // a present device is re-logged every 5 min
const MAX_ROWS = 150;

let lastRows = null, dirty = true, lastPaint = 0;

function passFilter(r){   // same rules as detections.js liveFilter()
  const f = state.liveFilter; if(!f) return true;
  if(f.threats && (typeof r.tier === "number" ? r.tier : tierNum(r.tier)) < 1) return false;
  const tags = (r.sources || []).map(s=>s.tag);
  return !tags.length || tags.some(t=>f.src[t] !== false);
}

function render(){
  const box = $("detsMore"), list = $("detsMoreList"), hdr = $("detsMoreHdr");
  if(!box || !list) return;
  const eng = state.engine;
  const live = state.connected && state.status && state.status.scan === "1";
  if(!eng || !eng.presentDevices || !live){ box.hidden = true; list.innerHTML = ""; return; }
  if(state.paused) return;   // the live list is frozen: keep this part frozen with it
  const top = new Set((state.lastDetRows || []).map(r=>String(r.mac || "").toUpperCase()));
  let rows = eng.presentDevices(MAX_AGE_S).filter(r=>!top.has(r.mac));
  for(const r of rows) if(r.sources.some(s=>s.tag === "BLE")){ const p = preferName(r.name, r.mac); if(p.gatt){ r.name = p.name; r.nsrc = "g"; } }
  const total = rows.length;
  rows = rows.filter(passFilter).sort((a, b)=>(b.tier - a.tier) || (b.rssi - a.rssi)).slice(0, MAX_ROWS);
  box.hidden = total === 0;
  if(!total){ list.innerHTML = ""; return; }
  if(hdr) hdr.innerHTML = `Also in range <span class="sub">· ${total} · from the log, RSSI as last logged</span>`;
  renderDetList(list, rows, { mode:"live", onRowClick:openLiveDetail, empty:"No devices match the filters…" });
}

export function initMoreList(){
  const anchor = $("dets");
  if(anchor && !$("detsMore")){   // built here so an older index.html still gets it
    const box = document.createElement("div");
    box.id = "detsMore"; box.hidden = true;
    box.innerHTML = `<h4 id="detsMoreHdr" class="detsmorehdr">Also in range</h4><div id="detsMoreList" class="dets tall"></div>`;
    anchor.after(box);
  }
  const mark = ()=>{ dirty = true; };
  if(state.engine) state.engine.onEngineChange(mark);
  onDevinfo(mark);
  setInterval(()=>{
    if(state.lastDetRows !== lastRows){ lastRows = state.lastDetRows; dirty = true; }
    const box = $("detsMore");
    if(box && !box.hidden && !(state.connected && state.status && state.status.scan === "1")) dirty = true;
    if(dirty || Date.now() - lastPaint >= 3000){ dirty = false; lastPaint = Date.now(); render(); }   // also ages rows out + follows filter changes
  }, 1000);
}
