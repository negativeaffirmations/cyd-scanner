// "Server data" dialog (#srvDlg, Map page button #srvFindBtn): look things up on the paired scan-analyzer server.
//   - Devices near me: GET /api/v1/devices/near?lat&lon&radius_m[&category][&limit] around the scanner's position
//     (STATUS pos=, as the log stamps it), else the phone's own fix; nearest first, plotted on a small map.
//   - Search: GET /api/v1/devices/search?q (advertised name, or part of a MAC with or without separators).
// Tapping a result opens the usual device popup (#detDlg); serverhist.js then adds that device's server history,
// notify.js its "Notify me" button. Contract: scan-analyzer docs/api-contract.md "Read API - v1" (Bearer key with
// read). Server epochs are LOCAL seconds -> formatted with UTC getters.
// NEW file (stale-module rule): only main.js imports it. It does NOT import serverhist.js (a second, differently
// keyed URL would load a second copy with its own popup observer).
import { $, esc } from '../util/dom.js';
import { state } from '../state.js';
import { TIER_COL, CAT_COL } from '../config.js';
import { isPaired, serverUrl } from './remote.js';
import { scannerFix } from '../gps.js';
import { newMap } from './map.js';
import { renderDetailMap } from './detail.js';
import { fillDetailWhitelist } from './whitelist.js';
import { deviceClusterLayer, addDots } from './cluster.js?v=20261121b';

const TIMEOUT_MS = 15000;
const CAT_KEYS = ["", "flock", "axon", "alpr", "cam", "ring", "raven", "glass", "tracker", "drone", "deauth", "flipper", "skim"];
const CAT_LABEL = ["Other", "Flock", "Axon", "ALPR", "Cam", "Ring", "Raven", "Glass", "Tracker", "Drone", "Deauth", "Flipper", "Skim"];
const TIER_IDX = { none:0, suspect:1, likely:2, confirmed:3 };
const CLASS_TXT = { stationary:"fixed spot", mobile:"moves around", indeterminate:"estimated spot" };
const MON = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];

let seq = 0;              // bumps per request; an older reply is dropped
let rows = [];            // current results
let here = null;          // {lat,lon} the near search ran from (null for a name search)
let fmap = null, flayer = null, fdots = null;   // flayer = you-are-here + estimate circles, fdots = the grouped device dots

function key(){ try{ return localStorage.getItem("rs.key") || ""; }catch(e){ return ""; } }
async function getJson(path){
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), TIMEOUT_MS);
  try{
    const r = await fetch(serverUrl().replace(/\/+$/, "") + path,
      { headers:{ "Authorization":"Bearer " + key() }, signal:ctl.signal, cache:"no-store" });
    let body = null; try{ body = await r.json(); }catch(e){}
    return { status:r.status, body };
  }catch(e){
    return { status:0, body:null, err: e.name === "AbortError" ? "no reply in " + (TIMEOUT_MS/1000) + " s" : e.message };
  }finally{ clearTimeout(t); }
}
function errText(r){
  if(r.status === 403) return "This phone's key may not read server data. Turn on read for it on the server's admin page (Keys tab).";
  if(r.status === 401) return "The server didn't accept this phone's key. Pair again in Settings → Remote Server.";
  if(r.status === 422) return "The server refused the request" + (r.body && r.body.detail && typeof r.body.detail === "string" ? ": " + r.body.detail : ".");
  if(r.status === 404) return "This server doesn't offer that yet.";
  return "Couldn't reach the server (" + (r.err || "HTTP " + r.status) + ").";
}

const okLL = (a, b) => a != null && b != null && isFinite(a) && isFinite(b) && !(a === 0 && b === 0);
const catIdx = k => Math.max(0, CAT_KEYS.indexOf(k || ""));
function dateTxt(e){ if(!(e > 0)) return ""; const d = new Date(e * 1000); return MON[d.getUTCMonth()] + " " + d.getUTCDate() + ", " + d.getUTCFullYear(); }
function distTxt(m){ return m == null ? "" : m < 1000 ? Math.round(m) + " m" : (m / 1000).toFixed(m < 10000 ? 1 : 0) + " km"; }
function label(r){
  const n = (r.name || "").trim();
  if(n && n.toUpperCase() !== (r.mac || "").toUpperCase()) return n;
  return r.vendor ? r.vendor + " " + (r.mac || "").slice(12) : (r.mac || "?");
}
function msg(t, err){ const el = $("srvFindMsg"); if(el){ el.textContent = t || ""; el.classList.toggle("err", !!err); } }

