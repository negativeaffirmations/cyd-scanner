// Session-log management: list ("Q" -> SESS= stream), download, map, upload, delete, wipe.
// Files are grouped into SCANS: one scan is normally one file, but older firmware split a busy drive
// into many parts (and New Session / reboots split a day), so consecutive files of the same stream
// (fg / bg) whose dated spans sit within GROUP_GAP_S of each other form one group. The Session-logs
// list is a multi-select of groups; every action fetches the selected groups' parts one at a time
// over BLE (bleFetch) and joins them (joinLogParts) into ONE file / ONE upload per scan.
// The one grouped list feeds both the Session-logs list and the Explore picker (#expSel).
import { state } from '../state.js';
import { $, log, esc, toast, saveBlob } from '../util/dom.js';
import { openMap, plotCsvText } from './map.js';
import { bleFetch } from '../ble/transfer.js';
import { joinLogParts } from '../data/parse.js';
import { uploadSessionText, isPaired } from './remote.js';
import { exportSpec, exportCmd, rangeFileName } from './timerange.js';

const GROUP_GAP_S = 6*60;          // next part starts within 6 min of the previous part's last row
let groups = [];                   // [{key, kind, parts:[{path,base,size,flag,first,last}], start, end, size, flag}]
const selected = new Set();        // group keys (first part's path) — survives a list refresh
let busy = false;
let exploreLoader = null;          // set by explore.js (avoids an import cycle)
export function setExploreLoader(fn){ exploreLoader = fn; }

// "YYYYMMDD-HHMMSS" in a file name -> local epoch (the device's epochs are LOCAL time as seconds).
function nameEpoch(base){
  const m=/(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})/.exec(base);
  return m ? Date.UTC(+m[1], +m[2]-1, +m[3], +m[4], +m[5], +m[6])/1000 : 0;
}
// Local-epoch -> "Oct 22, 2:05 PM" (UTC getters: the epoch is already local).
const fmtDay  = e=>new Date(e*1000).toLocaleString(undefined,{timeZone:"UTC",month:"short",day:"numeric",hour:"numeric",minute:"2-digit"});
const fmtTime = e=>new Date(e*1000).toLocaleTimeString(undefined,{timeZone:"UTC",hour:"numeric",minute:"2-digit"});
const fmtSize = b=>b>=1048576 ? (b/1048576).toFixed(1)+" MB" : Math.max(1,Math.round(b/1024))+" KB";

function buildGroups(text){
  const parts=(text||"").split("\n").filter(l=>l.includes("\t")).map(r=>{
    const [path,size,flag,first,last]=r.split("\t");
    const base=path.split("/").pop();
    const f=parseInt(first,10)||0, l=parseInt(last,10)||0;
    const start=f||nameEpoch(base), end=Math.max(l, start);
    return {path, base, size:parseInt(size,10)||0, flag:flag||"", start, end,
            kind: base.startsWith("bg-") ? "bg" : "fg", csv: /\.csv$/i.test(base)};
  });
  const out=[];
  for(const kind of ["fg","bg"]){
    const ps=parts.filter(p=>p.kind===kind && p.start && !p.csv).sort((a,b)=>a.start-b.start);
    let g=null;
    for(const p of ps){
      if(g && p.start - g.end <= GROUP_GAP_S){ g.parts.push(p); g.end=Math.max(g.end,p.end); }
      else { g={kind, parts:[p], start:p.start, end:p.end}; out.push(g); }
    }
  }
  // Undated files (pre-sync boots: sess-/bg-NNNNN) and legacy CSV: one group each.
  for(const p of parts) if(!p.start || p.csv) out.push({kind:p.kind, parts:[p], start:p.start, end:p.end});
  for(const g of out){
    g.key=g.parts[0].path;
    g.size=g.parts.reduce((s,p)=>s+p.size,0);
    g.flag=(g.parts.find(p=>p.flag)||{}).flag||"";
  }
  out.sort((a,b)=>(b.flag?1:0)-(a.flag?1:0) || (b.end||0)-(a.end||0) || b.key.localeCompare(a.key));  // flagged, then newest
  return out;
}
// Row line 1: the real file name(s) on the card (first … last for a multi-part scan).
function groupNames(g){
  const ps=g.parts;
  return ps.length===1 ? ps[0].base : ps[0].base+" … "+ps[ps.length-1].base;
}
// Row line 2: the human date/time span ("" when the files carry no time).
function groupTitle(g){
  if(!g.start) return "";
  const same = fmtDay(g.start).split(",")[0]===fmtDay(g.end).split(",")[0];
  return fmtDay(g.start)+(g.end-g.start>=60 ? " – "+(same?fmtTime(g.end):fmtDay(g.end)) : "");
}
function groupSub(g){
  const bits=[g.kind==="bg"?"Background":"Scan"];
  if(g.parts.length>1) bits.push(g.parts.length+" parts");
  bits.push(fmtSize(g.size));
  if(g.flag==="current") bits.push("current");
  else if(g.flag==="latest") bits.push("latest");
  return bits.join(" · ");
}
// Joined file name: a one-part scan keeps its own name; otherwise <first part>+<n>p.jsonl.
function groupFileName(gs, tag){
  const first=gs[0].parts[0].base.replace(/\.(jsonl|csv)$/i,"");
  const ext=(gs.length===1 && gs[0].parts.length===1 && gs[0].parts[0].csv) ? ".csv" : ".jsonl";
  const nParts=gs.reduce((s,g)=>s+g.parts.length,0);
  const many = gs.length>1 ? "+"+(gs.length-1)+"scans" : nParts>1 ? "+"+nParts+"p" : "";
  return first+many+(tag?"-"+tag:"")+ext;
}

