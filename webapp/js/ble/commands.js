// Simple BLE CMD writes (one characteristic write each). The device echoes authoritative
// state back via STATUS, so these are fire-and-log.
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, log } from '../util/dom.js';

export function updateScanBtn(){
  const b = $("scanBtn");
  // Rec (●) when idle -> tap to start; Stop (■) while scanning. Icon-only button.
  b.innerHTML = '<span class="material-symbols-outlined">' + (state.scanning ? "stop" : "fiber_manual_record") + '</span>';
  b.title = state.scanning ? "Stop scan" : "Start scan";
  b.classList.toggle("scanning", state.scanning);
  b.classList.toggle("rec", !state.scanning);
}
export async function toggleScan(){
  const want = !state.scanning;                 // authoritative state comes back via status
  try { await state.chars.cmd.writeValue(enc.encode(want ? "G" : "X"));
    log(want ? "Start scan requested." : "Stop scan requested."); }
  catch(e){ log("Scan command failed: " + e.message); }
}
export async function newSession(){
  try { await state.chars.cmd.writeValue(enc.encode("N")); log("New session started."); }
  catch(e){ log("New session failed: " + e.message); }
}

export async function toggleWifi(){
  state.wifiOn = !state.wifiOn;
  try{ await state.chars.cmd.writeValue(enc.encode(state.wifiOn?"1":"0"));
    $("wifiBtn").textContent = state.wifiOn ? "Stop Wi-Fi download" : "Wi-Fi download mode";
    log(state.wifiOn ? "Wi-Fi download on — scan the QR on the device to join, then again to open."
              : "Wi-Fi download off."); }
  catch(e){ state.wifiOn = !state.wifiOn; log("Wi-Fi cmd failed: "+e.message); }
}

export async function reloadDb(){
  try{ await state.chars.cmd.writeValue(enc.encode("R")); log("Requested signature-DB reload."); }
  catch(e){ log("Reload failed: "+e.message); }
}

export async function syncTime(){
  try{ // send UTC epoch + this phone's timezone offset; the device localizes.
    const utc = Math.floor(Date.now()/1000);
    const off = new Date().getTimezoneOffset();  // minutes, UTC-local
    await state.chars.time.writeValue(enc.encode(utc + ";" + off));
    log("Time synced (UTC "+utc+", tz "+(-off/60)+"h)."); }
  catch(e){ log("Time sync failed: "+e.message); }
}

export async function setBrightness(v){
  try{ await state.chars.cmd.writeValue(enc.encode("B:"+v)); }
  catch(e){ log("Brightness failed: "+e.message); }
}

// Lock / unlock the CYD touchscreen (K1/K0). Authoritative lock state comes back via STATUS lock=.
export async function setLock(lock){
  try{ await state.chars.cmd.writeValue(enc.encode(lock ? "K1" : "K0"));
    log(lock ? "Lock display requested." : "Unlock display requested."); }
  catch(e){ log("Lock command failed: "+e.message); }
}

// Time / GPS source preference: "P:<t><g>", each 0 prefer phone / 1 prefer device / 2 device only.
// Authoritative values come back via STATUS tpref= / gpref=.
export async function sendSourcePref(){
  try{ await state.chars.cmd.writeValue(enc.encode("P:" + $("tPrefSel").value + $("gPrefSel").value)); }
  catch(e){ log("Source setting failed: "+e.message); }
}

// Scan-source mask: BLE=2, Wi-Fi 2.4G = WIFI24(1)+PROBE(8), Wi-Fi 5G=16, 802.15.4=4.
export async function sendSrcMask(){
  const m = ($("sBle").checked?2:0) | ($("s24").checked?9:0) | ($("s5").checked?16:0) | ($("s154").checked?4:0);
  try{ await state.chars.cmd.writeValue(enc.encode("S:"+m)); }
  catch(e){ log("Scan settings failed: "+e.message); }
}
