// Wardriving map (Leaflet). Owns the main map instance; also exports newMap() so the detail
// and follower mini-maps share one Leaflet setup. `L` is the global from the Leaflet CDN script.
import { state } from '../state.js';
import { enc, TIER_COL } from '../config.js';
import { $, log, esc } from '../util/dom.js';
import { tierNum, hasGps, fmtEpoch } from '../util/format.js';
import { parseScanLog, parseScanEvents } from '../data/parse.js';

let lmap=null, mapLayer=null, mapDets=[], mapEvents=[];

export function newMap(id){   // shared Leaflet setup (wardriving map + detail mini-map)
  const m=L.map(id);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
    {maxZoom:19, attribution:"&copy; OpenStreetMap"}).addTo(m);
  m.setView([0,0],2);
  return m;
}
function ensureMap(){
  if(lmap) return;
  lmap=newMap("map");
  mapLayer=L.layerGroup().addTo(lmap);
}
export function openMap(){
  $("mapDlg").showModal();
  ensureMap();
  setTimeout(()=>{ lmap.invalidateSize(); renderMap(); }, 80);
  if(!mapDets.length && state.connected) mapCurrent();   // default view = the current scan
}

const EVT_STYLE={deauth:{col:"#f85149",ch:"D",name:"Deauth flood"}, eviltwin:{col:"#d29922",ch:"E",name:"Evil twin"},
  droneid:{col:"#58a6ff",ch:"R",name:"Drone Remote ID"}};

// Plot a scanlog (.jsonl or legacy .csv)'s geolocated rows on the wardriving map.
export function plotCsvText(text, label){
  const all=parseScanLog(text);
  mapEvents=parseScanEvents(text).filter(hasGps);
  if(!all.length && !mapEvents.length){ $("mapMsg").textContent="No scanlog rows in "+(label||"file")+"."; return; }
  mapDets=all.filter(hasGps);
  if(!$("mapDlg").open) openMap(); else renderMap();
  const pts=new Set(mapDets.map(d=>d.lat.toFixed(5)+","+d.lon.toFixed(5))).size;
  $("mapMsg").textContent=(label?label+": ":"")+mapDets.length+" geolocated detections at "+pts+" points"+
    (mapEvents.length?", "+mapEvents.length+" event pin(s).":".");
}

// Group filtered detections by GPS fix and draw one marker per location.
export function renderMap(){
  if(!lmap) return;
  mapLayer.clearLayers();
  const on={"2.4":$("f24").checked,"5G":$("f5").checked,"BLE":$("fble").checked,"PRB":$("fprb").checked,"154":$("f154").checked};
  const threatsOnly=$("fThreats").checked;
  const groups=new Map();
  for(const d of mapDets){
    if(!on[d.source]) continue;
    if(threatsOnly && tierNum(d.tier)===0) continue;
    const k=d.lat.toFixed(5)+","+d.lon.toFixed(5);
    let g=groups.get(k); if(!g){ g={lat:d.lat,lon:d.lon,items:[]}; groups.set(k,g); }
    g.items.push(d);
  }
  const pts=[];
  groups.forEach(g=>{
    let worst=0; for(const d of g.items) worst=Math.max(worst,tierNum(d.tier));
    const col=TIER_COL[worst];
    const m=L.circleMarker([g.lat,g.lon],{radius:Math.min(5+Math.sqrt(g.items.length),16),
      color:col, weight:1, fillColor:col, fillOpacity:worst?0.85:0.55});
    g.items.sort((a,b)=> tierNum(b.tier)-tierNum(a.tier) || b.rssi-a.rssi);
    const shown=g.items.slice(0,60).map(d=>{
      const flag=tierNum(d.tier)?" ["+d.tier+"]":"";
      return (d.source||"?")+" "+d.rssi+"dBm "+(d.name?d.name+" ":"")+d.mac+(d.pan?" PAN "+d.pan:"")+flag;
    }).join("\n");
    const more=g.items.length>60?"\n… +"+(g.items.length-60)+" more":"";
    m.bindPopup("<b>"+g.items.length+" device(s)</b><br>"+g.lat.toFixed(5)+", "+g.lon.toFixed(5)+
      '<div class="plist">'+esc(shown+more)+"</div>",{maxWidth:320});
    m.addTo(mapLayer); pts.push([g.lat,g.lon]);
  });
  if($("fEvents").checked) for(const e of mapEvents){   // event pins (deauth / eviltwin / droneid)
    const st=EVT_STYLE[e.evt]||{col:"#8b949e",ch:"!",name:e.evt};
    const m=L.marker([e.lat,e.lon],{icon:L.divIcon({className:"",iconSize:[18,18],
      html:`<div class="evpin" style="background:${st.col}">${st.ch}</div>`})});
    m.bindPopup("<b>"+esc(st.name)+"</b><br>count "+e.count+"<br>"+(e.epoch>0?fmtEpoch(e.epoch):"+"+Math.round(e.ms/1000)+"s")+
      "<br>"+e.lat.toFixed(5)+", "+e.lon.toFixed(5));
    m.addTo(mapLayer); pts.push([e.lat,e.lon]);
  }
  if(pts.length) lmap.fitBounds(pts,{padding:[30,30]});
}

export async function mapCurrent(){
  if(!state.connected){ log("Connect first to map the current session."); return; }
  try{ state.dl={mode:null,expect:0,got:0,chunks:[],name:"current session",sink:"map"};
    $("mapMsg").textContent="Loading current session…";
    await state.chars.cmd.writeValue(enc.encode("L")); }
  catch(e){ log("Map current failed: "+e.message); }
}
