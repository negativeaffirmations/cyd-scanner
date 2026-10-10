// "Following me" UI: a magenta banner (count from STATUS fol=) opens a flagged list,
// each row opens a detail modal mirroring the CYD follow-detail (+ a GPS track when available).
import { state } from '../state.js';
import { $, esc } from '../util/dom.js';
import { tierNum, hasGps } from '../util/format.js';
import { TIER_NAMES } from '../config.js';
import { newMap } from './map.js';

const FTIER_TXT = {2:"FOLLOWING", 1:"PERSISTENT", 0:"none"};
function followRows(){   // flagged live rows, FOLLOWING first then by score, then signal
  return state.lastDetRows.filter(r=>(r.ftier||0) >= 1)
    .sort((a,b)=> (b.ftier-a.ftier) || (b.fscore-a.fscore) || (b.rssi-a.rssi));
}
export function openFollowList(){
  const rows = followRows();
  const el = $("folList");
  if (!rows.length){
    // The DETS stream is capped at the top 12 devices, so a flagged follower ranked below
    // that won't be in the live snapshot even when the banner count (STATUS fol=) is > 0.
    const nf = +$("folCount").textContent || 0;
    el.innerHTML = '<div class="sub" style="padding:6px">' +
      (nf > 0 ? nf + ' follower(s) flagged on the device, but not in the current live top-12 list.'
              : 'No active followers right now.') + '</div>';
  } else {
    el.innerHTML = rows.map((r,ri)=>{
      const nm = esc(r.name) || esc(r.mac) || "&lt;hidden&gt;";
      const fc = r.ftier===2 ? "f2" : "f1";
      return `<div class="det clk" data-ri="${ri}"><span class="tiercol"><span class="fmark ${fc}">${r.ftier===2?'⚑':'•'}</span></span>`+
             `<span class="nm">${nm}</span><span class="folscore">${r.fscore}/100</span>`+
             `<span class="rssi">${r.rssi} dBm</span></div>`;
    }).join("");
    el.querySelectorAll(".det[data-ri]").forEach(d=>{ d.onclick = ()=> openFollowDetail(rows[+d.dataset.ri]); });
  }
  $("folDlg").showModal();
}
let folMap=null, folLayer=null;
function openFollowDetail(r){
  const tn = typeof r.tier === "number" ? r.tier : tierNum(r.tier);
  const rowsKv = [
    ["Name", r.name || r.mac || "<hidden>"],
    ["MAC", r.mac],
    ["Follow tier", FTIER_TXT[r.ftier] || "none"],
    ["Follow score", (r.fscore||0) + " / 100"],
    ["Threat tier", TIER_NAMES[tn] || "none"],
    ["RSSI", r.rssi + " dBm"],
    ["Signals", (r.sources||[]).map(s=>s.tag).join(", ") || "?"],
  ];
  if (r.muted) rowsKv.push(["Whitelisted","yes (muted)"]);
  if (r.ie && !/^0+$/.test(r.ie)) rowsKv.push(["IE fingerprint", r.ie]);
  $("folDetInfo").innerHTML = rowsKv.map(([k,v])=>
    `<div class="kv"><span class="k">${k}</span><span class="vv t${k==="Threat tier"?tn:0}">${esc(String(v))}</span></div>`).join("");
  // Map: plot this device's GPS sightings from the loaded Explore session if present; else
  // fall back to the phone's current position; else hide the map with a hint.
  const track = state.expAll.filter(x => (x.mac||"").toUpperCase() === (r.mac||"").toUpperCase() && hasGps(x));
  const haveHere = !!(state.lastGps && state.lastGps.lat);
  const show = track.length > 0 || haveHere;
  $("folDetMap").style.display = show ? "" : "none";
  $("folDetMapMsg").style.display = show ? "none" : "";
  if (!show) $("folDetMapMsg").textContent =
    "No GPS track for this device. Load its session in Explore, or enable GPS to show your position.";
  $("folDetDlg").showModal();
  if (!show) return;
  if (!folMap){ folMap = newMap("folDetMap"); folLayer = L.layerGroup().addTo(folMap); }
  setTimeout(()=>{
    folMap.invalidateSize(); folLayer.clearLayers();
    if (track.length){
      const pts = track.map(x=>[x.lat,x.lon]);
      pts.forEach(p=> L.circleMarker(p,{radius:6,color:"#ff5cbf",weight:2,fillColor:"#ff5cbf",fillOpacity:.6}).addTo(folLayer));
      if (pts.length>1) L.polyline(pts,{color:"#ff5cbf",weight:2,opacity:.6}).addTo(folLayer);
      folMap.fitBounds(L.latLngBounds(pts).pad(.3));
    } else {
      L.circleMarker([state.lastGps.lat,state.lastGps.lon],{radius:9,color:"#58a6ff",weight:2,fillColor:"#58a6ff",fillOpacity:.7})
        .addTo(folLayer).bindPopup("Your current position");
      folMap.setView([state.lastGps.lat,state.lastGps.lon],16);
    }
  }, 80);
}
