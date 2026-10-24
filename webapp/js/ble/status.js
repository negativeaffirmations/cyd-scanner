// STATUS characteristic notify handler: "key=val;…" -> UI cards, banners, scan state.
import { state } from '../state.js';
import { $ } from '../util/dom.js';
import { DEAUTH_ALERT } from '../config.js';
import { updateScanBtn } from './commands.js';
import { renderApCard } from '../features/wifiap.js';
import { renderStatusInfo } from '../features/statusinfo.js';

// Clock + location icons: two stacked source dots (blue = phone on top, purple = device below; grey when
// that source is unavailable) and the icon tinted with the source in use. Built from the merged
// state.status because the keys arrive in different STATUS parts (tsrc/gpssrc part 1, tav/gav part 2).
//   tsrc 0 none / 1 phone / 2 device GPS / 3 device RTC     tav bit0 phone, bit1 device
//   gpssrc 0 none / 1 phone / 2 device GPS / 3 held          gav bit0 phone, bit1 device
function setSrc(id, avail, use){
  const el = $(id);
  el.querySelector(".dot.ph").classList.toggle("avail", !!(avail & 1));
  el.querySelector(".dot.dev").classList.toggle("avail", !!(avail & 2));
  el.classList.toggle("src-ph",   use === "ph");
  el.classList.toggle("src-dev",  use === "dev");
  el.classList.toggle("src-held", use === "held");   // no live source: last known position is logged (orange)
}
function renderSourceIcons(){
  const s = state.status, ts = +s.tsrc||0, gs = +s.gpssrc||0, sats = +s.sats||0;
  // tav/gav come from fw >= 0.14; older firmware: infer availability from the source in use.
  const tav = "tav" in s ? +s.tav : (ts===1 ? 1 : ts>=2 ? 2 : 0);
  const gav = "gav" in s ? +s.gav : (gs===1 ? 1 : gs===2 ? 2 : 0);
  setSrc("timeState", tav, ts===1 ? "ph" : ts>=2 ? "dev" : "");
  setSrc("gpsState",  gav, gs===1 ? "ph" : gs===2 ? "dev" : gs===3 ? "held" : "");
  const avail = m => [(m&1) && "phone", (m&2) && "device"].filter(Boolean).join(" + ") || "none";
  $("timeState").title = "Time: " + (ts===3 ? "device RTC" : ts===2 ? "device GPS" : ts===1 ? "phone" : "not set")
                       + " · available: " + avail(tav);
  $("gpsSats").textContent = (gs===2 && sats>0) ? sats : "";   // sat count only while the device GPS drives
  $("gpsState").title = "GPS: " + (gs===2 ? "device · "+sats+" sat"+(sats===1?"":"s")
                                 : gs===1 ? "phone"
                                 : gs===3 ? "no live fix · logging the last known position"
                                 : "no fix") + " · available: " + avail(gav);
}

