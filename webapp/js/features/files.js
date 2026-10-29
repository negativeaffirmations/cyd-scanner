// SD card file explorer (fw >= 0.18). Every op is CMD "Z:<op>:<arg>" (list / mkdir / rename / delete /
// upload begin / abort) or "U<bytes>" (one upload chunk); the device answers each with ONE "FS=<n>" header
// + <n> bytes on LOGDATA ("OK\t…" / "DONE\t…" / "ERR\t<reason>"). This module keeps its own LOGDATA
// listener for that framing (download.js ignores FS= headers), so it works even while a phone still has
// an older cached download.js. Downloads reuse "F:<path>" through download.js's "save" sink.
// One request at a time: each await finishes before the next write (the device has one slot).
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, esc, log, toast } from '../util/dom.js';

const REPLY_MS = 25000;   // the device may be mid scan cycle before it services the request
const CHUNK = 240;        // upload bytes per "U" write: one ATT packet at Android's usual MTU
const MIN_FW = [0, 18, 0];

let cwd = "/";
let entries = [];         // [{name, size, dir, mtime, live}]
let picked = -1;          // index into entries
let busy = false;
let cancelUpload = false;
let pending = null;       // the in-flight FS reply: {expect, got, chunks, resolve, reject, timer}
let listenOn = null;      // the LOGDATA characteristic object our listener is attached to

function fwOk(){
  const v = (state.scannerFw || "").split(".").map(n => parseInt(n, 10) || 0);
  for (let i = 0; i < 3; i++) { if ((v[i] || 0) !== MIN_FW[i]) return (v[i] || 0) > MIN_FW[i]; }
  return true;
}

function onFsData(ev){
  if (!pending) return;
  const dv = ev.target.value;
  const v = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  if (pending.expect < 0) {
    const txt = new TextDecoder().decode(v);
    const m = /^FS=(\d+):(\d+)$/.exec(txt);
    if (!m) return;                              // someone else's stream (SIZE=/SESS=/WL=)
    if (+m[2] !== pending.seq) return;           // a late reply to a request we already gave up on
    pending.expect = +m[1];
    if (pending.expect === 0) settle("");
    return;
  }
  pending.chunks.push(new Uint8Array(v)); pending.got += v.length;
  if (pending.got >= pending.expect) {
    const all = new Uint8Array(pending.got); let o = 0;
    for (const c of pending.chunks) { all.set(c, o); o += c.length; }
    settle(new TextDecoder().decode(all));
  }
}
function settle(text){
  const p = pending; pending = null; clearTimeout(p.timer); p.resolve(text);
}

// Send one "Z" or "U" write and resolve with the reply body. Rejects on "ERR\t…", a timeout, or no link.
// `build(seq)` returns the bytes to write; every request carries a fresh 1-255 tag the device echoes in
// its "FS=<n>:<seq>" header, so a reply that arrives after a timeout can't answer the next request.
let seqCounter = 0;
function fsReq(build){
  return new Promise((resolve, reject)=>{
    if (!state.connected || !state.chars.cmd || !state.chars.log) { reject(new Error("not connected")); return; }
    if (pending) { reject(new Error("busy")); return; }
    if (state.dl.mode !== null) { reject(new Error("another transfer is in progress")); return; }
    if (listenOn !== state.chars.log) {          // (re)attach after a reconnect: new characteristic object
      state.chars.log.addEventListener("characteristicvaluechanged", onFsData);
      listenOn = state.chars.log;
    }
    seqCounter = seqCounter % 255 + 1;
    const seq = seqCounter;
    pending = { seq, expect:-1, got:0, chunks:[],
      resolve: t => t.startsWith("ERR\t") ? reject(new Error(t.slice(4).trim() || "device error")) : resolve(t),
      reject,
      timer: setTimeout(()=>{ pending = null; reject(new Error("no reply from the device")); }, REPLY_MS) };
    state.chars.cmd.writeValue(build(seq)).catch(e=>{ if (pending) { clearTimeout(pending.timer); pending = null; } reject(e); });
  });
}
const zCmd = (op, arg) => fsReq(seq => enc.encode("Z" + seq + ":" + op + ":" + (arg || "")));
// "U" + one raw tag byte + the chunk bytes.
const uChunk = bytes => fsReq(seq => { const c = new Uint8Array(bytes.length + 2); c[0] = 0x55; c[1] = seq; c.set(bytes, 2); return c; });

const join = (dir, name) => (dir === "/" ? "" : dir) + "/" + name;
function parentOf(p){ const i = p.lastIndexOf("/"); return i <= 0 ? "/" : p.slice(0, i); }
function fmtSize(n){
  if (n < 1024) return n + " B";
  if (n < 1048576) return (n / 1024).toFixed(1) + " KB";
  return (n / 1048576).toFixed(1) + " MB";
}
function fmtTime(t){   // FAT mtime; the CYD has no system clock (stamps read 1980) -> show nothing
  if (t < 1577836800) return "";
  return new Date(t * 1000).toLocaleString([], { month:"short", day:"numeric", hour:"numeric", minute:"2-digit" });
}
function msg(t, err){ $("fsMsg").textContent = t || ""; $("fsMsg").classList.toggle("err", !!err); }

