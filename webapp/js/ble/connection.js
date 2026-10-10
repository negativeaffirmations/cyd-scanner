// BLE connect / disconnect lifecycle: GATT setup, characteristic discovery, notify wiring,
// and the full UI enable/disable + teardown.
import { state } from '../state.js';
import { SVC, TIME, GPS, CMD, STAT, LOG, DETS } from '../config.js';
import { $, log, setConn } from '../util/dom.js';
import { updateScanBtn, syncTime } from './commands.js';
import { startGps } from '../gps.js';
import { onStatus } from './status.js';
import { onLogData } from './download.js';
import { onDets } from '../features/detections.js';

export async function connect(){
  if(!navigator.bluetooth){ log("Web Bluetooth not available. Use Chrome on Android."); return; }
  try{
    setConn("connecting");
    state.device = await navigator.bluetooth.requestDevice({
      // Filter by the advertised service UUID (robust) rather than the name,
      // which rides in the scan response and isn't always seen by the name filter.
      filters:[{services:[SVC]}], optionalServices:[SVC]
    });
    state.device.addEventListener("gattserverdisconnected", onDisconnect);
    log("Connecting to "+state.device.name+"…");
    const server = await state.device.gatt.connect();
    const svc = await server.getPrimaryService(SVC);
    state.chars.time = await svc.getCharacteristic(TIME);
    state.chars.gps  = await svc.getCharacteristic(GPS);
    state.chars.cmd  = await svc.getCharacteristic(CMD);
    state.chars.stat = await svc.getCharacteristic(STAT);
    state.chars.log  = await svc.getCharacteristic(LOG);
    await state.chars.stat.startNotifications();
    state.chars.stat.addEventListener("characteristicvaluechanged", onStatus);
    await state.chars.log.startNotifications();
    state.chars.log.addEventListener("characteristicvaluechanged", onLogData);
    // Live detection stream (optional — older firmware may not expose it).
    try {
      state.chars.dets = await svc.getCharacteristic(DETS);
      await state.chars.dets.startNotifications();
      state.chars.dets.addEventListener("characteristicvaluechanged", onDets);
    } catch(e){ log("No live-detection stream on this device."); }
    setConn("connected");
    state.connected = true;
    $("connectBtn").style.display = "none";
    ["scanNavBtn","scanBtn","newSessBtn","syncBtn","reloadBtn","dlBtn","wifiBtn","listBtn","sessWipeBtn","sessSel","expSel","briSlider","settingsBtn","mapCurBtn","sBle","s24","s5","s154","dlfBleBtn"]
      .forEach(id=>$(id).disabled=false);
    $("wlBtn").disabled=false;
    log("Connected.");
    await syncTime();
    startGps();
  }catch(e){ setConn("disconnected"); log("Connect failed: "+e.message); }
}

export function onDisconnect(){ setConn("disconnected"); log("Disconnected."); state.connected=false;
  $("folBanner").style.display = "none"; $("alertBanner").style.display = "none"; state.lastDetRows = [];   // clear the alerts
  if($("folDlg").open) $("folDlg").close(); if($("folDetDlg").open) $("folDetDlg").close();
  $("connectBtn").style.display = "";
  ["scanNavBtn","scanBtn","newSessBtn","syncBtn","reloadBtn","dlBtn","wifiBtn","listBtn","sessDlBtn","sessDlfBtn","sessDelBtn","sessMapBtn","sessWipeBtn","sessSel","expSel","briSlider","settingsBtn","mapCurBtn","sBle","s24","s5","s154","dlfBleBtn"]
    .forEach(id=>$(id).disabled=true);
  if($("settingsDlg").open) $("settingsDlg").close();
  $("wlBtn").disabled=true; if($("wlDlg").open) $("wlDlg").close();
  if($("scanSetDlg").open) $("scanSetDlg").close();
  state.wifiOn=false; $("wifiBtn").textContent="Wi-Fi download mode";
  state.scanning=false; updateScanBtn();
  $("scanPanel").style.display="none"; $("exploreView").style.display="none";
  if($("detDlg").open) $("detDlg").close();
  $("dets").innerHTML='<div class="sub" style="padding:6px">Connect to see live results…</div>';
  state.lastDetRows = []; $("catChips").innerHTML=""; $("catChips").style.display="none";
  if(state.gpsWatch!==null){ navigator.geolocation.clearWatch(state.gpsWatch); state.gpsWatch=null; } }
