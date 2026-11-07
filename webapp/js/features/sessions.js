// Session-log management: list ("Q" -> SESS= stream), download, map, upload, delete, wipe.
// Files are grouped into SCANS: one scan is normally one file, but older firmware split a busy drive
// into many parts (and New Session / reboots split a day), so consecutive files of the same stream
// (fg / bg / fleet) whose dated spans sit within GROUP_GAP_S of each other form one group. The Session-logs
// list is a multi-select of groups; every action fetches the selected groups' parts one at a time
// over BLE (bleFetch) and joins them (joinLogParts) into ONE file / ONE upload per scan.
// A fleet LEADER also has /logs/fleet-*.jsonl (the members' lines, forwarded to it): its own "Fleet" kind of scan,
// grouped like the others (docs/fleet-mode.md §22). It is listed next to the leader's own scans, not merged into them.
// Ticking a leader's own scan (fg / bg) automatically INCLUDES its time-overlapping Fleet group (same device, so the
// spans alone decide): Download / Upload / Map act on both, the Fleet row shows "included". The fleet file is never
// joined into the leader's file (a leader file must not carry member lines): Download saves it as a second file,
// Upload POSTs it as its own body. Delete never auto-includes it.
// The one grouped list feeds both the Session-logs list and the Explore picker (#expSel).
import { state } from '../state.js';
import { $, log, esc, toast, saveBlob } from '../util/dom.js';
import { openMap, plotCsvText } from './map.js';
import { bleFetch } from '../ble/transfer.js';
import { fetchLog, cacheStatus } from '../ble/logfetch.js';
import { onCacheChange } from '../data/logcache.js';
import { onUnitReset } from '../unitreset.js';
import { connGen } from '../ble/xfer.js';
import { joinLogParts } from '../data/parse.js';
import { uploadSessionText, isPaired } from './remote.js';
import { exportSpec, exportCmd, rangeFileName } from './timerange.js';

const FLEET_SLACK_S = 300;         // a Fleet group "overlaps" a scan when their dated spans touch within this
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
    const [path,size,flag,first,last,fid]=r.split("\t");   // fid (fw >= 0.26.0): the file's file_id, "" when it has none
    const base=path.split("/").pop();
    const f=parseInt(first,10)||0, l=parseInt(last,10)||0;
    const start=f||nameEpoch(base), end=Math.max(l, start);
    return {path, base, size:parseInt(size,10)||0, flag:flag||"", fid:/^[0-9a-f]{8}$/.test(fid||"") ? fid : "", start, end,
            kind: base.startsWith("bg-") ? "bg" : base.startsWith("fleet-") ? "fleet" : "fg", csv: /\.csv$/i.test(base)};
  });
  const out=[];
  for(const kind of ["fg","bg","fleet"]){
    const ps=parts.filter(p=>p.kind===kind && p.start && !p.csv).sort((a,b)=>a.start-b.start);
    let g=null;
    for(const p of ps){
      if(g && p.start - g.end <= GROUP_GAP_S){ g.parts.push(p); g.end=Math.max(g.end,p.end); }
      else { g={kind, parts:[p], start:p.start, end:p.end}; out.push(g); }
    }
  }
  // Undated files (pre-sync boots: sess-/bg-/fleet-NNNNN) and legacy CSV: one group each.
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
// Fleet groups that ride along with the selected scans (not already ticked themselves).
function fleetOf(g){
  // The fleet file's END must fall inside the scan (+slack): a carried tail only skews a fleet file's START, so a fleet
  // file that merely touches a neighbouring scan is not pulled in.
  return g.kind==="fleet" || !g.start ? [] : groups.filter(f=>f.kind==="fleet" && f.start && f.start<=g.end+FLEET_SLACK_S && f.end>=g.start-FLEET_SLACK_S && f.end<=g.end+FLEET_SLACK_S);
}
function includedFleet(gs){
  const out=[];
  for(const g of gs) for(const f of fleetOf(g)) if(!gs.includes(f) && !out.includes(f)) out.push(f);
  return out;
}
function groupSub(g){
  const bits=[g.kind==="bg"?"Background":g.kind==="fleet"?"Fleet":"Scan"];
  if(g.parts.length>1) bits.push(g.parts.length+" parts");
  bits.push(fmtSize(g.size));
  if(fleetOf(g).length) bits.push("+ Fleet");
  if(g.flag==="current") bits.push("current");
  else if(g.flag==="latest") bits.push("latest");
  return bits.join(" · ");
}
// Saved-copy tag for a scan's row: "saved" when every part is cached and current, "+N KB new" when a cached
// part grew on the device, "partly saved" when only some parts are cached. "" = nothing cached.
function groupCache(g){
  let full=0, fresh=0, any=0;
  for(const p of g.parts){
    const c=cacheStatus(p.path, p.size, p.fid);
    if(c.st==="full") full++; else if(c.st==="part"){ any++; fresh+=c.fresh; }
  }
  if(fresh) return "+"+fmtSize(fresh)+" new";
  if(full===g.parts.length) return "saved";
  if(!full && !any) return g.parts.some(p=>cacheStatus(p.path, p.size, p.fid).st==="path") ? "older copy saved" : "";   // same name, can't prove it's the same file
  return "partly saved";
}
// Joined file name: a one-part scan keeps its own name; otherwise <first part>+<n>p.jsonl.
function groupFileName(gs, tag){
  const first=gs[0].parts[0].base.replace(/\.(jsonl|csv)$/i,"");
  const ext=(gs.length===1 && gs[0].parts.length===1 && gs[0].parts[0].csv) ? ".csv" : ".jsonl";
  const nParts=gs.reduce((s,g)=>s+g.parts.length,0);
  const many = gs.length>1 ? "+"+(gs.length-1)+"scans" : nParts>1 ? "+"+nParts+"p" : "";
  return first+many+(tag?"-"+tag:"")+ext;
}