export function onStatus(ev){
  const txt = new TextDecoder().decode(ev.target.value);
  const kv = {}; txt.split(";").forEach(p=>{const[a,b]=p.split("=");if(a)kv[a]=b;});
  if("w24" in kv) $("w24").textContent = kv.w24;
  if("w5"  in kv) $("w5").textContent  = kv.w5;
  if("ble" in kv) $("ble").textContent = kv.ble;
  if("prb" in kv) $("prb").textContent = kv.prb;
  if("z"   in kv) $("z154").textContent = kv.z;
  if("uniq"in kv) $("uniq").textContent= kv.uniq;
  Object.assign(state.status, kv);   // latest value of every key, for the status-icon info modal
  if("link"in kv || "scan"in kv){ const el = $("linkState");   // host<->C5 scanner UART link (the headless host's only link readout)
    const up = state.status.link==="1", scanning = up && state.status.scan==="1";
    // Classes, not el.style.color: the inner glyph has its own muted colour (.si .material-symbols-outlined),
    // so an inline colour on the outer span never reaches it. Gray down / white up / light blue up + scanning.
    el.classList.toggle("linkup", up); el.classList.toggle("linkscan", scanning);
    el.title = scanning ? "Scanner (C5) link: up · scanning" : up ? "Scanner (C5) link: up · not scanning" : "Scanner (C5) link: DOWN"; }
  if("time"in kv) $("timeState").classList.toggle("on", kv.time==="1");  // status icons: white when on
  if("gps" in kv) $("gpsState").classList.toggle("on", kv.gps==="1");
  if(["tsrc","tav","gpssrc","gav","sats"].some(k=>k in kv)) renderSourceIcons();
  // Gray the sync button once both time and GPS are synced.
  const bothSynced = $("timeState").classList.contains("on") && $("gpsState").classList.contains("on");
  $("syncBtn").disabled = !state.connected || bothSynced;
  if("susp"in kv) $("susp").textContent = kv.susp;
  if("lk"  in kv) $("lk").textContent   = kv.lk;
  if("conf"in kv){ $("conf").textContent = kv.conf;
    $("conf").style.color = (+kv.conf>0) ? "#f85149" : ""; }
  if("db"  in kv) $("db").classList.toggle("on", kv.db==="1");   // DB loaded -> white icon
  if("wl"  in kv){ $("wlCnt").textContent = kv.wl; $("wlCnt2").textContent = kv.wl; $("wlHdr").textContent = kv.wl; }
  if("muted"in kv) $("wlMuted").textContent = kv.muted;
  if("fol"in kv){ const nf = +kv.fol||0;
    $("folBanner").style.display = nf>0 ? "flex" : "none"; $("folCount").textContent = nf; }
  if("deauth"in kv || "evil"in kv){   // C5 deauth / evil-twin event counts (STATUS deauth=/evil=)
    const da = +kv.deauth||0, ev = +kv.evil||0, parts = [];
    if(da >= DEAUTH_ALERT) parts.push("Deauth flood: "+da+" frames/s");
    if(ev > 0) parts.push("Evil twin: "+ev+" SSID"+(ev>1?"s":""));
    $("alertBanner").style.display = parts.length ? "flex" : "none";
    $("alertText").textContent = parts.join(" · ");
  }
  if("dl" in kv){                                       // Wi-Fi download AP creds (present only while the AP is up)
    state.ap = (kv.dl==="1" && kv.apssid && kv.appsk) ? { ssid:kv.apssid, psk:kv.appsk, url:kv.apurl||"http://192.168.4.1" } : null;
    renderApCard();
  }
  if("sid" in kv) state.scannerId = kv.sid;            // scan provenance: stable CYD id (eFuse MAC)
  if("fw"  in kv) state.scannerFw = kv.fw;             // CYD firmware version
  if("dbgen"in kv) state.scannerDbGen = +kv.dbgen||0;  // sig-DB generation
  if("name"in kv){ state.scannerName = kv.name; $("unitName").textContent = kv.name; }  // unit name, e.g. CFLO-MINI-KOBALI
  if("sess"in kv) $("sessName").textContent = "Session: " + kv.sess;
  if("scan"in kv){ state.scanning = kv.scan==="1"; updateScanBtn(); }
  if("src" in kv){ const m=+kv.src;
    if(!$("scanSetDlg").open){ $("sBle").checked=!!(m&2); $("s24").checked=!!(m&1); $("s5").checked=!!(m&16); $("s154").checked=!!(m&4); }
    // Gray the scan-type tiles that aren't being collected under the current config (MASK bits).
    $("st24").classList.toggle("off", !(m&1));  $("st5").classList.toggle("off", !(m&16));
    $("stble").classList.toggle("off", !(m&2)); $("st154").classList.toggle("off", !(m&4)); }
  // Time / GPS source preference (fw >= 0.15). The selects stay disabled until the scanner reports it,
  // so older firmware never receives a "P:" it doesn't understand.
  if("tpref" in kv) { $("tPrefSel").value = kv.tpref; $("tPrefSel").disabled = false; }
  if("gpref" in kv) { $("gPrefSel").value = kv.gpref; $("gPrefSel").disabled = false; }
  if("bri" in kv){ $("briVal").textContent = kv.bri;
    if(document.activeElement !== $("briSlider")) $("briSlider").value = kv.bri; }
  if("lock"in kv){ const locked = kv.lock==="1"; const b = $("lockBtn");
    b.dataset.locked = locked ? "1" : "0";
    b.textContent = locked ? "Unlock display" : "Lock display";
    b.classList.toggle("danger", locked); }   // highlight while locked
  if("bat"in kv){ const p = +kv.bat;           // battery % from the CYD divider (-1 = no cell / on USB)
    const el = $("battState"), ic = $("battIcon"), pc = $("battPct");
    if(Number.isFinite(p) && p >= 0){
      el.classList.remove("off");
      pc.textContent = p + "%";
      ic.textContent = p>=90 ? "battery_full"  : p>=70 ? "battery_6_bar" : p>=55 ? "battery_5_bar"
                     : p>=40 ? "battery_4_bar" : p>=25 ? "battery_3_bar" : p>=15 ? "battery_2_bar"
                     : p>=5  ? "battery_1_bar" : "battery_alert";
      el.classList.toggle("lowbat", p < 20);   // red when low
      el.title = "Battery " + p + "%";
    } else {
      el.classList.add("off"); el.classList.remove("lowbat");
      pc.textContent = ""; ic.textContent = "battery_unknown";
      el.title = "Battery: no cell / on USB";
    }
  }
  renderStatusInfo();   // live-refresh the status-icon info modal if it's open
}
