// The Data page's time-range picker (#expRange + custom D/H/M) -> {mode, arg, tag} for the device's
// on-CYD filter: mode 0 = since boot / 1 = past 24 h / 2 = past <arg> minutes. Shared by the
// all-logs export (download.js) and the selected-sessions export (sessions.js).
import { $, log } from '../util/dom.js';

export function exportSpec(){
  const m=$("expRange").value;
  if(m==="2"){
    const mins=Math.round((+$("expD").value||0)*1440+(+$("expH").value||0)*60+(+$("expM").value||0));
    if(mins<1){ log("Custom range: enter at least 1 minute."); return null; }
    const a=Math.min(mins,525600);
    return {mode:2, arg:a, tag:"last"+a+"min"};
  }
  return m==="1" ? {mode:1, arg:0, tag:"last24h"} : {mode:0, arg:0, tag:"sinceboot"};
}
// The BLE "T:" command for a spec + target ("*" = every log file, "/logs/..." = one file).
export function exportCmd(sp, target){
  return "T:"+sp.mode+(sp.mode===2?":"+sp.arg:"")+(target?":"+target:"");
}
