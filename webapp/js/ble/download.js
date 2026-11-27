// BLE download stream (LOGDATA) reassembly + the download commands.
// A short header notification announces the payload ("SIZE=<n>" = a file, "SESS=<n>" = the session
// list, "WL=<n>" = the whitelist), then <n> bytes of chunks follow.
// sink "promise" = a queued request from ble/transfer.js bleFetch(): any payload resolves its promise.
// With state.dl.raw (ble/logfetch.js) a file resolves as {kind:"file", n, blob} (or "tail" with off) instead of text.
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, log, toast, saveBlob } from '../util/dom.js';
import { parseScanLog } from '../data/parse.js';
import { plotCsvText } from '../features/map.js';
import { loadedExplore } from '../features/explore.js';
import { populateSessions } from '../features/sessions.js';
import { populateWhitelist } from '../features/whitelist.js';
import { uploadSessionText } from '../features/remote.js';
import { exportSpec, exportCmd, rangeFileName } from '../features/timerange.js';
import { resetDl } from './transfer.js';
import { fetchCurrent } from './logfetch.js';
import { reqBusy, connGen } from './xfer.js';
import { syncWait } from './syncwait.js';
import { gattWrite } from './gattq.js';

// Progress toast for the one-shot file sinks (2026-11-26: a 12.9 MB all-logs export showed "Collecting…" for 14 min).
// Before the SIZE header the scanner is still collecting (a "WAIT" notify every 5 s): elapsed time, when the request
// set state.dl.prep. Then bytes, percent and the time left, at most once a second. Small files get no progress.
const PROG_MIN = 64 * 1024;
const mb = b=>b >= 1048576 ? (b / 1048576).toFixed(1) + " MB" : Math.round(b / 1024) + " KB";
const mmss = ms=>{ const s = Math.max(0, Math.round(ms / 1000)); return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); };
// Idle watchdog for the one-shot sinks (save/map/explore/upload): if a transfer stalls mid-file (link
// hiccup, a short send), give up after IDLE_MS instead of leaving state.dl stuck in "file" mode, which
// would swallow every later SESS=/SIZE= header as file data. (bleFetch has its own watchdog.)
// d.fileIdleMs (the all-logs export) replaces IDLE_MS once the file is coming: its send pass filters every fleet file on
// the card and can go minutes without a matching row, and nothing can be sent in between (every byte is file data). A
// lost link ends it at once instead (2026-11-26: a "since boot" export was cancelled 20 s into a quiet stretch).
const IDLE_MS = 20000, QUIET_SAY_MS = 8000;
setInterval(()=>{
  const d = state.dl, now = Date.now();
  if ((d.mode===null && !d.waiting) || d.sink==="promise" || !d.lastRx) return;
  const lim = d.mode==="file" && d.fileIdleMs && state.connected ? d.fileIdleMs : IDLE_MS;
  if (state.connected && now-d.lastRx < lim) {   // no link: cancel now (nothing else clears a one-shot download)
    if (d.mode==="file" && d.fileIdleMs && d.expect >= PROG_MIN && now-d.lastRx >= QUIET_SAY_MS)
      toast((d.label || "Downloading " + d.name) + ": " + mb(d.got) + " of " + mb(d.expect) + " (" + Math.floor(100 * d.got / d.expect) +
            "%). The scanner is still filtering older logs (" + mmss(now - d.lastRx) + " without new rows).", {work:true, sticky:true});
    return;
  }
  if (d.holdUntil && Date.now() < d.holdUntil) return;   // a claimed list reply (ble/syncwait.js) may take a while
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

function progress(d, now){
  if(now - (d.progAt || 0) < 1000) return;
  d.progAt = now;
  if(d.mode === null){
    if(d.prep) toast(d.prep + " (" + mmss(now - (d.t0 || now)) + ")", {work:true, sticky:true});
    return;
  }
  if(d.expect < PROG_MIN) return;
  const el = now - (d.sizeAt || now), pct = Math.floor(100 * d.got / d.expect);
  const left = d.got >= Math.max(32768, d.expect / 20) && el > 5000 ? (d.expect - d.got) * el / d.got : 0;   // too early = wild
  toast((d.label || "Downloading " + d.name) + ": " + mb(d.got) + " of " + mb(d.expect) + " (" + pct + "%)" +
        (left ? ", about " + (left < 60000 ? Math.max(1, Math.round(left / 1000)) + " s" : Math.round(left / 60000) + " min") + " left" : ""),
        {work:true, sticky:true});
}

export function onLogData(ev){
  const dv = ev.target.value;
  const v = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  // fw >= 0.44.0 live-log push (features/livesync.js): "LL=<file_id>:<offset>:<bytes>", "LLOK=<n>", "LLERR=<why>". Single
  // notifies sent only between replies, so only looked for outside a payload (mode null); never touches state.dl.
  if (state.dl.mode === null && v.length >= 5 && v[0] === 76 && v[1] === 76 && state.onLiveLog &&
      (v[2] === 61 || (v[2] === 79 && v[3] === 75) || (v[2] === 69 && v[3] === 82))) {   // "LL=" / "LLOK" / "LLER"
    state.onLiveLog(v);
    return;
  }
  state.dl.lastRx = Date.now();               // bleFetch idle watchdog
  const isPromise = state.dl.sink==="promise";
  if (state.dl.mode === null) {               // expecting a header
    const txt = new TextDecoder().decode(v);
    if (!isPromise && txt === "WAIT") { progress(state.dl, Date.now()); return; }   // still collecting on the scanner
    if (txt.startsWith("SIZE=")) {
      state.dl.mode="file"; state.dl.expect=parseInt(txt.slice(5),10)||0; state.dl.got=0; state.dl.chunks=[]; state.dl.sizeAt=Date.now();
      if (state.dl.expect===0){
        if(isPromise){ settlePromise(""); return; }
        log(state.dl.sink==="map"||state.dl.sink==="explore"?"That session is empty.":"No log on device yet (or no rows in that time range).");
        if(state.dl.sink==="explore") loadedExplore([], state.dl.name);
        if(state.dl.sink==="upload") uploadSessionText("", state.dl.name);   // report empty + clear the upload busy state
        if(state.dl.sink==="save") toast("Nothing to download — no rows in that range.", {err:true});
        state.dl.mode=null; state.dl.waiting=false; }
      return;
    }
    // fw >= 0.26.0 tail form "F:<path>@<offset>:<file_id>" (ble/logfetch.js, raw requests only):
    // "TAIL=<n>@<offset>" + n raw bytes, or "TAILERR=path|id|off" = refused -> the requester fetches the whole file.
    if (txt.startsWith("TAILERR=")) {
      if (isPromise && state.dl.raw) { const res = state.dl.resolve; resetDl(); if(res) res({ kind:"tailerr", why:txt.slice(8) }); }
      return;
    }
    if (txt.startsWith("TAIL=")) {
      if (!(isPromise && state.dl.raw)) return;
      const m = /^TAIL=(\d+)@(\d+)$/.exec(txt);
      if (!m) { const res = state.dl.resolve; resetDl(); if(res) res({ kind:"tailerr", why:"header" }); return; }
      const n = parseInt(m[1],10), off = parseInt(m[2],10);
      if (n===0) { const res = state.dl.resolve; resetDl(); if(res) res({ kind:"tail", n:0, off, blob:new Blob([]) }); return; }
      state.dl.mode="file"; state.dl.expect=n; state.dl.got=0; state.dl.chunks=[]; state.dl.tail=off;
      return;
    }
    if (txt.startsWith("SESS=")) {
      state.dl.mode="list"; state.dl.expect=parseInt(txt.slice(5),10)||0; state.dl.got=0; state.dl.chunks=[];
      if (state.dl.expect===0){ if(!state.dl.quiet) populateSessions(""); if(isPromise) settlePromise(""); else { state.dl.mode=null; state.dl.waiting=false; } }
      return;
    }
    if (txt.startsWith("WL=")) {
      state.dl.mode="wl"; state.dl.expect=parseInt(txt.slice(3),10)||0; state.dl.got=0; state.dl.chunks=[];
      if (state.dl.expect===0){ populateWhitelist(""); if(isPromise) settlePromise(""); else { state.dl.mode=null; state.dl.waiting=false; } }
      return;
    }
    return;                                   // ignore stray data
  }
  state.dl.chunks.push(new Uint8Array(v)); state.dl.got += v.length;
  if (isPromise && state.dl.mode==="file" && state.dl.progress) state.dl.progress(state.dl.got, state.dl.expect);
  if (!isPromise && state.dl.mode==="file" && state.dl.got < state.dl.expect) progress(state.dl, Date.now());
  if (state.dl.got >= state.dl.expect) {
    // octet-stream, not the default empty/text type: otherwise Android Chrome sniffs the content as
    // text and appends ".txt" to the .jsonl download name. octet-stream = "save the bytes as named".
    const blob = new Blob(state.dl.chunks, { type: "application/octet-stream" });
    const mode = state.dl.mode;
    if (isPromise && state.dl.raw && mode==="file") {   // ble/logfetch.js: hand over the exact bytes (the cache needs them)
      const res = state.dl.resolve, n = state.dl.expect, off = state.dl.tail;
      resetDl();
      if(res) res(off===undefined ? { kind:"file", n, blob } : { kind:"tail", n, off, blob });
      return;
    }
    if (isPromise) {
      const res = state.dl.resolve, quiet = state.dl.quiet;   // quiet = features/livesync.js's own list: leave the Data/Explore UI alone
      state.dl.completing = true;   // bleFetch's watchdog must not call this "superseded" while the text is decoded
      resetDl();
      blob.text().then(t=>{ if(mode==="list"){ if(!quiet) populateSessions(t); } else if(mode==="wl") populateWhitelist(t); if(res) res(t); });
      return;
    }
    if (mode==="file") {
      if (state.dl.sink==="map") { blob.text().then(t=>plotCsvText(t, state.dl.name)); }
      else if (state.dl.sink==="explore") { const nm=state.dl.name; blob.text().then(t=>loadedExplore(parseScanLog(t), nm)); }
      else if (state.dl.sink==="upload") { const nm=state.dl.name; blob.text().then(t=>uploadSessionText(t, nm)); }
      else if (state.dl.spanName) { const fb=state.dl.name; blob.text().then(t=>saveBlob(blob, rangeFileName(t, fb))); }
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
  if(state.sessionsBusy && state.sessionsBusy()){ toast("Another transfer is running — wait for it to finish.", {err:true}); return; }
  const ctx = {gen:connGen(), sid:state.scannerId};   // the .cflo hook guards the whole action with this
  try{
    toast("Fetching "+fname+" over BLE…", {work:true, sticky:true});
    const cur = await fetchCurrent(fname);   // through the phone-side cache; null = use the plain "L" command
    if(cur){
      if(!cur.blob || !cur.blob.size){ toast("Nothing to download — no log on the device yet.", {err:true}); return; }
      if(state.saveCurrentCflo){ await state.saveCurrentCflo(fname, cur, ctx); return; }   // one .cflo (features/sessions.js)
      saveBlob(cur.blob, fname);
      if(state.fleetRole==="leader" && state.saveCurrentFleet) await state.saveCurrentFleet(fname);   // (features/sessions.js)
      return;
    }
    await syncWait();   // features/livesync.js pass
    if(reqBusy()){ toast("Another transfer is in progress — wait for it to finish.", {err:true}); return; }
    state.dl={mode:null,expect:0,got:0,chunks:[],name:fname,sink:"save",waiting:true,lastRx:Date.now()};
    await gattWrite(state.chars.cmd, enc.encode("L")); log("Requesting current session ("+fname+")…"); }
  catch(e){ log("Download failed: "+e.message); toast("Download failed: "+e.message, {err:true}); }
}
// --- Time-filtered export of ALL logs. The CYD filters on the device across EVERY session file
// (fg + bg, New Session, earlier boots), dates pre-sync rows, and streams one joined NDJSON:
// BLE = CMD "T:<mode>[:<minutes>]:*", Wi-Fi = http://192.168.4.1/dlf?f=all&mode=&arg=.
// (A single selected session's range export lives in sessions.js.)
export async function downloadFilteredAll(){
  const sp=exportSpec(); if(!sp) return;
  await syncWait();   // features/livesync.js pass
  if(reqBusy()){ toast("Another transfer is in progress — wait for it to finish.", {err:true}); return; }
  try{ const prep="Collecting "+sp.tag+" from all logs on the device…";
    state.dl={mode:null,expect:0,got:0,chunks:[],name:"all-"+sp.tag+".jsonl",sink:"save",spanName:true,waiting:true,lastRx:Date.now(),
              t0:Date.now(), prep, label:"Downloading "+sp.tag+" (all logs)", fileIdleMs:10*60000};  // saved as <first>-<last>.jsonl
    toast(prep, {work:true, sticky:true});
    await state.chars.cmd.writeValue(enc.encode(exportCmd(sp, "*"))); log("Requesting "+state.dl.name+" (all logs, filtered on device)…"); }
  catch(e){ log("Filtered download failed: "+e.message); toast("Filtered download failed: "+e.message, {err:true}); }
}
export function downloadFilteredWifi(){
  const sp=exportSpec(); if(!sp) return;
  const url="http://192.168.4.1/dlf?f=all&mode="+sp.mode+(sp.mode===2?"&arg="+sp.arg:"");
  log("Opening "+url+" — join the scanner Wi-Fi first (Data → Download over Wi-Fi). (all logs)");
  window.open(url,"_blank");
}
