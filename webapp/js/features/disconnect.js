// Disconnect from the scanner on purpose (webapp 2026-11-26). Before this the only way out was the Android app's
// notification button or closing the app. Two places: the Bluetooth row of the status popup (tap the app-bar Bluetooth
// icon) and Settings -> Device(s) -> Scanners. The normal gattserverdisconnected path (connection.js onDisconnect) does
// the teardown; the Android app's shim fires it too and reports it as a disconnect, not a lost link.
// NEW file, wired from main.js (stale-module rule).
import { state } from '../state.js';
import { $, toast } from '../util/dom.js';
import { anyBusy } from '../ble/xfer.js';

export function disconnectScanner(){
  const d = state.device;
  if(!d || !d.gatt || !d.gatt.connected) return;
  if(anyBusy()){ toast("A transfer is running. Finish or cancel it first, then disconnect.", { err:true }); return; }
  try{ d.gatt.disconnect(); }catch(e){}
}

// The Settings button follows the connection icon's state (#conn: "up" while connected).
function sync(){
  const b = $("disconnectBtn"), c = $("conn");
  if(b && c) b.disabled = !/\bup\b/.test(c.className);
}

export function initDisconnect(){
  const b = $("disconnectBtn");
  if(b) b.onclick = disconnectScanner;
  const c = $("conn");
  if(c) new MutationObserver(sync).observe(c, { attributes:true });
  sync();
}