// The device reads every log file's head/tail to build the list, so a refresh takes a few seconds on a
// full card: show it's in progress (button, list placeholder, working toast) until the list lands.
const LIST_TIMEOUT_MS = 30000;
let listWait = null;   // timeout handle while a refresh is pending
function endListWait(ok){
  if(!listWait) return;
  clearTimeout(listWait); listWait=null;
  const b=$("listBtn"); b.textContent="Refresh session list"; b.disabled=!state.connected;
  if(!ok){ toast("Session list didn't arrive — try again.", {err:true}); renderList(); }
}
export async function refreshSessions(){
  if(listWait) return;   // already waiting
  const b=$("listBtn"); b.disabled=true; b.textContent="Refreshing…";
  $("sessList").innerHTML='<div class="sub">Loading session list from the device…</div>';
  toast("Loading session list…", {work:true, sticky:true});
  listWait=setTimeout(()=>endListWait(false), LIST_TIMEOUT_MS);
  try{ await state.chars.cmd.writeValue(new TextEncoder().encode("Q")); log("Requesting session list…"); }
  catch(e){ log("List failed: "+e.message); endListWait(true); toast("List failed: "+e.message, {err:true}); renderList(); }
}
export function populateSessions(text){
  const wasRefresh = !!listWait;
  endListWait(true);
  groups=buildGroups(text);
  if(wasRefresh){   // replace the sticky "Loading…" toast with the result
    const nf=groups.reduce((s,g)=>s+g.parts.length,0);
    toast(groups.length ? groups.length+" scan(s), "+nf+" file(s)." : "No sessions on the device.");
  }
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
  const inc=includedFleet(selGroups());
  box.innerHTML=groups.map((g,i)=>{
    const tip=g.parts.map(p=>p.base+" ("+fmtSize(p.size)+")").join("\n");
    return `<label class="sessrow" title="${esc(tip)}"><input type="checkbox" data-i="${i}"${selected.has(g.key)?" checked":""}>`+
      `<span class="sessmain"><span class="sesst">${esc(groupNames(g))}</span>`+
      (groupTitle(g) ? `<span class="sessdate">${esc(groupTitle(g))}</span>` : "")+
      `<span class="sesssub">${esc(groupSub(g))}${groupCache(g) ? ` · <span class="sesscache">${esc(groupCache(g))}</span>` : ""}${inc.includes(g) ? ` · <span class="sesscache">included with the ticked scan</span>` : ""}</span></span></label>`;
  }).join("");
  box.querySelectorAll("input[type=checkbox]").forEach(cb=>cb.addEventListener("change", ()=>{
    const g=groups[+cb.dataset.i]; if(!g) return;
    if(cb.checked) selected.add(g.key); else selected.delete(g.key);
    renderList();   // refreshes the "included" tags (also updates the buttons)
  }));
  updateButtons();
}
onUnitReset(()=>{   // a different scanner is about to connect: its session list is not this one's
  endListWait(true);
  groups=[]; selected.clear(); state.exploreWant=false;
  $("sessList").innerHTML='<div class="sub">\u2014 connect and refresh \u2014</div>';
  $("expSel").innerHTML="<option>\u2014 no sessions \u2014</option>";
  updateButtons();
});
onCacheChange(()=>{ if(groups.length) renderList(); });   // a fetch finished / the cache was cleared: refresh the saved tags
export function selectAllSessions(on){ selected.clear(); if(on) groups.forEach(g=>selected.add(g.key)); renderList(); }
function selGroups(){ return groups.filter(g=>selected.has(g.key)); }
export function updateButtons(){
  const n=selGroups().length, ok=state.connected && n>0 && !busy;
  ["sessDlBtn","sessDlfBtn","sessMapBtn","sessDelBtn"].forEach(id=>$(id).disabled=!ok);
  $("sessUploadBtn").disabled = !(ok && isPaired());
  $("sessAll").checked = n>0 && n===groups.length;
  const all=selGroups().concat(includedFleet(selGroups()));
  const files=all.reduce((s,g)=>s+g.parts.length,0);
  const nf=all.length-n;
  $("sessSelInfo").textContent = n ? n+" scan(s)"+(nf?" + "+nf+" fleet":"")+" · "+files+" file(s) · "+fmtSize(all.reduce((s,g)=>s+g.size,0)) : "";
}

