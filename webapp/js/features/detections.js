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
import { renderCatChips } from './categories.js';

let detState = { expect:0, rows:[], rendered:false };
let curSeq = -1;
let emptyStreak = 0;
const expandedMacs = new Set();   // carat state survives snapshot re-renders

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
const detHeadLive =
  '<div class="dethead"><span class="tiercol" title="Threat tier">⚠</span>'+
  '<span class="sigcol">Signals</span><span class="hnm">Device</span>'+
  '<span class="rssi">RSSI</span><span class="caret"></span></div>';
const detHeadViewer =
  '<div class="dethead"><span class="tiercol" title="Threat tier">⚠</span>'+
  '<span class="sigcol">Source</span><span class="hnm">Device</span>'+
  '<span class="rssi">RSSI</span><span class="tcol">Time</span></div>';
// Shared detection-row renderer. opts.mode "live": rows = {tier,mac,rssi,ie,name,sources[]},
// multi-source pills + expand caret. "viewer": rows = parseScanCsv rows, single source pill +
// timecode, whole-row click -> opts.onRowClick(row). Same .det/.sp/.badge/.srcN styling for both.
export function renderDetList(el, rows, opts){
  const viewer = opts.mode === "viewer";
  const head = viewer ? detHeadViewer : detHeadLive;
  if (!rows.length){
    if (!viewer) expandedMacs.clear();
    el.innerHTML = head + '<div class="sub" style="padding:6px">'+opts.empty+'</div>'; return; }
  if (!viewer) for (const m of [...expandedMacs]) if (!rows.some(r => r.mac === m)) expandedMacs.delete(m);  // keep the Set bounded
  el.innerHTML = head + rows.map((r,ri)=>{
    const tn = typeof r.tier === "number" ? r.tier : tierNum(r.tier);
    const ft = viewer ? 0 : (r.ftier||0);                 // follow tier (live rows only)
    const muted = !viewer && r.muted;
    const fmk = ft>=1 ? `<span class="fmark f${ft}" title="${ft===2?'Possible follower':'Persistent'}">${ft===2?'⚑':'•'}</span> ` : "";
    const nm = fmk + (esc(r.name) || esc(r.mac) || "&lt;hidden&gt;") + (viewer ? "" : flagBadges(r.flags||0));
    const title = "MAC "+r.mac + (r.ie && r.ie!=="00000000" ? "  IE "+r.ie : "");
    const srcs = (viewer ? [{tag:r.source, rssi:r.rssi}] : r.sources).slice().sort((a,b)=>b.rssi-a.rssi);
    const tags = []; srcs.forEach(s=>{ if(!tags.includes(s.tag)) tags.push(s.tag); });  // distinct bands
    const pills = tags.map(t=>`<span class="sp ${pillClass[t]||""}">${esc(t)}</span>`).join("") || "?";
    const sc = srcs.length ? (srcClass[srcs[0].tag] || "") : "";  // tint row by strongest source
    const multi = !viewer && srcs.length > 1;      // only multi-source live devices get an expander
    const open = multi && expandedMacs.has(r.mac);
    const sub = multi ? srcs.map(s=>`<div class="sub1 ${srcClass[s.tag]||""}">${esc(s.tag)}  ${s.rssi} dBm</div>`).join("") : "";
    const ch = viewer && r.channel ? `<span class="chn">ch${esc(r.channel)}</span>` : "";
    return `<div class="det ${sc}${viewer?" clk":""}${muted?" muted":""}${ft===2?" following":""}" title="${title}"${viewer?` data-ri="${ri}"`:""}><span class="tiercol"><span class="badge b${tn}"></span></span>`+
           `<span class="sigcol">${pills}</span>`+
           `<span class="nm t${tn}">${nm}${ch}</span>`+
           `<span class="rssi">${r.rssi} dBm</span>`+
           (viewer ? `<span class="tcol">${fmtTimecode(r)}</span>`
                   : multi ? `<span class="caret" data-mac="${esc(r.mac)}" data-ri="${ri}">${open?"▾":"▸"}</span>`
                           : `<span class="caret"></span>`)+
           `</div>`+
           (multi ? `<div class="detsub" id="dsub${ri}" style="display:${open?"block":"none"}">${sub}</div>` : "");
  }).join("");
  if (viewer) {
    el.querySelectorAll(".det[data-ri]").forEach(d=>{ d.onclick = ()=> opts.onRowClick(rows[+d.dataset.ri]); });
    return;
  }
  el.querySelectorAll(".caret[data-mac]").forEach(c=>{
    c.onclick = ()=>{
      const sub = $("dsub"+c.dataset.ri), mac = c.dataset.mac;
      const show = sub.style.display === "none";
      sub.style.display = show ? "block" : "none";
      c.textContent = show ? "▾" : "▸";
      if (show) expandedMacs.add(mac); else expandedMacs.delete(mac);
    };
  });
}
export function renderDets(rows){   // live view
  state.lastDetRows = rows;
  renderDetList($("dets"), rows, {mode:"live", empty:"No devices in range yet…"});
  renderCatChips(rows);
}
