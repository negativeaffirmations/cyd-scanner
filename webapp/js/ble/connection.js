// BLE connect / disconnect lifecycle: GATT setup, characteristic discovery, notify wiring,
// and the full UI enable/disable + teardown.
import { state } from '../state.js';
import { SVC, TIME, GPS, CMD, STAT, LOG, DETS } from '../config.js';
import { $, log, setConn } from '../util/dom.js';
import { updateScanBtn, syncTime } from './commands.js';
import { startGps } from '../gps.js';
import { onStatus } from './status.js';
import { onLogData } from './download.js';
import { onDets, resetPause } from '../features/detections.js';
import { clearSightings } from '../features/sightings.js';
import { renderRemoteUI } from '../features/remote.js';
import { updateButtons as updateSessButtons } from '../features/sessions.js';
import { renderStatusInfo } from '../features/statusinfo.js';
import { acquireWake, releaseWake } from '../wakelock.js';
import { AUTH_UUID, authenticate, setLinkSecurity, clearLinkSecurity, openAuthDlg, legacyRefusal, legacyStatusRefusal, primeLegacyGuard, showRefusal, mayShareLocation } from './auth.js';

// Re-entrancy guard (REV-018): one connect/reconnect at a time (double taps, Save & reconnect while
// a connect is still running).
let connecting = false;

export async function connect(){
  if(!navigator.bluetooth){ log("Web Bluetooth not available. Use Chrome on Android."); return; }
  if(connecting) return;
  connecting = true;
  try{
    try{
      setConn("connecting");
      state.device = await navigator.bluetooth.requestDevice({
        // Filter by the advertised service UUID (robust) rather than the name,
        // which rides in the scan response and isn't always seen by the name filter.
        filters:[{services:[SVC]}], optionalServices:[SVC]
      });
    }catch(e){ setConn("disconnected"); log("Connect failed: "+e.message); return; }
    await connectDevice();
  } finally { connecting = false; }
}

// Reconnect to the last-picked scanner without the chooser (gatt.connect() needs no user gesture) —
// used by the key dialog's "Save & reconnect". Falls back to the chooser when none was picked yet.
export async function reconnect(){
  if(!state.device) return connect();
  if(connecting || state.device.gatt.connected) return;
  connecting = true;
  try{ setConn("connecting"); await connectDevice(); }
  finally { connecting = false; }
}

// Attach a listener once per object (a reconnect to the same device may hand back the same objects).
const hooked = new WeakMap();   // target -> Set of handlers already attached
function once(target, ev, fn){
  let s = hooked.get(target); if(!s){ s = new Set(); hooked.set(target, s); }
  if(s.has(fn)) return; target.addEventListener(ev, fn); s.add(fn);
}

async function connectDevice(){
  try{
    once(state.device, "gattserverdisconnected", onDisconnect);
    log("Connecting to "+state.device.name+"…");
    const server = await state.device.gatt.connect();
    const svc = await server.getPrimaryService(SVC);
    // Phone-link authentication (docs/ble-auth.md). The AUTH characteristic's presence marks new
    // firmware; without it this is an old, unauthenticated scanner -> legacy path + warning badge.
    let authChar = null;
    try{ authChar = await svc.getCharacteristic(AUTH_UUID); }
    catch(e){ if(e.name !== "NotFoundError") throw e; }
    let sec = "legacy", secInfo = null;
    if(authChar){
      const r = await authenticate(authChar, state.device);
      if(r.status !== "ok"){
        log("Auth: "+r.status+(r.msg ? " — "+r.msg : "")+". Disconnecting.");
        try{ state.device.gatt.disconnect(); }catch(_){}
        setConn("disconnected");
        openAuthDlg(r);
        return;
      }
      sec = "auth"; secInfo = { name:r.info.name || "", kg:r.info.kg, sid:r.info.sid };
      log("Authenticated to "+(r.info.name || r.info.sid)+".");
    }
    state.chars.time = await svc.getCharacteristic(TIME);
    state.chars.gps  = await svc.getCharacteristic(GPS);
    state.chars.cmd  = await svc.getCharacteristic(CMD);
    state.chars.stat = await svc.getCharacteristic(STAT);
    state.chars.log  = await svc.getCharacteristic(LOG);
    if(!authChar){
      // Legacy path: read STATUS once BEFORE subscribing. "auth=0" = new firmware behind a stale Android
      // GATT cache (REV-016); a name/sid we hold a key for, or fw >= 0.18.0 = impostor or stale cache (M2).
      let st = "";
      try{ st = new TextDecoder().decode(await state.chars.stat.readValue()); }catch(e){ log("STATUS read failed: "+e.message); }
      const refuse = await legacyRefusal(state.device.name, st);
      if(refuse){
        log(refuse.title+" — disconnecting.");
        try{ state.device.gatt.disconnect(); }catch(_){}
        setConn("disconnected"); clearLinkSecurity();
        showRefusal(refuse);
        return;
      }
      await primeLegacyGuard();
      log("WARNING: this scanner's firmware has no phone-link authentication (unsecured). Update it.");
    }
    setLinkSecurity(sec, secInfo);   // before subscribing, so the legacy STATUS guard below is armed
    await state.chars.stat.startNotifications();
    once(state.chars.stat, "characteristicvaluechanged", onStatus);
    if(!authChar) once(state.chars.stat, "characteristicvaluechanged", onLegacyStatus);
    await state.chars.log.startNotifications();
    once(state.chars.log, "characteristicvaluechanged", onLogData);
    // Live detection stream (optional — older firmware may not expose it).
    try {
      state.chars.dets = await svc.getCharacteristic(DETS);
      await state.chars.dets.startNotifications();
      once(state.chars.dets, "characteristicvaluechanged", onDets);
    } catch(e){ log("No live-detection stream on this device."); }
    if(!state.device.gatt.connected) return;   // the legacy STATUS guard refused it mid-setup
    setConn("connected");
    setLinkSecurity(sec, secInfo);   // after setConn (it rewrites the icon): legacy tint+badge / authed
    state.connected = true;
    $("connectBtn").style.display = "none";
    ["scanNavBtn","scanBtn","pauseBtn","newSessBtn","syncBtn","reloadBtn","dlBtn","wifiBtn","listBtn","sessWipeBtn","expSel","briSlider","lockBtn","settingsBtn","mapCurBtn","sBle","s24","s5","s154","dlfBleBtn"]
      .forEach(id=>$(id).disabled=false);
    $("wlBtn").disabled=false;
    renderRemoteUI();   // enable "Upload current session" now the link is up
    log("Connected.");
    acquireWake();   // keep the screen on so GPS keeps flowing (see wakelock.js)
    // TIME / GPS only to an authenticated scanner, or a legacy one the user agreed to (one-time prompt per
    // device name). Not awaited: the prompt may stay open; the connect is done either way.
    shareLocation(false);
  }catch(e){
    setConn("disconnected"); clearLinkSecurity(); log("Connect failed: "+e.message);
    try{ if(state.device && state.device.gatt.connected) state.device.gatt.disconnect(); }catch(_){}
  }
}

