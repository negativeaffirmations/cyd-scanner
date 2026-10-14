// STATUS characteristic notify handler: "key=val;…" -> UI cards, banners, scan state.
import { state } from '../state.js';
import { $ } from '../util/dom.js';
import { DEAUTH_ALERT } from '../config.js';
import { updateScanBtn } from './commands.js';

export function onStatus(ev){
  const txt = new TextDecoder().decode(ev.target.value);
  const kv = {}; txt.split(";").forEach(p=>{const[a,b]=p.split("=");if(a)kv[a]=b;});
  if("w24" in kv) $("w24").textContent = kv.w24;
  if("w5"  in kv) $("w5").textContent  = kv.w5;
  if("ble" in kv) $("ble").textContent = kv.ble;
  if("prb" in kv) $("prb").textContent = kv.prb;
  if("z"   in kv) $("z154").textContent = kv.z;
  if("uniq"in kv) $("uniq").textContent= kv.uniq;
  if("time"in kv) $("timeState").classList.toggle("on", kv.time==="1");  // status icons: white when on
  if("tsrc" in kv){                                // v10: which clock source is live (1 phone / 2 onboard GPS)
    const ts = +kv.tsrc||0, el = $("timeState");
    el.classList.toggle("onboard", ts===2);        // cyan tint when the onboard GPS keeps time (no phone)
    el.title = ts===2 ? "Time: onboard GPS" : ts===1 ? "Time: phone-synced" : "Time: not set";
  }
  if("gps" in kv) $("gpsState").classList.toggle("on", kv.gps==="1");
  if("gpssrc" in kv || "sats" in kv){            // v9: which source is live (1 phone / 2 onboard) + module sats
    const src = +kv.gpssrc||0, sats = +kv.sats||0, el = $("gpsState"), sl = $("gpsSats");
    el.classList.toggle("onboard", src===2);     // cyan tint when the onboard NEO-6M is the active fix
    sl.textContent = (src===2 && sats>0) ? sats : "";   // show sat count only when onboard is driving
    el.title = src===2 ? ("GPS: onboard · "+sats+" sat"+(sats===1?"":"s"))
             : src===1 ? "GPS: phone"
             : (sats>0 ? ("GPS: no fix · onboard "+sats+" sat"+(sats===1?"":"s")+" in view") : "GPS: no fix");
  }
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
  if("sess"in kv) $("sessName").textContent = "Session: " + kv.sess;
  if("scan"in kv){ state.scanning = kv.scan==="1"; updateScanBtn(); }
  if("src" in kv){ const m=+kv.src;
    if(!$("scanSetDlg").open){ $("sBle").checked=!!(m&2); $("s24").checked=!!(m&1); $("s5").checked=!!(m&16); $("s154").checked=!!(m&4); }
    // Gray the scan-type tiles that aren't being collected under the current config (MASK bits).
    $("st24").classList.toggle("off", !(m&1));  $("st5").classList.toggle("off", !(m&16));
    $("stble").classList.toggle("off", !(m&2)); $("st154").classList.toggle("off", !(m&4)); }
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
}
