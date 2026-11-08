// Wi-Fi download stepper (#wifiDlg, Data page "Download over Wi-Fi").
// The scanner raises a SoftAP and serves its logs over plain http://. Android + Chrome don't let this HTTPS
// page join that network or fetch from it, so the user has to: (1) wait for the AP, (2) join it by hand in
// Wi-Fi settings, (3) open the http:// page in another tab, (4) stop the AP. Each step gets one big action,
// and the dialog opens the instant the button is tapped (the AP takes seconds to come up).
//
// Advancing: step 1 -> 2 when STATUS carries the AP creds (state.ap); 2 -> 3 when STATUS apsta > 0
// (firmware that reports joined stations), when the page becomes visible again after the user left it
// (they went to Wi-Fi settings), or on "I've joined it"; 3 -> 4 once the download page was opened.
// Polls state every 300 ms while open, so it doesn't depend on which status.js copy a phone has cached.
//
// CYD download boot (fw 0.35.0): the CYD can't fit Wi-Fi beside the Bluetooth link, so it sends the creds with
// STATUS apboot=1 and restarts with Bluetooth off. The flow keeps those creds (`boot`) and carries on with no link:
// joining is confirmed by hand, and Stop opens the page's /stop, which restarts the scanner.
import { state } from '../state.js';
import { $, log } from '../util/dom.js';
import { enc } from '../config.js';

const SLOW_MS = 12000, STUCK_MS = 45000;
let step = 0;          // 0 idle, 1 starting, 2 join, 3 open page, 4 finish
let startedAt = 0, timer = 0, leftAt = 0, qrKey = "";
let boot = null;       // {ssid,psk,url} while a CYD download boot is running (no Bluetooth link)

const curAp = () => state.ap || boot;
const isOn = () => state.wifiOn || !!boot;