// Where "near me" is: the scanner's own position (what its log stamps right now), else this phone's fix.
function phoneFix(){
  return new Promise(res => {
    if(!navigator.geolocation){ res(null); return; }
    navigator.geolocation.getCurrentPosition(p => res({ lat:p.coords.latitude, lon:p.coords.longitude }),
      () => res(null), { enableHighAccuracy:true, timeout:10000, maximumAge:15000 });
  });
}

async function findNear(){
  if(!isPaired()){ msg("Pair with your server first (Settings → Remote Server).", true); return; }
  const s = ++seq;
  msg("Getting your position…");
  let pos = scannerFix(), from = "the scanner's position";
  if(!pos){ pos = await phoneFix(); from = "this phone's position"; }
  if(s !== seq) return;
  if(!pos){ msg("No position: connect a scanner with a GPS fix, or allow location for this page.", true); return; }
  const radius = +$("srvRadius").value || 500, cat = $("srvCat").value, threats = $("srvThreats").checked;
  let q = "/api/v1/devices/near?lat=" + pos.lat.toFixed(6) + "&lon=" + pos.lon.toFixed(6) + "&radius_m=" + radius +
          "&limit=" + (threats ? 1000 : 200);
  if(cat) q += "&category=" + encodeURIComponent(cat);
  msg("Asking the server…");
  const r = await getJson(q);
  if(s !== seq) return;
  if(r.status !== 200 || !Array.isArray(r.body)){ msg(errText(r), true); return; }
  let list = r.body;
  if(threats) list = list.filter(d => (TIER_IDX[d.tier] || 0) > 0);
  here = { lat:pos.lat, lon:pos.lon };
  rows = list.slice(0, 200);
  msg(rows.length ? rows.length + " device" + (rows.length === 1 ? "" : "s") + " within " + distTxt(radius) + " of " + from + "."
                  : "Nothing the server knows of within " + distTxt(radius) + " of " + from + ".");
  renderList(); renderMapView();
}

async function search(){
  if(!isPaired()){ msg("Pair with your server first (Settings → Remote Server).", true); return; }
  const qv = ($("srvQ").value || "").trim();
  if(qv.length < 2){ msg("Type at least 2 characters (a name, or part of a MAC).", true); return; }
  const s = ++seq;
  msg("Searching…");
  const r = await getJson("/api/v1/devices/search?q=" + encodeURIComponent(qv.slice(0, 64)) + "&limit=100");
  if(s !== seq) return;
  if(r.status !== 200 || !Array.isArray(r.body)){ msg(errText(r), true); return; }
  here = null;
  rows = r.body;
  msg(rows.length ? rows.length + " match" + (rows.length === 1 ? "" : "es") + ", most recently seen first." : "No device matches “" + qv + "”.");
  renderList(); renderMapView();
}

function renderList(){
  const el = $("srvFindList"); if(!el) return;
  el.innerHTML = rows.map((r, i) => {
    const tn = TIER_IDX[r.tier] || 0, ci = catIdx(r.threat_cat);
    const bits = [];
    if(here && r.distance_m != null) bits.push(esc(distTxt(r.distance_m)) + (r.position === "estimate" ? " · " + esc(CLASS_TXT[r.classification] || "estimated spot") : " · last seen here"));
    if(r.last_seen > 0) bits.push("last " + esc(dateTxt(r.last_seen)));
    if(r.sightings) bits.push(esc(r.sightings + " sighting" + (r.sightings === 1 ? "" : "s")));
    return `<div class="srvres" data-i="${i}" role="button" tabindex="0">` +
      `<span class="badge b${tn}"></span>` +
      `<div class="m"><div class="r1"><span class="nm t${tn}">${esc(label(r))}</span>` +
      (ci ? `<span class="srvcat" style="border-color:${CAT_COL[ci]};color:${CAT_COL[ci]}">${CAT_LABEL[ci]}</span>` : "") + `</div>` +
      `<div class="r2">${esc(r.mac || "")}${bits.length ? "  ·  " + bits.join("  ·  ") : ""}</div></div></div>`;
  }).join("");
  el.querySelectorAll(".srvres").forEach(d => {
    const open = () => openDevice(rows[+d.dataset.i]);
    d.onclick = open;
    d.onkeydown = e => { if(e.key === "Enter" || e.key === " "){ e.preventDefault(); open(); } };
  });
}