// Fetch every part of the given groups (one BLE request at a time), joined per group.
// cmdFor(part) -> a custom CMD for that part (filtered exports; fetched uncached). Default = the whole file
// through the phone-side cache (ble/logfetch.js): a saved copy that is still current costs no transfer.
// opts.trim: uploads drop a torn last row. opts.ctx = the action's {gen, sid} captured ONCE at its start (actionCtx): if the
// scanner changes (Switch scanner, reconnect) the whole multi-step action aborts instead of joining two scanners' parts.
const actionCtx = ()=>({gen:connGen(), sid:state.scannerId});
async function fetchGroups(gs, cmdFor, opts){
  const total=gs.reduce((s,g)=>s+g.parts.length,0);
  const {gen, sid}=(opts && opts.ctx) || actionCtx();
  const alive=()=>{ if(connGen()!==gen || (sid && state.scannerId!==sid)) throw new Error("the scanner changed during the transfer"); };
  let k=0; const texts=[];
  for(const g of gs){
    const pt=[];
    for(const p of g.parts){
      k++;
      const saved = !cmdFor && cacheStatus(p.path, p.size, p.fid).st==="full";
      toast((saved?"Loading saved ":"Fetching ")+(total>1?"file "+k+"/"+total+" — ":"")+p.base+"…", {work:true, sticky:true});
      alive();
      pt.push(cmdFor ? await bleFetch(cmdFor(p), p.base) : (await fetchLog(p.path, {size:p.size, fileId:p.fid, name:p.base, gen, sid, trim:!!(opts && opts.trim)})).text);
      alive();
    }
    texts.push(joinLogParts(pt));
  }
  return texts;
}
async function runBusy(what, fn){
  if(busy) return;
  if(!state.connected || !state.chars.cmd){ toast("Connect to the scanner first.", {err:true}); return; }
  busy=true; updateButtons();
  state.xferBusy=(state.xferBusy||0)+1;   // presence watching (features/switcher.js) pauses during transfers
  try{ await fn(); }
  catch(e){ log(what+" failed: "+e.message); toast(what+" failed: "+e.message, {err:true}); }
  finally{ busy=false; state.xferBusy--; updateButtons(); }
}
function needSel(){
  const gs=selGroups();
  if(!gs.length){ toast("Tick one or more scans in Session logs first.", {err:true}); return null; }
  return gs.slice().sort((a,b)=>(a.start||0)-(b.start||0));   // joined output in time order
}