function setBusy(b){
  busy = b;
  ["fsUp","fsRefresh","fsMkdir","fsFile"].forEach(id => $(id).disabled = b);
  updateActions();
}
function updateActions(){
  const e = entries[picked];
  $("fsDl").disabled  = busy || !e || e.dir;
  $("fsRen").disabled = busy || !e || e.live;
  $("fsDel").disabled = busy || !e || e.live;
}

function render(){
  $("fsPath").textContent = cwd;
  $("fsUp").disabled = busy || cwd === "/";
  if (!entries.length) { $("fsList").innerHTML = '<div class="sub" style="padding:6px">Empty folder</div>'; updateActions(); return; }
  $("fsList").innerHTML = entries.map((e, i) => {
    const sub = [e.dir ? "folder" : fmtSize(e.size), fmtTime(e.mtime), e.live ? "in use" : ""].filter(Boolean).join(" · ");
    return `<div class="sessrow fsrow${e.dir ? " dir" : ""}${i === picked ? " picked" : ""}" data-i="${i}">` +
      `<span class="material-symbols-outlined">${e.dir ? "folder" : "description"}</span>` +
      `<span class="sessmain"><span class="sesst">${esc(e.name)}</span><span class="sesssub">${esc(sub)}</span></span>` +
      (e.dir ? `<button class="iconbtn fsopen" data-open="${i}" title="Open" aria-label="Open folder"><span class="material-symbols-outlined">chevron_right</span></button>` : "") +
      `</div>`;
  }).join("");
  updateActions();
}

async function refresh(dir){
  setBusy(true); msg("Loading…");
  try {
    // The device sends ~4 KB pages; a full page ends "MORE\t<next skip>" and we ask for the next one.
    const want = dir || cwd, got = [];
    let skip = 0, shown = want;
    for (let page = 0; page < 200; page++) {
      const lines = (await zCmd("l", (skip ? skip + ":" : "") + want)).split("\n");
      shown = (lines[0] || "").split("\t")[1] || want;
      let next = 0;
      for (const ln of lines.slice(1)) {
        if (!ln) continue;
        if (ln.startsWith("MORE\t")) { next = parseInt(ln.slice(5), 10) || 0; continue; }
        const f = ln.split("\t");
        got.push({ name:f[0], size:+f[1] || 0, dir:f[2] === "d", mtime:+f[3] || 0, live:f[4] === "live" });
      }
      if (!next || next <= skip) break;
      skip = next;
      msg("Loading… " + got.length + " item(s)");
    }
    cwd = shown;
    entries = got.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric:true }));
    picked = -1;
    msg(entries.length + " item(s)");
  } catch (e) { msg("List failed: " + e.message, true); }
  setBusy(false); render();
}

export function openFiles(){
  if (!state.connected) { toast("Connect to the scanner first.", { err:true }); return; }
  if (!fwOk()) { toast("The file explorer needs scanner firmware 0.18.0 or newer (this one: " + (state.scannerFw || "?") + ").", { err:true }); return; }
  $("filesDlg").showModal();
  if (!busy) refresh(cwd);   // a cancelled upload may still be finishing; it refreshes itself
  else render();
}
export function closeFiles(){ $("filesDlg").close(); }
export function onFilesClosed(){ cancelUpload = true; }   // dialog closed (× or Esc): stop a running upload
export function fsUp(){ if (cwd !== "/") refresh(parentOf(cwd)); }
export function fsRefresh(){ refresh(cwd); }

export function fsListClick(ev){
  if (busy) return;
  const open = ev.target.closest("[data-open]");
  if (open) { refresh(join(cwd, entries[+open.dataset.open].name)); return; }
  const row = ev.target.closest("[data-i]");
  if (!row) return;
  const i = +row.dataset.i;
  picked = picked === i ? -1 : i;
  render();
}