function renderMapView(){
  const box = $("srvFindMap"); if(!box) return;
  const pts = rows.filter(r => okLL(r.lat, r.lon));
  box.hidden = !(here || pts.length);
  if(box.hidden) return;
  if(!fmap){ fmap = newMap("srvFindMap"); flayer = L.layerGroup().addTo(fmap); fdots = deviceClusterLayer(fmap); }
  setTimeout(() => {
    fmap.invalidateSize(); flayer.clearLayers(); fdots.clearLayers();
    const all = [], dots = [];
    if(here){
      all.push([here.lat, here.lon]);
      L.circleMarker([here.lat, here.lon], { radius:7, color:"#ffffff", weight:3, fillColor:"#388bfd", fillOpacity:1 })
        .addTo(flayer).bindPopup("You are here");
    }
    pts.forEach(r => {   // overlapping dots are grouped (features/cluster.js); a tap on the bubble fans them out
      const col = TIER_COL[TIER_IDX[r.tier] || 0], p = [r.lat, r.lon];
      all.push(p);
      if(r.position === "estimate" && r.radius_m > 0)
        L.circle(p, { radius:r.radius_m, color:col, weight:1, dashArray:"3 4", fillOpacity:.05, interactive:false }).addTo(flayer);
      const m = L.circleMarker(p, { radius:6, color:col, weight:2, fillColor:col, fillOpacity:.75, mac:r.mac, cat:catIdx(r.threat_cat) });
      m.bindTooltip(esc(label(r)));
      m.on("click", () => openDevice(r));
      dots.push(m);
    });
    addDots(fdots, dots);
    if(all.length > 1) fmap.fitBounds(L.latLngBounds(all).pad(0.2));
    else if(all.length) fmap.setView(all[0], 16);
  }, 80);
}

// The usual device popup for a server row. serverhist.js sees the new #detInfo and loads this MAC's history.
function openDevice(r){
  if(!r || !r.mac) return;
  const tn = TIER_IDX[r.tier] || 0, ci = catIdx(r.threat_cat);
  const kvs = [["Name", label(r)], ["MAC", r.mac], ["Category", CAT_LABEL[ci]], ["Threat tier", r.tier || "none"]];
  if(r.vendor) kvs.push(["Vendor", r.vendor]);
  if(r.category) kvs.push(["Device kind", r.category]);
  if(here && r.distance_m != null) kvs.push(["Distance", distTxt(r.distance_m) + (r.position === "estimate" ? " (estimated spot)" : " (last fix)")]);
  kvs.push(["From", "your server (not seen live now)"]);
  $("detInfo").innerHTML = kvs.map(([k, v]) =>
    `<div class="kv"><span class="k">${k}</span><span class="vv t${k === "Threat tier" ? tn : 0}">${esc(String(v))}</span></div>`).join("");
  fillDetailWhitelist(r.mac, false);
  renderDetailMap(okLL(r.lat, r.lon) ? [{ lat:r.lat, lon:r.lon, tier:tn, rssi:null, t:0 }] : [],
                  "The server has no position for this device.");
  $("detDlg").showModal();
}

function openDialog(){
  const dlg = $("srvDlg"); if(!dlg) return;
  $("srvPairHint").hidden = isPaired();
  dlg.showModal();
  if(rows.length) renderMapView();   // Leaflet needs a resize once the dialog is visible again
}

export function initServerFind(){
  state.srvOpenDevice = openDevice;   // the Map page's server layer (servermap.js) opens its markers through this
  const btn = $("srvFindBtn"); if(!btn) return;
  btn.onclick = openDialog;
  $("srvClose").onclick = () => $("srvDlg").close();
  $("srvDlg").addEventListener("click", e => { if(e.target === $("srvDlg")) $("srvDlg").close(); });
  $("srvNearBtn").onclick = findNear;
  $("srvSearchBtn").onclick = search;
  $("srvQ").addEventListener("keydown", e => { if(e.key === "Enter"){ e.preventDefault(); search(); } });
  const sel = $("srvCat");
  sel.innerHTML = `<option value="">Any category</option>` +
    CAT_KEYS.slice(1).map((k, i) => `<option value="${k}">${CAT_LABEL[i + 1]}</option>`).join("") + `<option value="other">Other</option>`;
}
