// Wi-Fi download AP join card (Data page). The headless host has no screen, so the device sends its
// SoftAP credentials in STATUS (apssid/appsk/apurl, only while the AP is up) and we draw the standard
// "WIFI:" join QR here for the phone camera to scan. The page itself can't join Wi-Fi or fetch the
// AP's http:// URL from this HTTPS app, hence scan-to-join + a link to open in a separate tab.
import { state } from '../state.js';
import { $ } from '../util/dom.js';

// Escape the characters that are special inside a WIFI: QR payload (\ ; , : ").
const wq = s => String(s).replace(/([\;,:"])/g, "\$1");

// Uses the vendored qrcode-generator (webapp/js/vendor/qrcode.js, MIT, Kazuhiko Arase) via its global `qrcode`.
function drawQr(canvas, text){
  if(typeof qrcode !== "function") return false;        // CDN lib not loaded (offline) -> text fallback
  const q = qrcode(0, "M"); q.addData(text); q.make();
  const n = q.getModuleCount(), cell = 5, pad = 4*cell, px = n*cell + 2*pad;
  canvas.width = canvas.height = px;
  const g = canvas.getContext("2d");
  g.fillStyle = "#fff"; g.fillRect(0, 0, px, px);
  g.fillStyle = "#000";
  for(let r=0;r<n;r++) for(let c=0;c<n;c++) if(q.isDark(r,c)) g.fillRect(pad+c*cell, pad+r*cell, cell, cell);
  return true;
}

let lastKey = "";
export function renderApCard(){
  const card = $("apCard"), ap = state.ap;
  if(!ap){ card.style.display = "none"; lastKey = ""; return; }
  card.style.display = "";
  const key = ap.ssid + "|" + ap.psk + "|" + ap.url;
  if(key === lastKey) return;                           // STATUS repeats ~1/s; only redraw on change
  lastKey = key;
  const ok = drawQr($("apQr"), "WIFI:T:WPA;S:" + wq(ap.ssid) + ";P:" + wq(ap.psk) + ";;");
  $("apQr").style.display = ok ? "" : "none";
  $("apTxt").textContent = "SSID " + ap.ssid + " · password " + ap.psk;
  $("apUrl").href = ap.url;
}
