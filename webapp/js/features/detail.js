// Detection detail modal (info + GPS mini-map). Used by the Explore Scan Viewer rows.
import { $, esc } from '../util/dom.js';
import { tierNum, hasGps, fmtEpoch } from '../util/format.js';
import { TIER_COL } from '../config.js';
import { newMap } from './map.js';

let detMap=null, detLayer=null;
const SRC_FRIENDLY={"2.4":"Wi-Fi 2.4 GHz","5G":"Wi-Fi 5 GHz","BLE":"BLE","PRB":"Probe request","154":"802.15.4 (Zigbee/Thread)"};
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
  const gps=hasGps(r);
  $("detMap").style.display=gps?"":"none";
  $("detMapMsg").style.display=gps?"none":"";
  $("detMapMsg").textContent="No GPS fix for this detection.";
  $("detDlg").showModal();
  if(!gps) return;
  if(!detMap){ detMap=newMap("detMap"); detLayer=L.layerGroup().addTo(detMap); }
  setTimeout(()=>{
    detMap.invalidateSize(); detLayer.clearLayers();
    const col=TIER_COL[tn];
    L.circleMarker([r.lat,r.lon],{radius:9,color:col,weight:2,fillColor:col,fillOpacity:.7}).addTo(detLayer);
    detMap.setView([r.lat,r.lon],17);
  }, 80);
}
