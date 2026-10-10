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
  if("time"in kv) $("timeState").textContent = kv.time==="1"?"yes":"no";
  if("gps" in kv) $("gpsState").textContent  = kv.gps==="1"?"yes":"no";
  // Gray the sync button once both time and GPS are synced.
  const bothSynced = ($("timeState").textContent==="yes" && $("gpsState").textContent==="yes");
  $("syncBtn").disabled = !state.connected || bothSynced;
  if("susp"in kv) $("susp").textContent = kv.susp;
  if("lk"  in kv) $("lk").textContent   = kv.lk;
  if("conf"in kv){ $("conf").textContent = kv.conf;
    $("conf").style.color = (+kv.conf>0) ? "#f85149" : ""; }
  if("db"  in kv) $("db").textContent = kv.db==="1"?"loaded":"fallback";
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
  if("src" in kv && !$("scanSetDlg").open){ const m=+kv.src;
    $("sBle").checked=!!(m&2); $("s24").checked=!!(m&1); $("s5").checked=!!(m&16); $("s154").checked=!!(m&4); }
  if("bri" in kv){ $("briVal").textContent = kv.bri;
    if(document.activeElement !== $("briSlider")) $("briSlider").value = kv.bri; }
}
