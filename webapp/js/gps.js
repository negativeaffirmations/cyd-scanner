// Phone geolocation -> device GPS characteristic.
import { state } from './state.js';
import { enc } from './config.js';
import { log } from './util/dom.js';

// The position the SCANNER is stamping on its log right now: STATUS pos=lat,lon + pfa=<fix_age> (fw >=
// 0.16; same source order as the file log: fresh phone, else onboard GPS, else held). Returns
// {lat, lon, fixAge} with fixAge = seconds when the log would mark it held/aged (undefined = live), or
// null with no fix. Older firmware doesn't send pos: fall back to the phone's own fix, only while < 10 s
// old, so a frozen phone fix (screen off) is never stamped as current.
export function scannerFix(){
  const s = state.status;
  if ("pos" in s){
    const [la, lo] = (s.pos || "").split(",").map(Number);
    if (!s.pos || !isFinite(la) || !isFinite(lo)) return null;
    const pfa = +s.pfa;
    return { lat: la, lon: lo, fixAge: pfa >= 0 ? pfa : undefined };
  }
  const g = state.lastGps;
  if (!g || !g.t || (!g.lat && !g.lon) || Date.now() - g.t > 10000) return null;
  return { lat: g.lat, lon: g.lon, fixAge: undefined };
}

export async function sendGps(pos, quiet=false){   // quiet: the ~5 s continuous feed doesn't log each send
  try{ const s=pos.coords.latitude.toFixed(6)+","+pos.coords.longitude.toFixed(6);
    await state.chars.gps.writeValue(enc.encode(s)); if(!quiet) log("GPS sent "+s); return true; }
  catch(e){ log("GPS send failed: "+e.message); return false; }
}

// Continuous GPS: push a fix at most every ~5 s, moving or not. The scanner treats a phone fix older
// than 10 s as stale (fw >= 0.14) and switches to its onboard GPS, so a sparser cadence made the source
// flap blue/purple while standing still. Only real watchPosition updates are sent, and never one the
// phone itself reports as > 10 s old, so the scanner can't be handed an old position as fresh.
let gpsBusy = false, staleLogged = false, lastTry = 0;
export async function onGpsWatch(pos){
  const now=Date.now(), la=pos.coords.latitude, lo=pos.coords.longitude;
  if(gpsBusy || state.gpsHold || now-lastTry < 5000) return;
  if(pos.timestamp && now-pos.timestamp > 10000){   // a cached / stale fix: don't present it as current
    // Logged once: a phone clock skewed > 10 s from its fix times would otherwise drop EVERY update silently.
    if(!staleLogged){ staleLogged=true; log("GPS: skipping a fix "+Math.round((now-pos.timestamp)/1000)+" s old"); }
    return;
  }
  gpsBusy=true; lastTry=now;
  // lastGps only advances once the scanner actually has the fix (older fw's scannerFix() falls back to it).
  try{ if(await sendGps(pos, true)) state.lastGps={t:now,lat:la,lon:lo}; } finally{ gpsBusy=false; }
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
