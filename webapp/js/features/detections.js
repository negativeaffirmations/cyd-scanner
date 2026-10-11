// Live detections (DETS stream) + the shared detection-row renderer (used by the live list and
// the Explore Scan Viewer).
//
// DETS: "D:<seq>:<n>" header then n rows (seq-tagged atomic snapshot, one row per MAC):
// seq tier mac bestRssi ie name srcList ("tag:rssi,tag:rssi") [ftier fscore muted flags cat] —
// v4 appended the decimal flag bits (iBeacon 8, Find My 16, Pwnagotchi 32, Evil Twin 64, Drone RID 128);
// the ftier/fscore/muted three (follow tier 0/1/2, follow score, muted flag) were added in v3;
// v5 appended the category id. Rows from older firmware have only the first 7 fields and parse fine.
import { state } from '../state.js';
import { $, esc } from '../util/dom.js';
import { tierNum, fmtTimecode, flagBadges } from '../util/format.js';
import { renderCatChips, renderScanCounts, openLiveDetail } from './categories.js';

let detState = { expect:0, rows:[], rendered:false };
let curSeq = -1;
let emptyStreak = 0;

// If the current snapshot accumulated rows but never reached its expected count
// (a row or the header was lost over BLE), show what we have instead of dropping it.
function flushPendingDets(){
  if (detState.rows.length && !detState.rendered) {
    renderDets(detState.rows); detState.rendered = true; emptyStreak = 0;
  }
}
export function onDets(ev){
  const txt = new TextDecoder().decode(ev.target.value);
  if (txt.startsWith("D:")) {                       // new snapshot header: D:<seq>:<n>
    const h = txt.slice(2).split(":");
    flushPendingDets();                             // render prior partial before starting fresh
    curSeq = parseInt(h[0],10);
    detState = { expect: parseInt(h[1],10) || 0, rows:[], rendered:false };
    if (detState.expect === 0 && ++emptyStreak >= 3) { renderDets([]); detState.rendered = true; }
    return;
  }
  const p = txt.split("\t");                        // seq tier mac rssi ie name srcList
  if (p.length < 7) return;                         // malformed/truncated
  const seq = +p[0];
  if (seq !== curSeq) {                             // header was lost -> rows start the snapshot
    flushPendingDets();
    curSeq = seq; detState = { expect:0, rows:[], rendered:false };
  }
  if (detState.rows.some(r => r.mac === p[2])) return;  // dedup by MAC within the snapshot
  const sources = p[6].split(",").filter(Boolean).map(x=>{
    const k = x.lastIndexOf(":");
    return {tag:x.slice(0,k), rssi:+x.slice(k+1)};
  });
  detState.rows.push({seq, tier:+p[1], mac:p[2], rssi:+p[3], ie:p[4], name:p[5], sources,
                      ftier: p.length>7 ? (+p[7]||0) : 0,   // DETS v3: follow tier (0/1/2)
                      fscore: p.length>8 ? (+p[8]||0) : 0,   // follow score 0..100
                      muted: p.length>9 ? p[9]==="1" : false,
                      flags: p.length>10 ? (+p[10]||0) : 0,     // DETS v4: notable flag bits
                      cat: p.length>11 ? (+p[11]||0) : 0});     // DETS v5: threat category id
  if (detState.expect && detState.rows.length >= detState.expect) {
    renderDets(detState.rows); detState.rendered = true; emptyStreak = 0;  // atomic full replace
  }
}

const srcClass  = {"2.4":"src24", "5G":"src5", "BLE":"srcble", "PRB":"srcprb", "154":"src154"};  // row tint
const pillClass = {"2.4":"p24",   "5G":"p5",  "BLE":"pble",   "PRB":"pprb",   "154":"p154"};    // band pill

