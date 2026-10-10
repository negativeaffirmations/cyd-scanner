// BLE download stream (LOGDATA) reassembly + the download commands.
// A short header notification announces the payload ("SIZE=<n>" = a file, "SESS=<n>" = the session
// list, "WL=<n>" = the whitelist), then <n> bytes of chunks follow.
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, log } from '../util/dom.js';
import { parseScanLog } from '../data/parse.js';
import { plotCsvText } from '../features/map.js';
import { loadedExplore } from '../features/explore.js';
import { populateSessions } from '../features/sessions.js';
import { populateWhitelist } from '../features/whitelist.js';

export function onLogData(ev){
  const dv = ev.target.value;
  const v = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  if (state.dl.mode === null) {               // expecting a header
    const txt = new TextDecoder().decode(v);
    if (txt.startsWith("SIZE=")) {
      state.dl.mode="file"; state.dl.expect=parseInt(txt.slice(5),10)||0; state.dl.got=0; state.dl.chunks=[];
      if (state.dl.expect===0){ log(state.dl.sink==="map"||state.dl.sink==="explore"?"That session is empty.":"No log on device yet (or no rows in that time range).");
        if(state.dl.sink==="explore") loadedExplore([], state.dl.name); state.dl.mode=null; }
      return;
    }
    if (txt.startsWith("SESS=")) {
      state.dl.mode="list"; state.dl.expect=parseInt(txt.slice(5),10)||0; state.dl.got=0; state.dl.chunks=[];
      if (state.dl.expect===0){ populateSessions(""); state.dl.mode=null; }
      return;
    }
    if (txt.startsWith("WL=")) {
      state.dl.mode="wl"; state.dl.expect=parseInt(txt.slice(3),10)||0; state.dl.got=0; state.dl.chunks=[];
      if (state.dl.expect===0){ populateWhitelist(""); state.dl.mode=null; }
      return;
    }
    return;                                   // ignore stray data
  }
  state.dl.chunks.push(new Uint8Array(v)); state.dl.got += v.length;
  if (state.dl.got >= state.dl.expect) {
    const blob = new Blob(state.dl.chunks);
    if (state.dl.mode==="file") {
      if (state.dl.sink==="map") { blob.text().then(t=>plotCsvText(t, state.dl.name)); }
      else if (state.dl.sink==="explore") { const nm=state.dl.name; blob.text().then(t=>loadedExplore(parseScanLog(t), nm)); }
      else {
        const a=document.createElement("a");
        a.href=URL.createObjectURL(blob); a.download=state.dl.name; a.click(); URL.revokeObjectURL(a.href);
        log("Downloaded "+state.dl.name+" ("+state.dl.got+" bytes).");
      }
    } else if (state.dl.mode==="list") {
      blob.text().then(populateSessions);
    } else if (state.dl.mode==="wl") {
      blob.text().then(populateWhitelist);
    }
    state.dl = { mode:null, expect:0, got:0, chunks:[], name:"scanlog.jsonl", sink:"save" };
  }
}
export async function downloadLog(){
  try{ state.dl={mode:null,expect:0,got:0,chunks:[],name:"scanlog.jsonl",sink:"save"};
    await state.chars.cmd.writeValue(enc.encode("L")); log("Requesting current session…"); }
  catch(e){ log("Download failed: "+e.message); }
}
// --- Time-filtered export. The CYD filters on the device and streams only the matching lines:
// BLE = CMD "T:<mode>[:<minutes>][:<path>]" (mode 0 whole file / 1 past 24 h / 2 past <minutes>;
// path optional, default = current session), Wi-Fi = http://192.168.4.1/dlf?f=<name|current>&mode=&arg=.
export function exportSpec(){
  const m=$("expRange").value;
  if(m==="2"){
    const mins=Math.round((+$("expD").value||0)*1440+(+$("expH").value||0)*60+(+$("expM").value||0));
    if(mins<1){ log("Custom range: enter at least 1 minute."); return null; }
    const a=Math.min(mins,525600);
    return {mode:2, arg:a, tag:"last"+a+"min"};
  }
  return m==="1" ? {mode:1, arg:0, tag:"last24h"} : {mode:0, arg:0, tag:"all"};
}
export async function downloadFiltered(path){   // path "" = the device's current session
  const sp=exportSpec(); if(!sp) return;
  const cmd="T:"+sp.mode+(sp.mode===2?":"+sp.arg:"")+(path?":"+path:"");
  const base=path ? path.split("/").pop().replace(/\.jsonl$/,"")
                  : ($("sessName").textContent.replace(/^Session:\s*/,"").trim()||"scanlog");
  try{ state.dl={mode:null,expect:0,got:0,chunks:[],name:base+"-"+sp.tag+".jsonl",sink:"save"};
    await state.chars.cmd.writeValue(enc.encode(cmd)); log("Requesting "+state.dl.name+" (filtered on device)…"); }
  catch(e){ log("Filtered download failed: "+e.message); }
}
export function downloadFilteredWifi(){
  const sp=exportSpec(); if(!sp) return;
  const sel=$("sessSel").value, useSel=sel && !sel.startsWith("—");
  const f=useSel ? sel.split("/").pop() : "current";
  const url="http://192.168.4.1/dlf?f="+encodeURIComponent(f)+"&mode="+sp.mode+(sp.mode===2?"&arg="+sp.arg:"");
  log("Opening "+url+" — join the device Wi-Fi first (Wi-Fi download mode). "+(useSel?"(selected session)":"(current session)"));
  window.open(url,"_blank");
}
