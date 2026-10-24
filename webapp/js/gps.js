// Phone geolocation -> device GPS characteristic.
import { state } from './state.js';
import { enc } from './config.js';
import { log } from './util/dom.js';

export async function sendGps(pos, quiet=false){   // quiet: the ~5 s continuous feed doesn't log each send
  try{ const s=pos.coords.latitude.toFixed(6)+","+pos.coords.longitude.toFixed(6);
    await state.chars.gps.writeValue(enc.encode(s)); if(!quiet) log("GPS sent "+s); }
  catch(e){ log("GPS send failed: "+e.message); }
}

// Continuous GPS: push a fix at most every ~5 s, moving or not. The scanner treats a phone fix older
// than 10 s as stale (fw >= 0.14) and switches to its onboard GPS, so a sparser cadence made the source
// flap blue/purple while standing still. Only real watchPosition updates are sent, and never one the
// phone itself reports as > 10 s old, so the scanner can't be handed an old position as fresh.
let gpsBusy = false;
export async function onGpsWatch(pos){
  const now=Date.now(), la=pos.coords.latitude, lo=pos.coords.longitude;
  if(gpsBusy || now-state.lastGps.t < 5000) return;
  if(pos.timestamp && now-pos.timestamp > 10000) return;   // a cached / stale fix: don't present it as current
  gpsBusy=true; state.lastGps={t:now,lat:la,lon:lo};
  try{ await sendGps(pos, true); } finally{ gpsBusy=false; }
}
export function startGps(){
  if(!navigator.geolocation){ log("No geolocation on this device."); return; }
  if(state.gpsWatch!==null) return;
  state.lastGps={t:0,lat:0,lon:0};
  state.gpsWatch = navigator.geolocation.watchPosition(onGpsWatch, e=>{
    log("GPS watch error: "+e.message+" - falling back to one-shot");
    if(state.gpsWatch!==null){ navigator.geolocation.clearWatch(state.gpsWatch); state.gpsWatch=null; }
    gpsOnce();
  }, {enableHighAccuracy:true, maximumAge:5000});
}
export function gpsOnce(){
  if(!navigator.geolocation){ log("No geolocation."); return; }
  navigator.geolocation.getCurrentPosition(sendGps, e=>log("GPS error: "+e.message),
    {enableHighAccuracy:true});
}
