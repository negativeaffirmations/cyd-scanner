// BLE download stream (LOGDATA) reassembly + the download commands.
// A short header notification announces the payload ("SIZE=<n>" = a file, "SESS=<n>" = the session
// list, "WL=<n>" = the whitelist), then <n> bytes of chunks follow.
// sink "promise" = a queued request from ble/transfer.js bleFetch(): any payload resolves its promise.
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, log, toast, saveBlob } from '../util/dom.js';
import { parseScanLog } from '../data/parse.js';
import { plotCsvText } from '../features/map.js';
import { loadedExplore } from '../features/explore.js';
import { populateSessions } from '../features/sessions.js';
import { populateWhitelist } from '../features/whitelist.js';
import { uploadSessionText } from '../features/remote.js';
import { exportSpec, exportCmd } from '../features/timerange.js';
import { resetDl } from './transfer.js';

// Idle watchdog for the one-shot sinks (save/map/explore/upload): if a transfer stalls mid-file (link
// hiccup, a short send), give up after IDLE_MS instead of leaving state.dl stuck in "file" mode, which
// would swallow every later SESS=/SIZE= header as file data. (bleFetch has its own watchdog.)
const IDLE_MS = 20000;
setInterval(()=>{
  const d = state.dl;
  if (d.mode===null || d.sink==="promise" || !d.lastRx || Date.now()-d.lastRx < IDLE_MS) return;
  log("Transfer stalled ("+d.got+" of "+d.expect+" bytes) — cancelled. Try again.");
  toast("Transfer stalled — cancelled. Try again.", {err:true});
  if (d.sink==="upload") uploadSessionText("", d.name);   // clears the upload busy state
  resetDl();
}, 2000);

// A queued (bleFetch) request finished: hand its text to the waiting promise and free the slot.
function settlePromise(text){
  const res = state.dl.resolve;
  resetDl();
  if(res) res(text);
}

export function onLogData(ev){
  const dv = ev.target.value;
  const v = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  state.dl.lastRx = Date.now();               // bleFetch idle watchdog
  const isPromise = state.dl.sink==="promise";
  if (state.dl.mode === null) {               // expecting a header
    const txt = new TextDecoder().decode(v);
    if (txt.startsWith("SIZE=")) {
      state.dl.mode="file"; state.dl.expect=parseInt(txt.slice(5),10)||0; state.dl.got=0; state.dl.chunks=[];
      if (state.dl.expect===0){
        if(isPromise){ settlePromise(""); return; }
        log(state.dl.sink==="map"||state.dl.sink==="explore"?"That session is empty.":"No log on device yet (or no rows in that time range).");
        if(state.dl.sink==="explore") loadedExplore([], state.dl.name);
        if(state.dl.sink==="upload") uploadSessionText("", state.dl.name);   // report empty + clear the upload busy state
        if(state.dl.sink==="save") toast("Nothing to download — no rows in that range.", {err:true});
        state.dl.mode=null; }
      return;
    }
    if (txt.startsWith("SESS=")) {
      state.dl.mode="list"; state.dl.expect=parseInt(txt.slice(5),10)||0; state.dl.got=0; state.dl.chunks=[];
      if (state.dl.expect===0){ populateSessions(""); if(isPromise) settlePromise(""); else state.dl.mode=null; }
      return;
    }
    if (txt.startsWith("WL=")) {
      state.dl.mode="wl"; state.dl.expect=parseInt(txt.slice(3),10)||0; state.dl.got=0; state.dl.chunks=[];
      if (state.dl.expect===0){ populateWhitelist(""); if(isPromise) settlePromise(""); else state.dl.mode=null; }
      return;
    }
    return;                                   // ignore stray data
  }
  state.dl.chunks.push(new Uint8Array(v)); state.dl.got += v.length;
  if (isPromise && state.dl.mode==="file" && state.dl.progress) state.dl.progress(state.dl.got, state.dl.expect);
  if (state.dl.got >= state.dl.expect) {
    // octet-stream, not the default empty/text type: otherwise Android Chrome sniffs the content as
    // text and appends ".txt" to the .jsonl download name. octet-stream = "save the bytes as named".
    const blob = new Blob(state.dl.chunks, { type: "application/octet-stream" });
    const mode = state.dl.mode;
    if (isPromise) {
      const res = state.dl.resolve;
      resetDl();
      blob.text().then(t=>{ if(mode==="list") populateSessions(t); else if(mode==="wl") populateWhitelist(t); if(res) res(t); });
      return;
    }
    if (mode==="file") {
      if (state.dl.sink==="map") { blob.text().then(t=>plotCsvText(t, state.dl.name)); }
      else if (state.dl.sink==="explore") { const nm=state.dl.name; blob.text().then(t=>loadedExplore(parseScanLog(t), nm)); }
      else if (state.dl.sink==="upload") { const nm=state.dl.name; blob.text().then(t=>uploadSessionText(t, nm)); }
      else saveBlob(blob, state.dl.name);
    } else if (mode==="list") {
      blob.text().then(populateSessions);
    } else if (mode==="wl") {
      blob.text().then(populateWhitelist);
    }
    resetDl();
  }
}
export async function downloadLog(){
  // Name the saved file after the device's CURRENT session (STATUS `sess=`, shown in the label),
  // which is the timestamped name (YYYYMMDD-HHMMSS) once the phone has synced time — not a fixed
  // "scanlog.jsonl". Falls back to scanlog.jsonl before any session name is known.
  const sess=($("sessName").textContent.replace(/^Session:\s*/,"").trim())||"scanlog";
  const fname=sess.endsWith(".jsonl")?sess:sess+".jsonl";
  try{ state.dl={mode:null,expect:0,got:0,chunks:[],name:fname,sink:"save"};
    toast("Fetching "+fname+" over BLE…", {work:true, sticky:true});
    await state.chars.cmd.writeValue(enc.encode("L")); log("Requesting current session ("+fname+")…"); }
  catch(e){ log("Download failed: "+e.message); toast("Download failed: "+e.message, {err:true}); }
}
// --- Time-filtered export of ALL logs. The CYD filters on the device across EVERY session file
// (fg + bg, New Session, earlier boots), dates pre-sync rows, and streams one joined NDJSON:
// BLE = CMD "T:<mode>[:<minutes>]:*", Wi-Fi = http://192.168.4.1/dlf?f=all&mode=&arg=.
// (A single selected session's range export lives in sessions.js.)
export async function downloadFilteredAll(){
  const sp=exportSpec(); if(!sp) return;
  try{ state.dl={mode:null,expect:0,got:0,chunks:[],name:"all-"+sp.tag+".jsonl",sink:"save"};
    toast("Collecting "+sp.tag+" from all logs on the device…", {work:true, sticky:true});
    await state.chars.cmd.writeValue(enc.encode(exportCmd(sp, "*"))); log("Requesting "+state.dl.name+" (all logs, filtered on device)…"); }
  catch(e){ log("Filtered download failed: "+e.message); toast("Filtered download failed: "+e.message, {err:true}); }
}
export function downloadFilteredWifi(){
  const sp=exportSpec(); if(!sp) return;
  const url="http://192.168.4.1/dlf?f=all&mode="+sp.mode+(sp.mode===2?"&arg="+sp.arg:"");
  log("Opening "+url+" — join the device Wi-Fi first (Wi-Fi download mode). (all logs)");
  window.open(url,"_blank");
}