// Escape the characters that are special inside a WIFI: QR payload (\ ; , : ").
const wq = s => String(s).replace(/([\\;,:"])/g, "\\$1");

// Uses the vendored qrcode-generator (webapp/js/vendor/qrcode.js, MIT, Kazuhiko Arase) via its global `qrcode`.
function drawQr(canvas, text){
  if(typeof qrcode !== "function") return false;
  const q = qrcode(0, "M"); q.addData(text); q.make();
  const n = q.getModuleCount(), cell = 5, pad = 4*cell, px = n*cell + 2*pad;
  canvas.width = canvas.height = px;
  const g = canvas.getContext("2d");
  g.fillStyle = "#fff"; g.fillRect(0, 0, px, px);
  g.fillStyle = "#000";
  for(let r=0;r<n;r++) for(let c=0;c<n;c++) if(q.isDark(r,c)) g.fillRect(pad+c*cell, pad+r*cell, cell, cell);
  return true;
}

async function sendWifi(on){
  await state.chars.cmd.writeValue(enc.encode(on ? "1" : "0"));
  state.wifiOn = on;
}

// STATUS apsta (stations joined to the AP); null while the firmware doesn't send it, or with no link (download boot).
function apSta(){
  if(boot) return null;
  const v = state.status && state.status.apsta; return v === undefined || v === "" ? null : +v;
}

function showErr(msg){ $("wfErr").textContent = msg; $("wfErr").hidden = !msg; }

function render(){
  for(let i=1;i<=4;i++){
    const li = $("wfS"+i);
    li.classList.toggle("active", i === step);
    li.classList.toggle("done", i < step);
    li.classList.toggle("todo", i > step);
    li.querySelector(".wfnum").textContent = i < step ? "✓" : String(i);
  }
  const ap = curAp(), sta = apSta();
  if(step === 1 && boot){
    $("wfS1Msg").textContent = "The scanner is restarting into Wi-Fi download…";
    $("wfS1Sub").textContent = "Bluetooth disconnects until you finish. That's expected.";
  } else if(step === 1){
    const secs = Math.round((Date.now() - startedAt) / 1000);
    $("wfS1Msg").textContent = "Starting the scanner's Wi-Fi… " + secs + " s";
    $("wfS1Sub").textContent = secs*1000 >= STUCK_MS
      ? "No answer from the scanner yet. Close this and try again, or check it is still connected."
      : secs*1000 >= SLOW_MS ? "The scanner finishes its current scan first, so this can take ~20 s." : "";
  }
  if(ap){
    $("wfSsid").textContent = ap.ssid;
    $("wfPsk").textContent = ap.psk;
    $("wfS2Done").textContent = "Joined " + ap.ssid + ".";
    $("wfOpen").href = ap.url;
    const key = ap.ssid + "|" + ap.psk;
    if(key !== qrKey){ qrKey = key; drawQr($("wfQr"), "WIFI:T:WPA;S:" + wq(ap.ssid) + ";P:" + wq(ap.psk) + ";;"); }
  }
  $("wfJoined").hidden = !(sta > 0);
  $("wfJoinedBtn").hidden = sta > 0;
  $("wfNotJoined").hidden = sta !== 0;   // only when the firmware positively reports nobody joined
  $("wfCancelBtn").hidden = !isOn() || step >= 4;   // step 4 has its own Stop
  $("wifiBtn").textContent = isOn() ? "Wi-Fi download is on — continue" : "Download over Wi-Fi";
  const s4 = $("wfS4Sub");
  if(s4) s4.textContent = boot
    ? "Got what you need? Restart the scanner: it goes back to scanning, and your phone returns to its usual network. Then reconnect over Bluetooth."
    : "Got what you need? Stop the scanner's Wi-Fi so it goes back to scanning. Your phone then returns to its usual network.";
  $("wfStopBtn").textContent = boot ? "Restart the scanner" : "Stop Wi-Fi download";
}

function tick(){
  // A CYD announces its download boot just before the Bluetooth link drops: keep the creds.
  if(!boot && state.ap && state.status && state.status.apboot === "1" && step >= 1){
    boot = { ...state.ap };
    log("Wi-Fi download: the scanner restarts with Bluetooth off (" + boot.ssid + ").");
  }
  if(boot){
    if(step === 1) step = 2;   // 2 -> 3 by "I've joined it" or coming back from Wi-Fi settings
    render();
    if(!$("wifiDlg").open && !boot){ clearInterval(timer); timer = 0; }
    return;
  }
  if(!state.connected){
    if(step){ step = 0; showErr("Bluetooth disconnected. Reconnect to the scanner, then start again."); render(); }
    clearInterval(timer); timer = 0; return;
  }
  if(step === 1 && state.ap){ step = 2; log("Wi-Fi download: the scanner's Wi-Fi is up (" + state.ap.ssid + ")."); }
  else if(step >= 2 && !state.ap && state.wifiOn){ step = 1; startedAt = Date.now(); }   // AP dropped: wait again
  if(step === 2 && apSta() > 0) step = 3;
  render();
  if(!$("wifiDlg").open && !state.wifiOn){ clearInterval(timer); timer = 0; }
}

export async function openWifiFlow(){
  showErr("");
  $("wifiDlg").showModal();
  if(boot){ if(!step) step = 2; if(!timer) timer = setInterval(tick, 300); render(); return; }
  if(!state.wifiOn){
    step = 1; startedAt = Date.now(); render();   // instant feedback: the AP takes seconds to come up
    try { await sendWifi(true); log("Wi-Fi download: starting the scanner's Wi-Fi…"); }
    catch(e){ step = 0; render(); showErr("Couldn't reach the scanner: " + e.message); return; }
  } else if(!step) step = state.ap ? 2 : 1;
  if(!timer) timer = setInterval(tick, 300);
  render();
}

export async function stopWifiFlow(){
  if(boot){   // no Bluetooth link: the scanner's own page restarts it
    window.open(boot.url + "/stop", "_blank", "noopener");
    log("Wi-Fi download: restarting the scanner. Reconnect over Bluetooth when it's back.");
    boot = null; step = 0; state.ap = null; state.wifiOn = false; render();
    $("wifiDlg").close();
    return;
  }
  try { await sendWifi(false); log("Wi-Fi download off."); }
  catch(e){ showErr("Couldn't reach the scanner: " + e.message); return; }
  step = 0; state.ap = null; render();
  $("wifiDlg").close();
}

export async function copyPassword(){
  const psk = state.ap && state.ap.psk, b = $("wfCopyBtn");
  if(!psk) return;
  try { await navigator.clipboard.writeText(psk); }
  catch(e){ showErr("Copy failed. Type the password shown above."); return; }
  b.textContent = "Copied ✓";
  setTimeout(()=>{ b.textContent = "Copy password"; }, 1500);
}

// Android's Wi-Fi settings via an intent: link. If Chrome won't resolve it the page simply stays visible,
// so after a beat show the manual hint.
export function openWifiSettings(){
  $("wfWifiHint").hidden = true;
  location.href = "intent:#Intent;action=android.settings.WIFI_SETTINGS;end";
  setTimeout(()=>{ if(document.visibilityState === "visible") $("wfWifiHint").hidden = false; }, 1500);
}

export function markJoined(){ if(step === 2){ step = 3; render(); } }
export function pageOpened(){ if(step === 3) setTimeout(()=>{ step = 4; render(); }, 800); }
export function reopenPage(){ const ap = curAp(); if(ap) window.open(ap.url, "_blank", "noopener"); }

// Leaving the page (Wi-Fi settings, the download tab) and coming back is the cue for the next step.
document.addEventListener("visibilitychange", ()=>{
  if(document.visibilityState !== "visible"){ if(step === 2 || step === 3) leftAt = step; return; }
  if(leftAt === 2 && step === 2 && curAp()) step = 3;
  else if(leftAt === 3 && step === 3) step = 4;
  leftAt = 0;
  if($("wifiDlg").open) render();
});