export async function refreshSessions(){
  try{ await state.chars.cmd.writeValue(new TextEncoder().encode("Q")); log("Requesting session list…"); }
  catch(e){ log("List failed: "+e.message); }
}
export function populateSessions(text){
  groups=buildGroups(text);
  for(const k of [...selected]) if(!groups.some(g=>g.key===k)) selected.delete(k);
  renderList();
  const ex=$("expSel"); ex.innerHTML="";
  if(!groups.length){
    ex.innerHTML="<option>— no sessions —</option>";
    if(state.exploreWant){ state.exploreWant=false; $("expMsg").textContent="No sessions on the device."; $("scanViewer").innerHTML=""; }
    return;
  }
  for(const g of groups){
    const o=document.createElement("option"); o.value=g.key;
    const t=groupTitle(g);
    o.textContent=groupNames(g)+(t?"  — "+t:"")+"  ("+groupSub(g)+")"; ex.appendChild(o);
  }
  const nf=groups.reduce((s,g)=>s+g.parts.length,0);
  log(nf+" session file(s) in "+groups.length+" scan(s).");
  if(state.exploreWant){ state.exploreWant=false; exploreSelected(); }   // newest is first -> default pick
}
function renderList(){
  const box=$("sessList");
  if(!groups.length){ box.innerHTML='<div class="sub">— no sessions —</div>'; updateButtons(); return; }
  box.innerHTML=groups.map((g,i)=>{
    const tip=g.parts.map(p=>p.base+" ("+fmtSize(p.size)+")").join("\n");
    return `<label class="sessrow" title="${esc(tip)}"><input type="checkbox" data-i="${i}"${selected.has(g.key)?" checked":""}>`+
      `<span class="sessmain"><span class="sesst">${esc(groupNames(g))}</span>`+
      (groupTitle(g) ? `<span class="sessdate">${esc(groupTitle(g))}</span>` : "")+
      `<span class="sesssub">${esc(groupSub(g))}</span></span></label>`;
  }).join("");
  box.querySelectorAll("input[type=checkbox]").forEach(cb=>cb.addEventListener("change", ()=>{
    const g=groups[+cb.dataset.i]; if(!g) return;
    if(cb.checked) selected.add(g.key); else selected.delete(g.key);
    updateButtons();
  }));
  updateButtons();
}
export function selectAllSessions(on){ selected.clear(); if(on) groups.forEach(g=>selected.add(g.key)); renderList(); }
function selGroups(){ return groups.filter(g=>selected.has(g.key)); }
export function updateButtons(){
  const n=selGroups().length, ok=state.connected && n>0 && !busy;
  ["sessDlBtn","sessDlfBtn","sessMapBtn","sessDelBtn"].forEach(id=>$(id).disabled=!ok);
  $("sessUploadBtn").disabled = !(ok && isPaired());
  $("sessAll").checked = n>0 && n===groups.length;
  const files=selGroups().reduce((s,g)=>s+g.parts.length,0);
  $("sessSelInfo").textContent = n ? n+" scan(s) · "+files+" file(s) · "+fmtSize(selGroups().reduce((s,g)=>s+g.size,0)) : "";
}