// Send our time + start GPS if this link may have them (see mayShareLocation). ask=true re-prompts a
// legacy scanner after a remembered "no" (the Sync button). Returns whether anything was sent.
export async function shareLocation(ask){
  const dev = state.device;
  const ok = await mayShareLocation(dev && dev.name, ask);
  if(!ok || !state.connected || state.device !== dev){
    if(state.connected && !ok) log("Time & location NOT sent — unsecured firmware (tap Sync time & GPS to send).");
    return false;
  }
  await syncTime();
  startGps();
  return true;
}

// Legacy-link STATUS guard: a later STATUS part can reveal a sid we hold a key for, or fw >= 0.18.0.
function onLegacyStatus(ev){
  const r = legacyStatusRefusal(state.device && state.device.name, new TextDecoder().decode(ev.target.value));
  if(!r) return;
  log(r.title+" — disconnecting.");
  try{ state.device.gatt.disconnect(); }catch(_){}
  showRefusal(r);
}

export function onDisconnect(){ setConn("disconnected"); clearLinkSecurity(); log("Disconnected."); state.connected=false;
  state.scannerName = ""; $("unitName").textContent = "";
  clearSightings();   // drop the in-memory per-device sighting trails for this session
  $("folBanner").style.display = "none"; $("alertBanner").style.display = "none"; state.lastDetRows = [];   // clear the alerts
  if($("folDlg").open) $("folDlg").close(); if($("folDetDlg").open) $("folDetDlg").close();
  $("connectBtn").style.display = "";
  ["scanNavBtn","scanBtn","pauseBtn","newSessBtn","syncBtn","reloadBtn","dlBtn","wifiBtn","listBtn","sessDlBtn","sessDlfBtn","sessDelBtn","sessMapBtn","sessWipeBtn","expSel","briSlider","lockBtn","settingsBtn","mapCurBtn","sBle","s24","s5","s154","dlfBleBtn","tPrefSel","gPrefSel","ledSlider"]
    .forEach(id=>$(id).disabled=true);
  $("sessUploadBtn").disabled=true; renderRemoteUI(); updateSessButtons();   // disable session/current upload (no link)
  $("wlBtn").disabled=true; if($("wlDlg").open) $("wlDlg").close();
  if($("scanSetDlg").open) $("scanSetDlg").close();
  state.wifiOn=false; $("wifiBtn").textContent="Wi-Fi download mode"; state.ap=null; $("apCard").style.display="none";
  state.scanning=false; updateScanBtn(); resetPause();
  $("scanPanel").style.display="none"; $("exploreView").style.display="none";
  if($("detDlg").open) $("detDlg").close();
  $("dets").innerHTML='<div class="sub" style="padding:6px">Connect to see live results…</div>';
  state.lastDetRows = []; $("catChips").innerHTML=""; $("catChips").style.display="none";
  { const bs=$("battState"); bs.classList.add("off"); bs.classList.remove("lowbat");  // reset battery indicator
    $("battPct").textContent=""; $("battIcon").textContent="battery_unknown"; bs.title="Battery"; }
  { const ls=$("linkState"); ls.classList.remove("linkup","linkscan"); ls.title="Scanner (C5) link"; }  // reset C5-link icon (gray)
  for(const k in state.status) delete state.status[k]; renderStatusInfo();   // status modal: back to "not connected"
  if(state.gpsWatch!==null){ navigator.geolocation.clearWatch(state.gpsWatch); state.gpsWatch=null; }
  releaseWake(); }