// Split a selection into the leader/own scans and the fleet groups (ticked or auto-included), each fleet group
// fetched + saved/uploaded as its OWN file.
function splitFleet(gs){
  return {own: gs.filter(g=>g.kind!=="fleet"), fleet: gs.filter(g=>g.kind==="fleet").concat(includedFleet(gs))};
}
const pause = ms=>new Promise(r=>setTimeout(r, ms));
// Save the own scans joined as ONE file, then each fleet group as a SEPARATE second download (no zip: the page has
// no zip library and a leader file must stay free of member lines). Chrome may ask once to allow multiple downloads.
async function saveSplit(gs, cmdFor, tag, empty){
  const {own, fleet}=splitFleet(gs);
  const ctx=actionCtx();   // one guard for the whole action (scan + fleet files)
  let saved=0;
  if(own.length){
    const text=joinLogParts(await fetchGroups(own, cmdFor, {ctx}));
    if(text){ saveBlob(new Blob([text], {type:"application/octet-stream"}), tag===undefined ? groupFileName(own) : rangeFileName(text, groupFileName(own, tag))); saved++; }
  }
  for(const f of fleet){
    const text=joinLogParts(await fetchGroups([f], cmdFor, {ctx}));
    if(!text) continue;
    if(saved) await pause(400);
    saveBlob(new Blob([text], {type:"application/octet-stream"}), tag===undefined ? groupFileName([f]) : rangeFileName(text, groupFileName([f], tag)));
    saved++;
  }
  if(!saved) toast(empty, {err:true});
  else if(fleet.length && own.length) toast("Saved the scan + its Fleet file as separate downloads.");
}
// "Download / Upload current session" on a leader: also handle the Fleet file(s) of that session. Matched against the
// current FILE's span, not its scan group: a group can span a whole day of boots, which would drag in every older fleet
// file. Overlap only, so the previous boot's fleet file (which ends before this file starts) stays out.
// State hooks, not exports: a stale cached copy of either module then just skips this, where a missing export would
// stop the whole page loading.
async function currentFleet(sess){
  toast("Looking for this session's Fleet file…", {work:true, sticky:true});
  await bleFetch("Q", "sessions");   // download.js refreshes `groups` before this resolves
  const base=sess.replace(/\.jsonl$/, "");
  const part=groups.flatMap(g=>g.kind==="fleet" ? [] : g.parts).find(p=>p.base.replace(/\.jsonl$/, "")===base);
  if(!part) return [];
  return groups.filter(f=>f.kind==="fleet").map(f=>{
    const parts=f.parts.filter(p=>p.end>part.start && p.start<=part.end+FLEET_SLACK_S);
    return parts.length ? {...f, parts, start:parts[0].start, end:parts[parts.length-1].end} : null;
  }).filter(Boolean);
}
state.uploadCurrentFleet = sess => runBusy("Fleet upload", async ()=>{
  if(state.fleetRole!=="leader") return;
  const ctx=actionCtx(), fleet=await currentFleet(sess);
  if(!fleet.length){ toast("Uploaded the scan. It has no Fleet file."); return; }
  let bad=false;
  for(const f of fleet){   // its OWN body, as on the Session-logs list
    const [text]=await fetchGroups([f], null, {trim:true, ctx});
    const r=await uploadSessionText(text, groupFileName([f]));
    if(r!==true) bad=true;
    if(r==="auth") break;   // a revoked key fails every body: stop
  }
  toast(bad ? "Uploaded the scan, but its Fleet file failed." : "Uploaded the scan + its Fleet file.", {err:bad});
});
state.saveCurrentFleet = sess => runBusy("Fleet download", async ()=>{
  if(state.fleetRole!=="leader") return;
  const ctx=actionCtx(), fleet=await currentFleet(sess);
  if(!fleet.length){ toast("Saved the scan. It has no Fleet file."); return; }
  for(const f of fleet){
    const text=joinLogParts(await fetchGroups([f], null, {ctx}));
    if(!text) continue;
    await pause(400);
    saveBlob(new Blob([text], {type:"application/octet-stream"}), groupFileName([f]));
  }
  toast("Saved the scan + its Fleet file as separate downloads.");
});
export function downloadSession(){
  const gs=needSel(); if(!gs) return;
  runBusy("Download", ()=>saveSplit(gs, null, undefined, "Nothing to download — those sessions are empty."));
}
// Selected scans, filtered to the Data page's time range ON the device (T:<mode>[:min]:<path> per file).
export function downloadSessionRange(){
  const gs=needSel(); if(!gs) return;
  const sp=exportSpec(); if(!sp) return;
  runBusy("Filtered download", async()=>{
    await saveSplit(gs, p=>exportCmd(sp, p.path), sp.tag, "No rows in that time range.");
  });
}
export function mapSelectedSession(){
  const gs=needSel(); if(!gs) return;
  openMap();
  const all=gs.concat(includedFleet(gs)).sort((a,b)=>(a.start||0)-(b.start||0));   // plot the scan AND its fleet file
  runBusy("Map", async()=>{
    $("mapMsg").textContent="Loading "+all.length+" scan(s)…";
    const text=joinLogParts(await fetchGroups(all, null, {ctx:actionCtx()}));
    toast("Loaded "+groupFileName(all)+".");
    plotCsvText(text, groupFileName(all));
  });
}
// One upload per selected scan (its parts joined), so the server gets one session per scan.
export function uploadSelectedSessions(){
  if(!isPaired()){ toast("Not paired — pair a server in Settings.", {err:true}); return; }
  const gs=needSel(); if(!gs) return;
  runBusy("Upload", async()=>{
    const ctx=actionCtx(), list=gs.concat(includedFleet(gs)), res=[];   // the Fleet file is its OWN body, after the scan
    for(const g of list){
      const [text]=await fetchGroups([g], null, {trim:true, ctx});
      const r=await uploadSessionText(text, groupFileName([g]));
      res.push({g, r});
      if(r==="auth") break;   // a revoked key fails every body: stop
    }
    if(list.length>1){   // one closing summary (each body already toasted its own result)
      const what=x=>(x.g.kind==="fleet"?"fleet":"scan")+(x.r===true?" uploaded":x.r==="auth"?" rejected (re-pair)":" failed");
      const parts=res.map(what).concat(list.slice(res.length).map(g=>(g.kind==="fleet"?"fleet":"scan")+" skipped"));
      toast("Upload: "+parts.join(", ")+".", {err:res.length<list.length || res.some(x=>x.r!==true)});
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