export async function fsMkdir(){
  const name = (prompt("New folder name:") || "").trim();
  if (!name) return;
  if (/[\\/:*?"<>|]/.test(name)) { msg("A name can't contain \\ / : * ? \" < > |", true); return; }
  setBusy(true);
  try { await zCmd("m", join(cwd, name)); log("Created folder " + join(cwd, name)); }
  catch (e) { msg("New folder failed: " + e.message, true); setBusy(false); return; }
  refresh(cwd);
}

export async function fsRename(){
  const e = entries[picked]; if (!e) return;
  const to = (prompt("Rename to (a name, or a full /path to move it):", e.name) || "").trim();
  if (!to || to === e.name) return;
  const dest = to.startsWith("/") ? to : join(cwd, to);
  setBusy(true);
  try { await zCmd("r", join(cwd, e.name) + "\t" + dest); log("Renamed " + join(cwd, e.name) + " → " + dest); }
  catch (err) { msg("Rename failed: " + err.message, true); setBusy(false); return; }
  refresh(cwd);
}

export async function fsDelete(){
  const e = entries[picked]; if (!e) return;
  const p = join(cwd, e.name);
  if (!confirm("Delete " + p + (e.dir ? " (the folder must be empty)" : "") + "? This can't be undone.")) return;
  setBusy(true);
  try { await zCmd("d", p); log("Deleted " + p); }
  catch (err) { msg("Delete failed: " + err.message, true); setBusy(false); return; }
  refresh(cwd);
}

// Download through download.js's existing "save" sink ("F:<path>" -> SIZE= + raw bytes -> saved file).
export async function fsDownload(){
  const e = entries[picked]; if (!e || e.dir) return;
  if (pending || state.dl.mode !== null) { msg("Another transfer is in progress.", true); return; }
  const p = join(cwd, e.name);
  const mine = { mode:null, expect:0, got:0, chunks:[], name:e.name, sink:"save" };
  state.dl = mine;
  setBusy(true); msg("Downloading " + e.name + "…");
  try { await state.chars.cmd.writeValue(enc.encode("F:" + p)); }
  catch (err) { msg("Download failed: " + err.message, true); setBusy(false); return; }
  const t0 = Date.now();
  // download.js fills `mine` (expect from SIZE=, got per chunk) and swaps state.dl for a fresh object
  // when it finishes -- after saving the file OR after its stall watchdog cancels, so success is judged
  // by the byte count, not by the swap. SIZE=0 leaves `mine` in place with mode null; treat that as an
  // empty answer once the stream has been quiet a moment (a stray notification also stamps lastRx).
  const outcome = await new Promise(res => {
    const iv = setInterval(()=>{
      let r = null;
      if (state.dl !== mine) r = mine.expect > 0 && mine.got >= mine.expect ? "saved" : "stalled";
      else if (mine.lastRx && mine.mode === null && mine.expect === 0 && Date.now() - mine.lastRx > 1500) r = "empty";
      else if (!mine.lastRx && Date.now() - t0 > REPLY_MS) r = "noreply";
      else if (mine.expect) msg("Downloading " + e.name + "… " + Math.floor(100 * mine.got / mine.expect) + "%");
      if (r) { clearInterval(iv); res(r); }
    }, 400);
  });
  if (outcome === "saved") msg("Saved " + e.name + ".");
  else if (outcome === "empty") msg("Nothing downloaded: the file is empty or couldn't be read.", true);
  else if (outcome === "stalled") msg("Download stalled at " + mine.got + " of " + mine.expect + " bytes. Try again.", true);
  else msg("Download failed: no reply from the device.", true);
  setBusy(false);
}

// Upload the picked phone file into the current folder: Z:u:<size>:<path>, then 240-byte "U" chunks,
// each acknowledged before the next. The device writes <path>.upl and renames it over <path> at the end.
export async function fsUpload(ev){
  const file = ev.target.files && ev.target.files[0];
  ev.target.value = "";               // so picking the same file again still fires "change"
  if (!file) return;
  if (/[\\/:*?"<>|]/.test(file.name)) { msg("That file name has characters the SD card can't store.", true); return; }
  const p = join(cwd, file.name);
  const old = entries.find(x => x.name === file.name);
  if (old && old.dir) { msg("A folder with that name already exists.", true); return; }
  if (!confirm(old ? p + " already exists. Replace it with " + file.name + " (" + fmtSize(file.size) + ")?"
                   : "Upload " + file.name + " (" + fmtSize(file.size) + ") to " + p + "?")) return;
  const buf = new Uint8Array(await file.arrayBuffer());
  setBusy(true); cancelUpload = false;
  // Android rejects a GATT write while another is in flight ("GATT operation failed for unknown reason"),
  // and the ~5 s GPS feed would collide with one of the hundreds of chunk writes: hold it for the upload.
  state.gpsHold = true;
  try {
    await zCmd("u", buf.length + ":" + p);
    const t0 = Date.now();
    for (let off = 0; off < buf.length; off += CHUNK) {
      if (cancelUpload || !state.connected) { try { await zCmd("a"); } catch (_) {} throw new Error("cancelled"); }
      const n = Math.min(CHUNK, buf.length - off);
      await uChunk(buf.subarray(off, off + n));
      const kbs = (off + n) / 1024 / Math.max(0.001, (Date.now() - t0) / 1000);
      msg("Uploading " + file.name + "… " + Math.floor(100 * (off + n) / buf.length) + "% (" + kbs.toFixed(1) + " KB/s)");
    }
    log("Uploaded " + p + " (" + fmtSize(buf.length) + ")");
    toast("Uploaded " + file.name + ".");
  } catch (err) { msg("Upload failed: " + err.message, true); setBusy(false); return; }
  finally { state.gpsHold = false; }
  setBusy(false);
  if ($("filesDlg").open) refresh(cwd);
}
