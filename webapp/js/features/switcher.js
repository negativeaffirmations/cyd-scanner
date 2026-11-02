// "Switch scanner": jump between remembered scanners without reloading the page (and without the chooser).
// - Devices: ble/devices.js (picked this page session + navigator.bluetooth.getDevices() when Chrome has it).
// - Presence: BluetoothDevice.watchAdvertisements() on every remembered scanner that is NOT the connected one
//   (a connected peripheral doesn't advertise), only while the page is visible and no BLE file transfer runs.
//   A scanner is "in range" for PRESENT_MS after its last advertisement. The app-bar switch button is enabled
//   only when some OTHER remembered scanner is in range. Without watchAdvertisements the button stays enabled
//   and a tap tries gatt.connect() with a short timeout ("not found").
// - The connect itself is connection.js switchTo() (clean disconnect of the current unit first, then the normal
//   auth flow with the per-sid key), passed in through initSwitcher() so this file never imports connection.js
//   (that module is versioned with main.js; a second differently-keyed import would load it twice).
// NEW file, wired from main.js.
import { state } from '../state.js';
import { $, esc, toast, log } from '../util/dom.js';
import { anyBusy } from '../ble/xfer.js';
import { allDevices, getEntry, onDevicesChange, loadRemembered, unitNameFor, setUnitName, canGetDevices } from '../ble/devices.js';

const PRESENT_MS = 10000;     // "not seen" after this long without an advertisement
const TICK_MS = 2000;
let actions = null;           // {switchTo(device, timeoutMs), addScanner()} from connection.js
let switching = false;
let renderTimer = 0;

export const presenceSupported = ()=>typeof BluetoothDevice !== "undefined" && typeof BluetoothDevice.prototype.watchAdvertisements === "function";
const inRange = e=>!!e.lastSeen && Date.now() - e.lastSeen < PRESENT_MS;
const isCurrent = e=>!!(state.connected && state.device && state.device.id === e.id);
const labelOf = e=>(isCurrent(e) && state.scannerName) || unitNameFor(e.id) || e.device.name || "Scanner";
const others = ()=>allDevices().filter(e=>!isCurrent(e));

// --- browser identification (shown so the user can tell which flags to look at) ---
function browserText(){
  const d = navigator.userAgentData;
  if(d && d.brands && d.brands.length){
    const b = d.brands.filter(x=>!/not.?a.?brand/i.test(x.brand)).map(x=>x.brand + " " + x.version);
    if(b.length) return b.join(" / ") + (d.platform ? " (" + d.platform + ")" : "");
  }
  const m = /(Vivaldi|EdgA?|SamsungBrowser|OPR|Chromium|Chrome)\/([\d.]+)/.exec(navigator.userAgent || "");
  return m ? m[1] + " " + m[2] : (navigator.userAgent || "unknown browser").slice(0, 60);
}
let fullVersion = "";
try{
  if(navigator.userAgentData && navigator.userAgentData.getHighEntropyValues)
    navigator.userAgentData.getHighEntropyValues(["uaFullVersion"]).then(v=>{ fullVersion = v.uaFullVersion || ""; schedule(); }).catch(()=>{});
}catch(e){}

// --- presence watching ---
async function startWatch(e){
  if(e.watching || e.starting || !e.device.watchAdvertisements) return;
  e.starting = true;
  e.ctl = new AbortController();
  e.onAdv = ev=>{ e.lastSeen = Date.now(); e.rssi = typeof ev.rssi === "number" ? ev.rssi : null; schedule(); };
  e.device.addEventListener("advertisementreceived", e.onAdv);
  try{
    await e.device.watchAdvertisements({ signal:e.ctl.signal });
    if(e.ctl.signal.aborted){ e.watching = false; return; }   // stopWatch() ran while the request was pending
    e.watching = true;
  }
  catch(err){ e.device.removeEventListener("advertisementreceived", e.onAdv); }   // aborted, or the browser refused: try again next tick
  finally{ e.starting = false; }
}
function stopWatch(e){
  if(!e.watching && !e.starting) return;
  try{ if(e.ctl) e.ctl.abort(); }catch(err){}
  if(e.onAdv) e.device.removeEventListener("advertisementreceived", e.onAdv);
  e.watching = false;
}
function tick(){
  const want = document.visibilityState === "visible" && !anyBusy() && !switching && presenceSupported();
  for(const e of allDevices()){
    if(want && !isCurrent(e)) startWatch(e); else stopWatch(e);
  }
  render();
}

