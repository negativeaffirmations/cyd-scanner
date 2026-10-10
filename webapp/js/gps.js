// Phone geolocation -> device GPS characteristic.
import { state } from './state.js';
import { enc } from './config.js';
import { log } from './util/dom.js';
import { gpsMovedM } from './util/format.js';

export async function sendGps(pos){
  try{ const s=pos.coords.latitude.toFixed(6)+","+pos.coords.longitude.toFixed(6);
    await state.chars.gps.writeValue(enc.encode(s)); log("GPS sent "+s); }
  catch(e){ log("GPS send failed: "+e.message); }
}

// Continuous GPS for the device's "following me" detector: push a fix at most every ~5 s,
// and only if we moved >= ~10 m (or 30 s passed as a keep-alive). Same GPS characteristic.
let gpsBusy = false;
export async function onGpsWatch(pos){
  const now=Date.now(), la=pos.coords.latitude, lo=pos.coords.longitude;
  if(gpsBusy || now-state.lastGps.t < 5000) return;
  if(state.lastGps.t && now-state.lastGps.t < 30000 && gpsMovedM(state.lastGps.lat,state.lastGps.lon,la,lo) < 10) return;
  gpsBusy=true; state.lastGps={t:now,lat:la,lon:lo};
  try{ await sendGps(pos); } finally{ gpsBusy=false; }
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
