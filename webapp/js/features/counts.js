// Scan-summary modal: tap the Home summary card → every count it shows, one per line, spelled out
// (the card itself uses short labels so its columns stay on one line). Values come from the latest
// STATUS key=val map in state.status; re-renders live while open (ble/status.js calls renderCounts
// on every notify).
import { state } from '../state.js';
import { $, esc } from '../util/dom.js';

// [STATUS key, verbose label, scan-source mask bit(s) that must be on (0 = always collected)].
// Mask bits as in the Scan Settings toggles: Wi-Fi 2.4 = 1 (+ probe 8), BLE = 2, 802.15.4 = 4, 5 GHz = 16.
const ROWS = [
  ["w24",  "2.4 GHz Wi-Fi access points",              1],
  ["w5",   "5 GHz Wi-Fi access points",                16],
  ["ble",  "Bluetooth LE devices",                     2],
  ["z",    "802.15.4 devices (Zigbee / Thread)",       4],
  ["prb",  "Wi-Fi clients (probe requests + data frames)", 1],
  ["uniq", "Unique devices logged this session",       0],
  ["susp", "Suspect threats",                          0],
  ["lk",   "Likely threats",                           0],
  ["conf", "Confirmed threats",                        0],
];

export function renderCounts(){
  const dlg = $("countsDlg"); if(!dlg || !dlg.open) return;
  const k = state.status, live = state.connected, mask = +k.src || 0;
  $("countsUnit").textContent = state.scannerName || "";
  $("countsList").innerHTML = ROWS.map(([key, label, bit]) => {
    const off = live && "src" in k && bit && !(mask & bit);
    const v = !live ? "–" : off ? "off" : (key in k ? k[key] : "–");
    return '<div class="cnrow' + (off ? " off" : "") + '"><span class="cnlabel">' + esc(label) +
           '</span><span class="cnval">' + esc(String(v)) + '</span></div>';
  }).join("");
}

export function openCounts(){
  $("countsDlg").showModal();
  renderCounts();
}
