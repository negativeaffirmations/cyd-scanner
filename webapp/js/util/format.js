// Pure formatting / geometry helpers (no DOM, no state).
import { FLAG_BADGES } from '../config.js';

export function tierNum(t){ return t==="confirmed"?3 : t==="likely"?2 : t==="suspect"?1 : 0; }

export const hasGps = r => isFinite(r.lat) && isFinite(r.lon) && !(r.lat===0 && r.lon===0);

// equirectangular distance, metres
export function gpsMovedM(a,b,c,d){
  const k=111320, dx=(d-b)*k*Math.cos(a*Math.PI/180), dy=(c-a)*k; return Math.hypot(dx,dy);
}

// epoch is LOCAL time encoded as seconds, so read it with UTC getters (no tz shift).
export function fmtEpoch(e){ return new Date(e*1000).toISOString().slice(0,19).replace("T"," "); }
export function fmtTimecode(r){
  return r.epoch>0 ? fmtEpoch(r.epoch).slice(11) : "+"+Math.round(r.ms/1000)+"s";
}

export function flagBadges(bits){
  return FLAG_BADGES.filter(f=>bits&f[0]).map(f=>`<span class="flagb" title="${f[2]}">${f[1]}</span>`).join("");
}
