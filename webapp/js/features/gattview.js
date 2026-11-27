// Device popup: "Identified" section, from the scanner's GATT identification read of this device (fw >= 0.47.0,
// docs/gatt-identify.md; data/devinfo.js holds what the live copy and loaded logs carried). Shows the read name, maker,
// model, firmware / hardware revision and appearance. Like phoneview.js it hooks nothing in older modules: it watches
// #detInfo (rewritten on every popup open) and reads the MAC from its rows. NEW file (stale-module rule).
import { $, esc } from '../util/dom.js';
import { devinfoFor, onDevinfo } from '../data/devinfo.js';

// Bluetooth SIG appearance categories (appearance >> 6), the common ones.
const APPEAR = ["Unknown", "Phone", "Computer", "Watch", "Clock", "Display", "Remote control", "Eyeglasses", "Tag",
  "Keyring", "Media player", "Barcode scanner", "Thermometer", "Heart rate sensor", "Blood pressure", "HID (keyboard / mouse)",
  "Glucose meter", "Running sensor", "Cycling sensor", "Control device", "Network device", "Sensor", "Light fixture", "Fan",
  "HVAC", "Air conditioning", "Humidifier", "Heating", "Access control", "Motorized device", "Power device", "Light source",
  "Window covering", "Audio sink", "Audio source", "Motorized vehicle", "Domestic appliance", "Wearable audio", "Aircraft",
  "AV equipment", "Display equipment", "Hearing aid", "Gaming", "Signage"];

let curMac = "";

function popupMac(){
  for(const row of document.querySelectorAll("#detInfo .kv")){
    const k = row.querySelector(".k"), v = row.querySelector(".vv");
    if(k && v && k.textContent.trim() === "MAC") return v.textContent.trim().toUpperCase();
  }
  return "";
}
const kv = (k, v)=>`<div class="kv"><span class="k">${esc(k)}</span><span class="vv">${esc(v)}</span></div>`;

function box(){
  let el = $("detGatt");
  const info = $("detInfo");
  if(!el && info){ el = document.createElement("div"); el.id = "detGatt"; info.after(el); }
  return el;
}

function render(){
  const el = box();
  if(!el) return;
  const r = curMac ? devinfoFor(curMac) : null;
  if(!r){ el.innerHTML = ""; return; }
  let h = `<div class="srvhead"><span class="material-symbols-outlined">badge</span><strong>Identified</strong>` +
          `<span class="sub">&nbsp;read from the device</span></div>`;
  if(r.name) h += kv("Device name", r.name);
  else if(r.nw) h += kv("Device name", "not kept (looks personal)");
  if(r.mfr) h += kv("Maker", r.mfr);
  if(r.model) h += kv("Model", r.model);
  if(r.fw) h += kv("Firmware", r.fw);
  if(r.hw) h += kv("Hardware", r.hw);
  if(typeof r.app === "number" && r.app){
    const c = APPEAR[r.app >> 6];
    h += kv("Appearance", (c || "Category " + (r.app >> 6)) + " (0x" + r.app.toString(16).toUpperCase().padStart(4, "0") + ")");
  }
  if(r.auth) h += `<div class="sub">Some fields need pairing to read; the scanner never pairs.</div>`;
  el.innerHTML = h;
}

export function initGattView(){
  const info = $("detInfo");
  if(info) new MutationObserver(()=>{ curMac = popupMac(); render(); }).observe(info, { childList:true });
  const dlg = $("detDlg");
  if(dlg) dlg.addEventListener("close", ()=>{ curMac = ""; });
  onDevinfo(()=>{ const d = $("detDlg"); if(curMac && d && d.open) render(); });
}
