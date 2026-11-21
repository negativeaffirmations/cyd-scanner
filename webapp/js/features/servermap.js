// Map page: a "Server devices" layer on the main wardriving map, from the paired scan-analyzer server.
//
// With Map Filters -> "Server devices" ticked, the visible part of the map is asked for with
// GET /api/v1/devices/near?lat&lon&radius_m (the view's centre, radius = centre to corner, capped at the server's 5 km)
// each time the map stops moving. Markers are hollow rings (session dots are filled) in the tier colour, with the
// estimate's radius drawn when the server has one; a tap opens the normal device popup (serverfind.js's
// openDevice, reached through state.srvOpenDevice, so serverhist.js then loads the full server history).
// "Threats only" (the same Map filter) keeps devices the server rates above "none".
//
// NEW file (stale-module rule): only main.js imports it. The main map comes from state.mainMap (set inside map.js,
// which creates it on the first visit to the Map page), so nothing is imported from older modules but plain helpers.
import { state } from '../state.js';
import { $, esc, toast } from '../util/dom.js';
import { TIER_COL } from '../config.js';
import { isPaired, serverUrl } from './remote.js';

const TIMEOUT_MS = 15000;
const MAX_RADIUS = 5000;     // the server's /devices/near limit
const LS_KEY = "cflo.mapServer";
const TIER_IDX = { none:0, suspect:1, likely:2, confirmed:3 };

let layer = null, hooked = null, seq = 0, timer = 0, wideSaid = false;

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

const okLL = (a, b) => a != null && b != null && isFinite(a) && isFinite(b) && !(a === 0 && b === 0);
function label(r){
  const n = (r.name || "").trim();
  if(n && n.toUpperCase() !== (r.mac || "").toUpperCase()) return n;
  return r.vendor ? r.vendor + " " + (r.mac || "").slice(12) : (r.mac || "?");
}
const on = () => { const c = $("fServer"); return !!(c && c.checked); };

function clear(){ if(layer) layer.clearLayers(); }

async function refresh(){
  const m = state.mainMap;
  if(!m || !on()) { clear(); return; }
  if(!isPaired()){ toast("Server devices: pair with your server first (Settings → Remote Server).", {err:true}); return; }
  const c = m.getCenter(), b = m.getBounds();
  let radius = Math.round(m.distance(c, b.getNorthEast()));
  if(radius > MAX_RADIUS){
    radius = MAX_RADIUS;
    if(!wideSaid){ wideSaid = true; toast("Server devices: showing the 5 km around the map's centre. Zoom in to see the rest."); }
  }
  if(radius < 100) radius = 100;
  const threats = !!($("fThreats") && $("fThreats").checked);
  const s = ++seq;
  const r = await getJson("/api/v1/devices/near?lat=" + c.lat.toFixed(6) + "&lon=" + c.lng.toFixed(6) +
                          "&radius_m=" + radius + "&limit=" + (threats ? 1000 : 300));
  if(s !== seq || !on()) return;   // the map moved again, or the layer was turned off
  if(r.status !== 200 || !Array.isArray(r.body)){
    const why = r.status === 403 ? "this phone's key may not read (turn on read on the server's admin page)"
              : r.status === 401 ? "the server didn't accept this phone's key (pair again)"
              : "couldn't reach the server (" + (r.err || "HTTP " + r.status) + ")";
    toast("Server devices: " + why + ".", {err:true});
    return;
  }
  let list = r.body.filter(d => okLL(d.lat, d.lon));
  if(threats) list = list.filter(d => (TIER_IDX[d.tier] || 0) > 0);
  if(!layer) layer = L.layerGroup().addTo(m);
  layer.clearLayers();
  list.slice().reverse().forEach(d => {   // farthest first, so the nearest draws on top
    const col = TIER_COL[TIER_IDX[d.tier] || 0], p = [d.lat, d.lon];
    if(d.position === "estimate" && d.radius_m > 0)
      L.circle(p, { radius:d.radius_m, color:col, weight:1, dashArray:"3 4", fill:false, interactive:false }).addTo(layer);
    const mk = L.circleMarker(p, { radius:7, color:col, weight:3, fillColor:"#000000", fillOpacity:.15 }).addTo(layer);
    mk.bindTooltip(esc(label(d)) + " <span style='opacity:.7'>(server)</span>");
    mk.on("click", () => { if(state.srvOpenDevice) state.srvOpenDevice(Object.assign({}, d, { distance_m:null })); });   // no "Distance" row: it is from the map centre
  });
}

function schedule(){ clearTimeout(timer); timer = setTimeout(refresh, 700); }

// The main map is created on the first Map-page visit; hook it once it exists.
function hook(){
  const m = state.mainMap;
  if(!m || hooked === m) return;
  hooked = m; layer = null;
  m.on("moveend", () => { if(on()) schedule(); });
  if(on()) schedule();
}

export function initServerMap(){
  const cb = $("fServer"); if(!cb) return;
  try{ cb.checked = localStorage.getItem(LS_KEY) === "1"; }catch(e){}
  cb.addEventListener("change", () => {
    try{ localStorage.setItem(LS_KEY, cb.checked ? "1" : "0"); }catch(e){}
    if(cb.checked){ wideSaid = false; schedule(); } else { seq++; clear(); }
  });
  const th = $("fThreats"); if(th) th.addEventListener("change", () => { if(on()) schedule(); });
  setInterval(hook, 1000);
}