// Shared detection-row renderer (live list + Explore viewer): TALL, 2-line, whole-row tappable ->
// opts.onRowClick(row). Line 1 = name (+ follow/flag markers) and strongest RSSI; line 2 = source
// pills + MAC (+ channel / timecode for viewer rows). Same .detrow styling for both so the two lists
// look identical (ui-component-reuse). Row is tinted + left-barred by its strongest source.
export function renderDetList(el, rows, opts){
  const viewer = opts.mode === "viewer";
  if (!rows.length){ el.innerHTML = '<div class="sub" style="padding:8px">'+opts.empty+'</div>'; return; }
  el.innerHTML = rows.map((r,ri)=>{
    const tn = typeof r.tier === "number" ? r.tier : tierNum(r.tier);
    const ft = viewer ? 0 : (r.ftier||0);                 // follow tier (live rows only)
    const muted = !viewer && r.muted;
    const fmk = ft>=1 ? `<span class="fmark f${ft}" title="${ft===2?'Possible follower':'Persistent'}">${ft===2?'⚑':'•'}</span> ` : "";
    const nm = fmk + (esc(r.name) || esc(r.mac) || "&lt;hidden&gt;") + (viewer ? "" : flagBadges(r.flags||0));
    const srcs = (viewer ? [{tag:r.source, rssi:r.rssi}] : (r.sources||[])).slice().sort((a,b)=>b.rssi-a.rssi);
    const tags = []; srcs.forEach(s=>{ if(!tags.includes(s.tag)) tags.push(s.tag); });  // distinct bands
    const pills = tags.map(t=>`<span class="sp ${pillClass[t]||""}">${esc(t)}</span>`).join("") || "?";
    const sc = srcs.length ? (srcClass[srcs[0].tag] || "") : "";  // tint row by strongest source
    const extra = [esc(r.mac || "")];
    if (viewer && r.channel) extra.push("ch"+esc(r.channel));
    if (viewer) extra.push(fmtTimecode(r));
    return `<div class="detrow clk ${sc}${muted?" muted":""}${ft===2?" following":""}" data-ri="${ri}">`+
           `<span class="tiercol"><span class="badge b${tn}"></span></span>`+
           `<div class="detmain">`+
             `<div class="r1"><span class="nm t${tn}">${nm}</span><span class="rssi">${r.rssi} dBm</span></div>`+
             `<div class="r2"><span class="sigcol">${pills}</span><span class="r2sub">${extra.filter(Boolean).join("  ·  ")}</span></div>`+
           `</div></div>`;
  }).join("");
  el.querySelectorAll(".detrow[data-ri]").forEach(d=>{ d.onclick = ()=> opts.onRowClick && opts.onRowClick(rows[+d.dataset.ri]); });
}
// Live-list filter (from the Filters modal): keep rows whose tier/source pass. Counts + threat strip
// still reflect ALL detections — only the on-screen list is filtered.
function liveFilter(rows){
  const f = state.liveFilter; if (!f) return rows;
  return rows.filter(r=>{
    if (f.threats && (typeof r.tier==="number" ? r.tier : tierNum(r.tier)) < 1) return false;
    const tags = (r.sources||[]).map(s=>s.tag);
    if (tags.length && !tags.some(t=> f.src[t] !== false)) return false;
    return true;
  });
}
export function applyLiveFilter(){   // called when a Filters-modal control changes
  state.liveFilter = { threats: $("lfThreats").checked, src: {
    "2.4":$("lf24").checked, "5G":$("lf5").checked, "BLE":$("lfble").checked,
    "PRB":$("lfprb").checked, "154":$("lf154").checked } };
  renderDets(state.lastDetRows || []);
}
// Scan title indicator: 'radar' (live scan, sweeping) vs 'description' (a loaded log file).
export function setScanMode(live){
  const el = $("scanModeIcon"); if (!el) return;
  el.textContent = live ? "radar" : "description";
  el.title = live ? "Live scan" : "Loaded log";
}
export function renderDets(rows){   // live view
  state.lastDetRows = rows;
  setScanMode(true);
  const shown = liveFilter(rows);
  renderDetList($("dets"), shown, {mode:"live", onRowClick: openLiveDetail,
    empty: rows.length ? "No devices match the filters…" : "No devices in range yet…"});
  renderCatChips(rows);       // counts + strip reflect ALL detections, not the filtered subset
  renderScanCounts(rows);
}
