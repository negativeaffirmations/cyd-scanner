// Detection detail modal (info + GPS mini-map). Used by the Explore Scan Viewer rows.
import { $, esc } from '../util/dom.js';
import { tierNum, hasGps, fmtEpoch } from '../util/format.js';
import { TIER_COL } from '../config.js';
import { newMap } from './map.js';
import { fillDetailWhitelist } from './whitelist.js';

let detMap=null, detLayer=null;
const SRC_FRIENDLY={"2.4":"Wi-Fi 2.4 GHz","5G":"Wi-Fi 5 GHz","BLE":"BLE","PRB":"Probe request","154":"802.15.4 (Zigbee/Thread)"};

// Shared detDlg mini-map renderer — OWNS the single Leaflet instance on #detMap, so both the Explore
// detail (one point) and the live detail (a sighting trail) reuse it (two Leaflet maps on one div
// would throw). `points` = [{lat,lon,tier,rssi,t}] oldest-first; pass [] to hide the map and show `msg`.
export function renderDetailMap(points, msg){
  const pts = (points||[]).filter(p=>isFinite(p.lat)&&isFinite(p.lon)&&!(p.lat===0&&p.lon===0));
  const has = pts.length > 0;
  $("detMap").style.display = has ? "" : "none";
  $("detMapMsg").style.display = has ? "none" : "";
  $("detMapMsg").textContent = msg || "No GPS fix for this detection.";
  if(!has) return;
  if(!detMap){ detMap=newMap("detMap"); detLayer=L.layerGroup().addTo(detMap); }
  setTimeout(()=>{
    detMap.invalidateSize(); detLayer.clearLayers();
    const latlngs = pts.map(p=>[p.lat,p.lon]);
    if(pts.length>1)
      L.polyline(latlngs,{color:"#8b949e",weight:2,opacity:.6,dashArray:"4 4"}).addTo(detLayer);  // trail
    pts.forEach((p,i)=>{
      const last = i===pts.length-1, col = TIER_COL[p.tier||0];
      const pop = (last?"Latest":"Seen")+(p.rssi!=null?" · "+p.rssi+" dBm":"")+
                  (p.t?"<br>"+new Date(p.t).toLocaleTimeString():"");
      L.circleMarker([p.lat,p.lon],{radius:last?9:5,color:col,weight:2,fillColor:col,
        fillOpacity:last?.8:.5}).addTo(detLayer).bindPopup(pop);
    });
    if(pts.length>1) detMap.fitBounds(L.latLngBounds(latlngs).pad(0.3));
    else detMap.setView(latlngs[0],17);
  }, 80);
}
export function openDetail(r){
  const tn=tierNum(r.tier);
  const tierTxt=(tn?r.tier.charAt(0).toUpperCase()+r.tier.slice(1):"None")+(r.score?" (score "+r.score+")":"")+
    (r.signature?" — "+r.signature:"");
  const rowsKv=[["Name",r.name||r.mac||"<hidden>"],["MAC",r.mac],["Source",SRC_FRIENDLY[r.source]||r.source||"?"],
    ["RSSI",r.rssi+" dBm"],["Channel",r.channel||"–"],["Threat tier",tierTxt]];
  if(r.wl) rowsKv.push(["Whitelisted","yes (muted)"]);
  if(r.flags) rowsKv.push(["Flags",r.flags.replace(/\+/g,", ")]);
  if(r.pan) rowsKv.push(["PAN ID",r.pan]);
  if(r.ie && !/^0+$/.test(r.ie)) rowsKv.push(["IE fingerprint",r.ie]);
  if(r.cid && !/^0+$/.test(r.cid)) rowsKv.push(["BLE company ID",r.cid]);
  if(r.uuid && !/^0+$/.test(r.uuid)) rowsKv.push(["Service UUID",r.uuid]);
  rowsKv.push(["Time", r.epoch>0 ? fmtEpoch(r.epoch) : "unsynced (+"+Math.round(r.ms/1000)+"s since boot)"]);
  rowsKv.push(["GPS", hasGps(r) ? r.lat.toFixed(6)+", "+r.lon.toFixed(6) : "none"]);
  $("detInfo").innerHTML=rowsKv.map(([k,v])=>`<div class="kv"><span class="k">${k}</span><span class="vv t${k==="Threat tier"?tn:0}">${esc(String(v))}</span></div>`).join("");
  fillDetailWhitelist(r.mac, !!r.wl);
  renderDetailMap(hasGps(r) ? [{lat:r.lat,lon:r.lon,tier:tn,rssi:r.rssi,t:r.epoch>0?r.epoch*1000:0}] : [],
                  "No GPS fix for this detection.");
  $("detDlg").showModal();
}
