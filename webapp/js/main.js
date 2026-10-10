// Entry point: wires every DOM event listener to its handler. Loaded as <script type="module">,
// which is deferred, so the DOM is ready by the time this runs.
import { $, log } from './util/dom.js';
import { connect } from './ble/connection.js';
import { toggleScan, newSession, reloadDb, toggleWifi, setBrightness, sendSrcMask, syncTime } from './ble/commands.js';
import { downloadLog, downloadFiltered, downloadFilteredWifi } from './ble/download.js';
import { gpsOnce } from './gps.js';
import { refreshSessions, downloadSession, deleteSession, wipeAllLogs, mapSelectedSession, fetchSession } from './features/sessions.js';
import { openExplore, applyExplore } from './features/explore.js';
import { openWhitelist, wlCmd, refreshWhitelist, addWhitelistRule, wlKindChanged } from './features/whitelist.js';
import { openMap, mapCurrent, renderMap, plotCsvText } from './features/map.js';
import { openFollowList } from './features/followers.js';

// Whitelist "kind" select -> pattern length rules (+ initial pass).
$("wlKind").addEventListener("change", wlKindChanged);
wlKindChanged();

$("connectBtn").onclick = connect;
$("scanBtn").onclick = toggleScan;
$("newSessBtn").onclick = newSession;
$("scanNavBtn").onclick = ()=>{ const p=$("scanPanel"); p.style.display = p.style.display==="none" ? "" : "none"; };
$("exploreBtn").onclick = openExplore;
$("expSel").addEventListener("change", ()=>fetchSession("explore"));
["expSort","ef24","ef5","efble","efprb","ef154","efThreats","efDist","efText"].forEach(id=>
  $(id).addEventListener(id==="efText"?"input":"change", applyExplore));
$("efDist").addEventListener("input", applyExplore);
// Dual-handle time window: two ranges, clamped so start <= end.
$("efT0").addEventListener("input", ()=>{ if(+$("efT0").value>+$("efT1").value) $("efT1").value=$("efT0").value; applyExplore(); });
$("efT1").addEventListener("input", ()=>{ if(+$("efT1").value<+$("efT0").value) $("efT0").value=$("efT1").value; applyExplore(); });
$("detClose").onclick = ()=> $("detDlg").close();
$("detDlg").addEventListener("click", e=>{ if(e.target === $("detDlg")) $("detDlg").close(); });
$("catClose").onclick = ()=> $("catDlg").close();
$("catDlg").addEventListener("click", e=>{ if(e.target === $("catDlg")) $("catDlg").close(); });
$("folBanner").onclick = openFollowList;
$("folClose").onclick = ()=> $("folDlg").close();
$("folDlg").addEventListener("click", e=>{ if(e.target === $("folDlg")) $("folDlg").close(); });
$("folDetClose").onclick = ()=> $("folDetDlg").close();
$("folDetDlg").addEventListener("click", e=>{ if(e.target === $("folDetDlg")) $("folDetDlg").close(); });
$("scanSetBtn").onclick = ()=> $("scanSetDlg").showModal();
$("scanSetClose").onclick = ()=> $("scanSetDlg").close();
$("scanSetDlg").addEventListener("click", e=>{ if(e.target === $("scanSetDlg")) $("scanSetDlg").close(); });
["sBle","s24","s5","s154"].forEach(id=>$(id).addEventListener("change", sendSrcMask));
$("syncBtn").onclick = async () => { await syncTime(); gpsOnce(); };
$("reloadBtn").onclick = reloadDb;
$("dlBtn").onclick = downloadLog;
$("expRange").addEventListener("change", ()=>{ $("expCustom").style.display = $("expRange").value==="2" ? "" : "none"; });
$("dlfBleBtn").onclick = ()=>downloadFiltered("");
$("dlfWifiBtn").onclick = downloadFilteredWifi;
$("sessDlfBtn").onclick = ()=>{ const p=$("sessSel").value; if(p && !p.startsWith("—")) downloadFiltered(p); };
$("wifiBtn").onclick = toggleWifi;
$("listBtn").onclick = refreshSessions;
$("sessDlBtn").onclick = downloadSession;
$("sessDelBtn").onclick = deleteSession;
$("sessWipeBtn").onclick = wipeAllLogs;
$("briSlider").addEventListener("input", e=>{ $("briVal").textContent = e.target.value; });
$("briSlider").addEventListener("change", e=>{ setBrightness(e.target.value); });
$("settingsBtn").onclick = ()=> $("settingsDlg").showModal();
$("wlBtn").onclick = openWhitelist;
$("scanSetWlBtn").onclick = openWhitelist;
$("wlClose").onclick = ()=> $("wlDlg").close();
$("wlDlg").addEventListener("click", e=>{ if(e.target === $("wlDlg")) $("wlDlg").close(); });
$("wlReloadBtn").onclick = async()=>{ await wlCmd("W","Reloading whitelist…"); setTimeout(refreshWhitelist,300); };
$("wlAddBtn").onclick = addWhitelistRule;
$("settingsClose").onclick = ()=> $("settingsDlg").close();
$("settingsDlg").addEventListener("click", e=>{ if(e.target === $("settingsDlg")) $("settingsDlg").close(); });
$("mapBtn").onclick = openMap;
$("mapClose").onclick = ()=> $("mapDlg").close();
$("mapCurBtn").onclick = mapCurrent;
$("sessMapBtn").onclick = mapSelectedSession;
["fThreats","f24","f5","fble","fprb","f154","fEvents"].forEach(id=>$(id).addEventListener("change", ()=> renderMap()));
$("mapFile").addEventListener("change", e=>{
  const f=e.target.files[0]; if(!f) return;
  const rd=new FileReader();
  rd.onload=()=>plotCsvText(rd.result, f.name);
  rd.readAsText(f);
  e.target.value="";   // allow re-loading the same file
});
if(!navigator.bluetooth) log("This browser has no Web Bluetooth. Use Chrome on Android.");
