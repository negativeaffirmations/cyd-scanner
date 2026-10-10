// Session-log management: list ("Q" -> SESS= stream), download, map, delete, wipe.
// The one session list feeds both the Session-logs picker and the Explore picker.
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, log } from '../util/dom.js';
import { openMap } from './map.js';

export async function refreshSessions(){
  try{ await state.chars.cmd.writeValue(enc.encode("Q")); log("Requesting session list…"); }
  catch(e){ log("List failed: "+e.message); }
}
export function populateSessions(text){
  const sels=[$("sessSel"),$("expSel")]; sels.forEach(s=>s.innerHTML="");  // one list feeds both pickers
  const rows=(text||"").split("\n").filter(l=>l.includes("\t"));
  if(!rows.length){ sels.forEach(s=>s.innerHTML="<option>— no sessions —</option>");
    $("sessDlBtn").disabled=true; $("sessDlfBtn").disabled=true; $("sessDelBtn").disabled=true; $("sessMapBtn").disabled=true;
    if(state.exploreWant){ state.exploreWant=false; $("expMsg").textContent="No sessions on the device."; $("scanViewer").innerHTML=""; }
    return; }
  rows.sort().reverse();  // date-time names sort newest-first
  const fi=rows.findIndex(r=>{const f=r.split("\t")[2]; return f==="current"||f==="latest";});
  if(fi>0) rows.unshift(rows.splice(fi,1)[0]);  // float the flagged (newest) row to the top
  for(const r of rows){
    const [path,size,flag]=r.split("\t");
    const base=path.split("/").pop();
    const kb=(parseInt(size,10)/1024).toFixed(1);
    let label=base+"  ("+kb+" KB)";
    if(flag==="current")      label+="  (current session)";
    else if(flag==="latest")  label+="  (latest session)";
    sels.forEach(s=>{ const o=document.createElement("option"); o.value=path; o.textContent=label; s.appendChild(o); });
  }
  $("sessDlBtn").disabled=false; $("sessDlfBtn").disabled=false; $("sessDelBtn").disabled=false; $("sessMapBtn").disabled=false;
  log(rows.length+" session(s) found.");
  if(state.exploreWant){ state.exploreWant=false; fetchSession("explore"); }   // newest is first -> default pick
}
export async function fetchSession(sink){
  const path=$(sink==="explore"?"expSel":"sessSel").value;
  if(!path || path.startsWith("—")) return;
  try{ state.dl={mode:null,expect:0,got:0,chunks:[],name:path.split("/").pop(),sink:sink};
    if(sink==="map") $("mapMsg").textContent="Loading "+state.dl.name+"…";
    if(sink==="explore") $("expMsg").textContent="Loading "+state.dl.name+"…";
    await state.chars.cmd.writeValue(enc.encode("F:"+path)); log("Requesting "+state.dl.name+"…"); }
  catch(e){ log("Download failed: "+e.message); }
}
export function downloadSession(){ fetchSession("save"); }
export function mapSelectedSession(){ if(!$("mapDlg").open) openMap(); fetchSession("map"); }
export async function deleteSession(){
  const path=$("sessSel").value;
  if(!path || path.startsWith("—")) return;
  const base=path.split("/").pop();
  if(!confirm("Delete session "+base+"?\nThis cannot be undone. (The current in-progress session can't be deleted.)")) return;
  try{ await state.chars.cmd.writeValue(enc.encode("D:"+path)); log("Deleting "+base+"… (list will refresh)"); }
  catch(e){ log("Delete failed: "+e.message); }
}
// Wipe every session log (CMD "D:*"); the device keeps the live in-progress session.
export async function wipeAllLogs(){
  if(!state.chars.cmd) return;
  if(!confirm("Wipe ALL session logs on the SD card?\nThis cannot be undone. The current in-progress session is kept.")) return;
  try{ await state.chars.cmd.writeValue(enc.encode("D:*")); log("Wiping all logs… (list will refresh)"); }
  catch(e){ log("Wipe failed: "+e.message); }
}
