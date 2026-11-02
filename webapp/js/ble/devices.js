// Registry of the scanners (BluetoothDevice objects) this page knows about: every device picked this page
// session, plus the ones Chrome remembers across reloads when navigator.bluetooth.getDevices() exists.
// Entries also carry the live-presence fields the switcher fills in (features/switcher.js). The unit NAME of a
// scanner (STATUS name=) is remembered per device id in localStorage so the list is readable while the unit is away.
// NEW file; connection.js records the devices it connects to, the switcher reads them.
const NAMES_KEY = "cflo.unitNames";
const entries = new Map();   // device.id -> {id, device, lastSeen, rssi, watching, starting, ctl, onAdv}
const listeners = new Set();

function readNames(){ try{ return JSON.parse(localStorage.getItem(NAMES_KEY) || "{}") || {}; }catch(e){ return {}; } }
export function onDevicesChange(fn){ listeners.add(fn); }
function changed(){ listeners.forEach(fn=>{ try{ fn(); }catch(e){} }); }

export function rememberDevice(device){
  if(!device || !device.id) return null;
  let e = entries.get(device.id);
  if(!e){ e = { id:device.id, device, lastSeen:0, rssi:null, watching:false, starting:false, ctl:null, onAdv:null }; entries.set(device.id, e); changed(); }
  else e.device = device;   // a fresh object for the same scanner (the chooser hands one back)
  return e;
}
export const allDevices = ()=>[...entries.values()];
export const getEntry = id=>entries.get(id) || null;

export function unitNameFor(id){ return readNames()[id] || ""; }
export function setUnitName(id, name){
  if(!id || !name) return;
  const n = readNames(); if(n[id] === name) return;
  n[id] = name;
  try{ localStorage.setItem(NAMES_KEY, JSON.stringify(n)); }catch(e){}
  changed();
}

export const canGetDevices = ()=>!!(navigator.bluetooth && typeof navigator.bluetooth.getDevices === "function");

// Scanners Chrome remembers from earlier visits (needs the new Web Bluetooth permissions backend). Returns whether it ran.
export async function loadRemembered(){
  if(!canGetDevices()) return false;
  try{
    const ds = await navigator.bluetooth.getDevices();
    ds.forEach(rememberDevice);
    changed();
    return true;
  }catch(e){ return false; }
}