// --- UI ---
function schedule(){ if(!renderTimer) renderTimer = setTimeout(()=>{ renderTimer = 0; render(); }, 150); }
function canSwitch(){
  if(!navigator.bluetooth || switching) return false;
  return presenceSupported() ? others().some(inRange) : true;
}
function stateText(e){
  if(isCurrent(e)) return "Connected";
  if(presenceSupported()) return inRange(e) ? "In range" + (e.rssi != null ? " · " + e.rssi + " dBm" : "") : "Not seen";
  return "Presence unknown — tap to try";
}
function render(){
  const btn = $("switchBtn");
  if(btn){
    const ok = canSwitch();
    btn.classList.toggle("disabled", !ok); btn.classList.toggle("ready", ok && presenceSupported());
    btn.setAttribute("aria-disabled", ok ? "false" : "true");
    btn.title = ok ? "Switch scanner" : "Switch scanner (no other remembered scanner is in range)";
    btn.hidden = !navigator.bluetooth;
  }
  const dlg = $("switchDlg");
  if(!dlg || !dlg.open) return;
  const list = allDevices().sort((a, b)=>(isCurrent(b) - isCurrent(a)) || (inRange(b) - inRange(a)) || labelOf(a).localeCompare(labelOf(b)));
  $("switchList").innerHTML = list.length ? list.map(e=>{
    const cur = isCurrent(e), live = presenceSupported() ? inRange(e) : true;
    const col = cur ? "#3fb950" : presenceSupported() ? (live ? "#388bfd" : "#8b949e") : "#8b949e";
    return `<div class="sessrow keyrow"><span class="material-symbols-outlined" style="color:${col}">bluetooth</span>` +
      `<div class="sessmain"><span class="sessdate">${esc(labelOf(e))}</span><span class="sesssub">${esc(stateText(e))}</span></div>` +
      (cur ? "" : `<button class="secondary swgo" data-id="${esc(e.id)}"${live && !switching ? "" : " disabled"}>Switch</button>`) + `</div>`;
  }).join("") : '<div class="sub" style="padding:6px;margin:0">No remembered scanners yet. Use Add scanner to pick one.</div>';
  $("switchAddBtn").disabled = switching;
  const present = presenceSupported();
  let info = `Browser: ${browserText()}${fullVersion ? " (" + fullVersion + ")" : ""}<br>` +
    `Live presence (watchAdvertisements): <b>${present ? "supported" : "not supported"}</b> · ` +
    `Remembered across reloads (getDevices): <b>${canGetDevices() ? "supported" : "not supported"}</b>`;
  if(!present || !canGetDevices()){
    info += `<br><span class="switchhint">Suggestion, flag names may differ by version: open <code>chrome://flags</code> ` +
      `(<code>vivaldi://flags</code> in Vivaldi), enable <b>Experimental Web Platform features</b> ` +
      `(<code>#enable-experimental-web-platform-features</code>)` +
      (canGetDevices() ? "" : ` and <b>Use the new permissions backend for Web Bluetooth</b> (<code>#enable-web-bluetooth-new-permissions-backend</code>)`) +
      `, then relaunch the browser. Without them scanners are only remembered until the page reloads and Switch tries to connect blindly.</span>`;
  }
  $("switchInfo").innerHTML = info;
}

const BUSY_MSG = "Finish or cancel the transfer first, then switch scanners.";
async function go(id){
  const e = getEntry(id); if(!e || switching || !actions) return;
  if(anyBusy()){ toast(BUSY_MSG, { err:true }); return; }   // a switch mid-transfer would mix two scanners' data
  const name = labelOf(e);
  $("switchDlg").close();
  switching = true; tick();
  toast("Switching to " + name + "…", { work:true, sticky:true });
  let r;
  try{ r = await actions.switchTo(e.device, presenceSupported() ? 15000 : 8000); }
  catch(err){ r = { ok:false, why:"failed", msg:err.message }; }
  switching = false;
  if(r.ok){ toast("Connected to " + name + "."); log("Switched to " + name + "."); }
  else if(r.why === "not-found"){ toast(name + " not found — is it powered on and in range?", { err:true }); log("Switch: " + name + " not found."); }
  else if(r.why === "busy") toast("Another connection is in progress.", { err:true });
  else toast("Could not connect to " + name + " — see the Activity log.", { err:true });
  tick();
}

export function openSwitcher(){
  if(!navigator.bluetooth){ toast("Web Bluetooth isn't available in this browser.", { err:true }); return; }
  $("switchDlg").showModal();
  render();
}

export function initSwitcher(a){
  actions = a;
  $("switchBtn").addEventListener("click", ()=>{ if(anyBusy()) toast(BUSY_MSG, { err:true }); else if(canSwitch()) openSwitcher(); else toast(presenceSupported() ? "No other remembered scanner is in range." : "Web Bluetooth isn't available.", { err:true }); });
  $("switchBtn").addEventListener("keydown", ev=>{ if(ev.key === "Enter" || ev.key === " "){ ev.preventDefault(); $("switchBtn").click(); } });
  $("switchOpenBtn").onclick = openSwitcher;   // Settings: always reachable (also how a second scanner gets added)
  $("switchClose").onclick = ()=>$("switchDlg").close();
  $("switchDlg").addEventListener("click", ev=>{ if(ev.target === $("switchDlg")) $("switchDlg").close(); });
  $("switchList").addEventListener("click", ev=>{ const b = ev.target.closest(".swgo"); if(b && !b.disabled) go(b.dataset.id); });
  $("switchAddBtn").onclick = ()=>{
    if(state.connected && anyBusy()){ toast(BUSY_MSG, { err:true }); return; }
    $("switchDlg").close(); if(actions.addScanner) actions.addScanner();
  };   // the chooser needs this tap's user gesture
  // Remember the connected unit's name (STATUS name=, mirrored into #unitName) against its device id.
  new MutationObserver(()=>{ const n = $("unitName").textContent.trim(); if(n && state.device) setUnitName(state.device.id, n); })
    .observe($("unitName"), { childList:true, characterData:true, subtree:true });
  onDevicesChange(schedule);
  document.addEventListener("visibilitychange", tick);
  loadRemembered().then(tick);
  setInterval(tick, TICK_MS);
  tick();
}