// Fetch every part of the given groups (one BLE request at a time), joined per group.
// cmdFor(part) -> the CMD for that part (default "F:<path>" = the whole file).
async function fetchGroups(gs, cmdFor){
  const total=gs.reduce((s,g)=>s+g.parts.length,0);
  let k=0; const texts=[];
  for(const g of gs){
    const pt=[];
    for(const p of g.parts){
      k++;
      toast("Fetching "+(total>1?"file "+k+"/"+total+" — ":"")+p.base+"…", {work:true, sticky:true});
      pt.push(await bleFetch(cmdFor ? cmdFor(p) : "F:"+p.path, p.base));
    }
    texts.push(joinLogParts(pt));
  }
  return texts;
}
async function runBusy(what, fn){
  if(busy) return;
  if(!state.connected || !state.chars.cmd){ toast("Connect to the scanner first.", {err:true}); return; }
  busy=true; updateButtons();
  try{ await fn(); }
  catch(e){ log(what+" failed: "+e.message); toast(what+" failed: "+e.message, {err:true}); }
  finally{ busy=false; updateButtons(); }
}
function needSel(){
  const gs=selGroups();
  if(!gs.length){ toast("Tick one or more scans in Session logs first.", {err:true}); return null; }
  return gs.slice().sort((a,b)=>(a.start||0)-(b.start||0));   // joined output in time order
}

export function downloadSession(){
  const gs=needSel(); if(!gs) return;
  runBusy("Download", async()=>{
    const text=joinLogParts(await fetchGroups(gs));
    if(!text){ toast("Nothing to download — those sessions are empty.", {err:true}); return; }
    saveBlob(new Blob([text], {type:"application/octet-stream"}), groupFileName(gs));
  });
}
// Selected scans, filtered to the Data page's time range ON the device (T:<mode>[:min]:<path> per file).
export function downloadSessionRange(){
  const gs=needSel(); if(!gs) return;
  const sp=exportSpec(); if(!sp) return;
  runBusy("Filtered download", async()=>{
    const text=joinLogParts(await fetchGroups(gs, p=>exportCmd(sp, p.path)));
    if(!text){ toast("No rows in that time range.", {err:true}); return; }
    saveBlob(new Blob([text], {type:"application/octet-stream"}), rangeFileName(text, groupFileName(gs, sp.tag)));
  });
}
export function mapSelectedSession(){
  const gs=needSel(); if(!gs) return;
  openMap();
  runBusy("Map", async()=>{
    $("mapMsg").textContent="Loading "+gs.length+" scan(s)…";
    const text=joinLogParts(await fetchGroups(gs));
    toast("Loaded "+groupFileName(gs)+".");
    plotCsvText(text, groupFileName(gs));
  });
}
// One upload per selected scan (its parts joined), so the server gets one session per scan.
export function uploadSelectedSessions(){
  if(!isPaired()){ toast("Not paired — pair a server in Settings.", {err:true}); return; }
  const gs=needSel(); if(!gs) return;
  runBusy("Upload", async()=>{
    for(const g of gs){
      const [text]=await fetchGroups([g]);
      await uploadSessionText(text, groupFileName([g]));
    }
  });
}
export function deleteSession(){
  const gs=needSel(); if(!gs) return;
  const files=gs.flatMap(g=>g.parts);
  if(!confirm("Delete "+gs.length+" scan(s) ("+files.length+" file(s))?\nThis cannot be undone. (The in-progress session is kept.)")) return;
  runBusy("Delete", async()=>{
    let k=0;
    for(const p of files){   // each D: answers with a fresh session list; await it before the next
      toast("Deleting "+(++k)+"/"+files.length+"…", {work:true, sticky:true});
      await bleFetch("D:"+p.path, p.base);
    }
    gs.forEach(g=>selected.delete(g.key));
    toast("Deleted "+files.length+" file(s).");
  }).then(renderList);
}
// Wipe every session log (CMD "D:*"); the device keeps the live in-progress session.
export async function wipeAllLogs(){
  if(!state.chars.cmd) return;
  if(!confirm("Wipe ALL session logs on the SD card?\nThis cannot be undone. The current in-progress session is kept.")) return;
  try{ await state.chars.cmd.writeValue(new TextEncoder().encode("D:*")); log("Wiping all logs… (list will refresh)"); }
  catch(e){ log("Wipe failed: "+e.message); }
}
// Explore picker: load the chosen scan (all its parts, joined) into the Scan Viewer.
export function exploreSelected(){
  const g=groups.find(x=>x.key===$("expSel").value) || groups[0];
  if(!g || !exploreLoader) return;
  const name=groupFileName([g]);
  $("expMsg").textContent="Loading "+name+"…";
  runBusy("Explore", async()=>{
    const [text]=await fetchGroups([g]);
    toast("Loaded "+name+".");
    exploreLoader(text, name);
  });
}
