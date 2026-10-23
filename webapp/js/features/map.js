// Wardriving map (Leaflet). Owns the main map instance; also exports newMap() so the detail
// and follower mini-maps share one Leaflet setup. `L` is the global from the Leaflet CDN script.
import { state } from '../state.js';
import { enc, TIER_COL, CAT_COL } from '../config.js';
import { $, log, esc } from '../util/dom.js';
import { tierNum, hasGps, fmtEpoch } from '../util/format.js';
import { parseScanLog, parseScanEvents } from '../data/parse.js';
import { showPage } from '../nav.js';

let lmap=null, mapLayer=null, mapDets=[], mapEvents=[], mapRidEvents=[], mapByMac=new Map();

// Look up a parsed detection row by MAC (lowercased) — used by the evil-twin event popup to open
// the full device detail for a clicked BSSID. Returns undefined if that MAC isn't in the dataset.
export function detByMac(mac){ return mapByMac.get(String(mac||"").toLowerCase()); }

export function newMap(id){   // shared Leaflet setup (wardriving map + detail mini-map)
  const m=L.map(id);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
    {maxZoom:19, attribution:"&copy; OpenStreetMap", className:"basemap"}).addTo(m);   // .basemap = grayscale tiles only (see _map.scss)
  m.setView([0,0],2);
  return m;
}
function ensureMap(){
  if(lmap) return;
  lmap=newMap("map");
  mapLayer=L.layerGroup().addTo(lmap);
}
export function openMap(){
  showPage('map');                 // the Map is its own nav page now (not a dialog)
  ensureMap();
  setTimeout(()=>{ lmap.invalidateSize(); renderMap(); }, 80);   // the div is now visible + sized
  if(!mapDets.length && state.connected) mapCurrent();   // default view = the current scan
}

const EVT_STYLE={deauth:{col:"#f85149",ch:"D",name:"Deauth flood"}, eviltwin:{col:"#d29922",ch:"E",name:"Evil twin"},
  droneid:{col:"#58a6ff",ch:"R",name:"Drone Remote ID"}, follow:{col:"#ff5cbf",ch:"F",name:"Follower"}};

// Plot a scanlog (.jsonl or legacy .csv)'s geolocated rows on the wardriving map.
export function plotCsvText(text, label){
  const all=parseScanLog(text);
  mapByMac=new Map();                       // MAC -> row, for the evil-twin popup's clickable BSSIDs
  for(const d of all) if(d.mac) mapByMac.set(d.mac.toLowerCase(), d);
  const evs=parseScanEvents(text);
  mapEvents=evs.filter(e=>e.evt!=="rid" && hasGps(e));
  mapRidEvents=evs.filter(e=>e.evt==="rid");   // positions come from the drone/operator fix, not the phone's
  if(!all.length && !mapEvents.length && !mapRidEvents.length){ $("mapMsg").textContent="No scanlog rows in "+(label||"file")+"."; return; }
  mapDets=all.filter(hasGps);
  openMap();   // switch to the Map page, ensure Leaflet is sized, and render
  const pts=new Set(mapDets.map(d=>d.lat.toFixed(5)+","+d.lon.toFixed(5))).size;
  $("mapMsg").textContent=(label?label+": ":"")+mapDets.length+" geolocated detections at "+pts+" points"+
    (mapEvents.length?", "+mapEvents.length+" event pin(s).":".");
}

// Draw one Remote-ID emitter's drone + operator markers (+ link line when both known) on mapLayer;
// unknown/NaN coordinates are skipped. Pushes plotted points onto `pts` for fitBounds.
function plotRid(r,pts){
  const ok=(a,b)=>a!=null&&b!=null&&isFinite(a)&&isFinite(b)&&!(a===0&&b===0);
  const d=ok(r.dlat,r.dlon)?[r.dlat,r.dlon]:null, o=ok(r.olat,r.olon)?[r.olat,r.olon]:null;
  const col=CAT_COL[9], id=esc(r.uas||r.mac||"");
  if(d){ L.circleMarker(d,{radius:8,color:col,weight:2,fillColor:col,fillOpacity:.8}).addTo(mapLayer)
      .bindPopup("<b>Drone</b>"+(id?"<br>"+id:"")+"<br>"+d[0].toFixed(5)+", "+d[1].toFixed(5)); pts.push(d); }
  if(o){ L.circleMarker(o,{radius:6,color:"#ffffff",weight:3,fillColor:col,fillOpacity:.5}).addTo(mapLayer)
      .bindPopup("<b>Operator</b>"+(id?"<br>"+id:"")+"<br>"+o[0].toFixed(5)+", "+o[1].toFixed(5)); pts.push(o); }
  if(d&&o) L.polyline([d,o],{color:col,weight:2,opacity:.8}).addTo(mapLayer);
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
    let html="<b>"+esc(st.name)+"</b><br>"+(e.evt==="follow" ? "score "+e.score : "count "+e.count)+"<br>"+
      (e.epoch>0?fmtEpoch(e.epoch):"+"+Math.round(e.ms/1000)+"s")+"<br>"+e.lat.toFixed(5)+", "+e.lon.toFixed(5);
    const hasMacs = e.evt==="eviltwin" && e.macs && e.macs.length;
    if(e.evt==="eviltwin"){   // name the suspect SSID + its BSSIDs; each BSSID opens the device detail
      if(e.ssid) html+="<br><b>SSID:</b> "+esc(e.ssid);
      if(hasMacs) html+="<br><b>BSSIDs:</b> "+
        e.macs.map(x=>`<span class="evmac" data-mac="${esc(x)}">${esc(x)}</span>`).join(", ")+
        '<br><span class="evhint">tap an address for details</span>';
    }
    if(e.evt==="follow"){   // who was following: name / MAC / source / detector flags (flags tell a Find-My tag from a car)
      if(e.name) html+="<br><b>"+esc(e.name)+"</b>";
      html+="<br>"+esc(e.mac)+(e.src?" · "+esc(e.src):"")+(e.flags?"<br>"+esc(e.flags):"");
    }
    m.bindPopup(html,{maxWidth:280});
    if(hasMacs) m.on('popupopen', ev=>{   // wire clicks directly on the popup DOM (Leaflet stops click
      ev.popup.getElement().querySelectorAll('.evmac').forEach(el=>{   // propagation, so delegation won't reach)
        el.addEventListener('click', ()=>{
          const row=detByMac(el.dataset.mac)||{mac:el.dataset.mac, name:e.ssid||"", source:"2.4",
            rssi:0, tier:"likely", flags:"eviltwin"};   // minimal fallback if the AP wasn't logged as a row
          import('./detail.js').then(d=>d.openDetail(row));   // deferred import: avoids a static map<->detail cycle
        });
      });
    });
    m.addTo(mapLayer); pts.push([e.lat,e.lon]);
  }
  if($("fEvents").checked){   // Remote-ID positions: logged rid events + live emitters (drone, operator, link)
    const ridSrc=mapRidEvents.slice();
    for(const r of Object.values(state.rids||{})) ridSrc.push(r);
    for(const r of ridSrc) plotRid(r,pts);
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
