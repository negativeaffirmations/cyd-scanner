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

// File name for a time-range download: <first>-<last>.jsonl, both YYYYMMDD-HHMMSS (the same
// stamp format as the device's log names, so saved exports sort chronologically by name), taken
// from the first/last dated row actually in the file. The device's epochs are LOCAL time, so they
// are formatted with UTC getters. Falls back to `fallback` when nothing in the file is dated.
const stamp = e=>new Date(e*1000).toISOString().slice(0,19).replace(/[-:]/g,"").replace("T","-");
export function rangeFileName(text, fallback){
  let lo=Infinity, hi=0;
  const re=/"epoch":(\d+)/g;
  for(let m; (m=re.exec(text||"")); ){ const e=+m[1]; if(e>0){ if(e<lo) lo=e; if(e>hi) hi=e; } }
  return hi ? stamp(lo)+"-"+stamp(hi)+".jsonl" : fallback;
}
