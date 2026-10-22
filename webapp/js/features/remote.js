// Remote server (scan-analyzer) pairing + upload client.
//
// Realizes cyd-scanner HANDOFF #11 against the scan-analyzer API contract v0 (see the inter-project
// bridge). Three jobs, all to the USER's OWN private server — no hard-coded address:
//   1. PAIRING  — a QR on the scan-analyzer UI deep-links here with `#server=<enc>&token=<token>`
//      (a `#` fragment, so the token never reaches GitHub Pages' request logs). We exchange the
//      one-time token for a long-lived per-device key: POST <server>/api/v1/pair {token} -> {key,device_id}.
//      A Settings paste field is the fallback for phones whose camera can't deep-link.
//   2. FILE UPLOAD (full fidelity) — POST <server>/api/v1/uploads, raw `application/x-ndjson` body,
//      Bearer key, X-Session-Name / X-Client-Tz headers, sha256-idempotent on the server.
//   3. LIVE-POST (Phase 3, opt-in) — POST <server>/api/v1/detections, batched REDUCED log-shaped
//      NDJSON (each row `"live":1`); the web app can't produce ch/cid/uuid/pan/sig/score so those are
//      omitted. Off by default (a browser tab can't reliably hold BLE+GPS+network in the background).
//
// The server is the user's private scan-analyzer; uploading our own captures to it is the intended
// design. The key/token are secrets — stored in localStorage, never written to the Activity log.
import { state } from '../state.js';
import { $, log, esc, toast } from '../util/dom.js';
import { enc, TIER_NAMES } from '../config.js';

// --- persisted config (localStorage) ------------------------------------------------------------
const LS = { server:"rs.server", key:"rs.key", dev:"rs.deviceId", live:"rs.live" };
function lsGet(k){ try{ return localStorage.getItem(k) || ""; }catch(e){ return ""; } }
function lsSet(k,v){ try{ v==null||v==="" ? localStorage.removeItem(k) : localStorage.setItem(k,v); }catch(e){} }

export function serverUrl(){ return lsGet(LS.server); }
function deviceKey(){ return lsGet(LS.key); }
function deviceId(){ return lsGet(LS.dev); }
export function isPaired(){ return !!(serverUrl() && deviceKey()); }
function liveEnabled(){ return lsGet(LS.live) === "1"; }

// Normalize a pasted/QR server origin: trim, drop a trailing slash. Endpoints are appended.
function normServer(u){ return (u||"").trim().replace(/\/+$/,""); }
function api(path){ return normServer(serverUrl()) + path; }
function authHeaders(extra){ return Object.assign({ "Authorization": "Bearer " + deviceKey() }, extra||{}); }
function clientTz(){ try{ return Intl.DateTimeFormat().resolvedOptions().timeZone || ""; }catch(e){ return ""; } }
// The CYD logs `epoch` as LOCAL wall-clock encoded as seconds (read back with UTC getters — see
// util/format.js). Match that convention for live rows so they line up with file-uploaded rows.
function localEpoch(){ return Math.floor(Date.now()/1000) - new Date().getTimezoneOffset()*60; }

// --- pairing ------------------------------------------------------------------------------------
// Exchange a one-time token for a per-device key. Returns true on success.
export async function pair(server, token){
  const s = normServer(server);
  if(!/^https:\/\//i.test(s)){
    log("Remote: server must be HTTPS (the app is served over HTTPS, so the browser blocks mixed/HTTP content).");
    return false;
  }
  if(!token){ log("Remote: no pairing token."); return false; }
  log("Remote: pairing with "+s+"…");
  try{
    const r = await fetch(s + "/api/v1/pair", {
      method:"POST", headers:{ "Content-Type":"application/json" },
      body: JSON.stringify({ token })
    });
    if(!r.ok){ log("Remote: pairing failed ("+r.status+" "+r.statusText+")."); return false; }
    const j = await r.json();
    if(!j || !j.key){ log("Remote: pairing response had no key."); return false; }
    lsSet(LS.server, s); lsSet(LS.key, j.key); lsSet(LS.dev, j.device_id || "");
    log("Remote: paired"+(j.device_id?(" as device "+j.device_id):"")+".");   // never log the key itself
    renderRemoteUI();
    return true;
  }catch(e){
    log("Remote: pairing error — "+e.message+" (check the URL, HTTPS, and the server's CORS for "+location.origin+").");
    return false;
  }
}

export function unpair(){
  lsSet(LS.key, ""); lsSet(LS.dev, "");   // keep the server URL so it's easy to re-pair
  log("Remote: unpaired (per-device key forgotten).");
  renderRemoteUI();
}

// Read a pairing deep link from the URL fragment: #server=<url-enc>&token=<token>. Called once at
// startup. The fragment is never sent to a server, so the token can't leak into static-host logs.
export async function handlePairHash(){
  const h = (location.hash || "").replace(/^#/, "");
  if(!h) return;
  const p = {}; h.split("&").forEach(kv=>{ const i=kv.indexOf("="); if(i>0) p[kv.slice(0,i)] = decodeURIComponent(kv.slice(i+1)); });
  if(!p.server && !p.token) return;
  // Clear the fragment immediately so the token doesn't linger in the address bar / history.
  try{ history.replaceState(null, "", location.pathname + location.search); }catch(e){}
  if(p.server && p.token){ await pair(p.server, p.token); }
  else if(p.server){ lsSet(LS.server, normServer(p.server)); log("Remote: server set from QR; paste the pairing token in Settings to finish."); renderRemoteUI(); }
}

// --- file upload (full-fidelity SD log) ---------------------------------------------------------
// Disable/enable both upload buttons for the duration of a fetch+upload so a double-tap can't start
// two. Re-enabling honors the current paired/connected state (via renderRemoteUI).
function setUploadBusy(busy){
  if(busy){ ["rsUploadCurBtn","sessUploadBtn"].forEach(id=>{ const e=$(id); if(e) e.disabled = true; }); return; }
  renderRemoteUI();   // restores rsUploadCurBtn per paired+connected
  const su = $("sessUploadBtn"); if(su) su.disabled = !(state.connected && isPaired());
}

// Upload raw NDJSON text (as fetched from the device) to the server. sessionName -> X-Session-Name.
// This is the terminal step of the BLE fetch -> upload flow (download.js routes the "upload" sink
// here), so it always clears the busy state, success or fail.
export async function uploadSessionText(text, sessionName){
  if(!isPaired()){ log("Remote: not paired — pair with a server first (Settings)."); toast("Not paired — pair a server in Settings.", {err:true}); setUploadBusy(false); return; }
  if(!text || !text.trim()){ log("Remote: nothing to upload (empty session)."); toast("Nothing to upload — that session is empty.", {err:true}); setUploadBusy(false); return; }
  const isCsv = /\.csv$/i.test(sessionName||"");
  const bytes = enc.encode(text);
  log("Remote: uploading "+(sessionName||"session")+" ("+bytes.length+" bytes)…");
  toast("Uploading "+(sessionName||"session")+" ("+Math.round(bytes.length/1024)+" KB)…", {work:true, sticky:true});
  try{
    const r = await fetch(api("/api/v1/uploads"), {
      method:"POST",
      headers: authHeaders({ "Content-Type": isCsv ? "text/csv" : "application/x-ndjson",
                             "X-Session-Name": (sessionName||"").slice(0,120), "X-Client-Tz": clientTz() }),
      body: bytes
    });
    if(r.status===401 || r.status===403){
      log("Remote: upload rejected ("+r.status+") — the device key may be revoked. Re-pair in Settings.");
      toast("Upload rejected ("+r.status+") — re-pair in Settings.", {err:true}); return;
    }
    if(!r.ok){ log("Remote: upload failed ("+r.status+" "+r.statusText+")."); toast("Upload failed ("+r.status+" "+r.statusText+").", {err:true}); return; }
    const j = await r.json().catch(()=>null);
    if(j){
      const msg = "Uploaded "+(sessionName||"session")+": "+(j.sightings??"?")+" sighting(s), "+(j.events??"?")+" event(s)"+(j.duplicate?" — already had it (dedup)":"")+".";
      log("Remote: "+msg); toast(msg);
    } else { log("Remote: upload accepted."); toast("Uploaded "+(sessionName||"session")+" — accepted."); }
  }catch(e){
    log("Remote: upload error — "+e.message+" (HTTPS + server CORS for "+location.origin+" required).");
    toast("Upload error — "+e.message, {err:true});
  }finally{
    setUploadBusy(false);
  }
}

// --- live-POST (Phase 3, opt-in) ----------------------------------------------------------------
// Decimal DetFlags bits -> the NDJSON log's `+`-joined flag names (matches the SD-log schema so the
// reduced live rows validate the same way).
const FLAG_NAMES = [[8,"ibeacon"],[16,"findmy"],[32,"pwnagotchi"],[64,"eviltwin"],[128,"droneid"]];
function flagStr(bits){ return FLAG_NAMES.filter(f=>bits&f[0]).map(f=>f[1]).join("+"); }

let livePosting = false;    // in-flight guard (don't pile POSTs if the network lags behind the ~2 s snapshot cadence)
let liveFailLogged = false; // only log the first failure in a run, then stay quiet until a success

// Scan-provenance anchor for a live-POST batch: the locked {"evt":"meta"} shape, built from the
// STATUS fields the connected CYD reports (state.scannerId/Fw/DbGen). File uploads already carry the
// CYD's own meta line as line 1; for live rows the phone learns the id over STATUS and prepends it
// here. Returns "" (no meta) until the CYD has reported an id — the server ingests meta-less fine.
function metaLine(){
  if(!state.scannerId) return "";
  const o = { evt:"meta", scanner_id: state.scannerId };
  if(state.scannerName) o.scanner_name = state.scannerName;
  if(state.scannerFw) o.fw = state.scannerFw;
  if(state.scannerDbGen) o.db_gen = state.scannerDbGen;
  return JSON.stringify(o);
}

// Build one reduced, log-shaped NDJSON line from a live DETS row. Omits the fields the web app can't
// produce (ch/ie/cid/uuid/pan/score/sig) per the contract; lat/lon = the phone's own fix (omitted
// when there's no fix). epoch = LOCAL seconds, ms = phone wall-clock ms (unique per snapshot for the
// server's session+ms dedup).
function liveRow(r, sess, epoch, ms){
  const g = state.lastGps, haveFix = !!(g && g.t && (g.lat || g.lon));
  const srcs = (r.sources||[]).slice().sort((a,b)=>b.rssi-a.rssi);
  const o = { live:1, session:sess, epoch, ms, src: srcs.length?srcs[0].tag:"", mac:r.mac, rssi:r.rssi };
  if(haveFix){ o.lat = +g.lat.toFixed(6); o.lon = +g.lon.toFixed(6); }
  if(r.name) o.name = r.name;
  const tn = typeof r.tier==="number" ? r.tier : 0;
  if(tn>0) o.tier = TIER_NAMES[tn];
  const fs = flagStr(r.flags||0); if(fs) o.flags = fs;
  return JSON.stringify(o);
}

// Called from renderDets once per completed live snapshot. No-ops unless paired, live-upload is on,
// and a scan is actually running.
export function maybePostLive(rows){
  if(!liveEnabled() || !isPaired() || !state.scanning || !state.connected) return;
  if(livePosting || !rows || !rows.length) return;
  const sess = ($("sessName").textContent.replace(/^Session:\s*/,"").trim()) || "live";
  const epoch = localEpoch();
  const baseMs = Date.now();
  const meta = metaLine();
  const body = (meta ? meta+"\n" : "") + rows.map((r,i)=> liveRow(r, sess, epoch, baseMs+i)).join("\n") + "\n";
  livePosting = true;
  fetch(api("/api/v1/detections"), {
    method:"POST", headers: authHeaders({ "Content-Type":"application/x-ndjson" }), body
  }).then(r=>{
    if(!r.ok){ if(!liveFailLogged){ log("Remote: live upload failed ("+r.status+") — pausing live POSTs until it recovers."); liveFailLogged=true; } return; }
    liveFailLogged = false;
    return r.json().catch(()=>null);
  }).catch(e=>{
    if(!liveFailLogged){ log("Remote: live upload error — "+e.message+". Pausing until it recovers."); liveFailLogged=true; }
  }).finally(()=>{ livePosting = false; });
}

// --- Settings UI --------------------------------------------------------------------------------
export function renderRemoteUI(){
  const st = $("rsStatus"); if(st){
    if(isPaired()) st.innerHTML = 'Paired to <strong>'+esc(serverUrl())+'</strong>'+(deviceId()?(' · device <code>'+esc(deviceId())+'</code>'):'');
    else if(serverUrl()) st.innerHTML = 'Server set: <strong>'+esc(serverUrl())+'</strong> — not paired yet.';
    else st.textContent = 'Not paired. Scan the QR on your scan-analyzer server, or paste its URL + token below.';
  }
  const srv = $("rsServer"); if(srv && !srv.matches(":focus") && serverUrl()) srv.value = serverUrl();
  const liveCb = $("rsLive"); if(liveCb) liveCb.checked = liveEnabled();
  const paired = isPaired();
  const unp = $("rsUnpairBtn"); if(unp) unp.disabled = !paired;
  // "Upload current session" needs both a pairing AND a live BLE link (it fetches the log first).
  const upc = $("rsUploadCurBtn"); if(upc) upc.disabled = !(paired && state.connected);
  const liveWrap = $("rsLiveWrap"); if(liveWrap) liveWrap.style.opacity = paired ? "1" : ".5";
}

// Settings "Pair" button: pair from the pasted URL + token.
export async function pairFromForm(){
  const server = $("rsServer").value, token = $("rsToken").value.trim();
  if(!server.trim()){ log("Remote: enter the server URL."); return; }
  if(!token){ log("Remote: enter the pairing token (from the server's QR page)."); return; }
  const ok = await pair(server, token);
  if(ok) $("rsToken").value = "";   // one-time token — clear it after use
}

export function toggleLive(){
  const on = $("rsLive").checked;
  if(on && !isPaired()){ $("rsLive").checked = false; log("Remote: pair with a server before enabling live upload."); return; }
  lsSet(LS.live, on ? "1" : "");
  log("Remote: live detection upload "+(on?"ON — the app will POST each scan snapshot while scanning (keep the screen on).":"off."));
}

// Download the device's CURRENT session over BLE, then upload it (sink "upload" -> onLogData ->
// uploadSessionText). Reuses the same LOGDATA stream the normal download uses.
export async function uploadCurrentSession(){
  if(!isPaired()){ log("Remote: pair with a server first (Settings)."); return; }
  if(!state.connected || !state.chars.cmd){ log("Remote: connect to the scanner first."); return; }
  const sess = ($("sessName").textContent.replace(/^Session:\s*/,"").trim()) || "scanlog";
  const fname = sess.endsWith(".jsonl")||sess.endsWith(".csv") ? sess : sess+".jsonl";
  setUploadBusy(true);                                        // optimistic: lock buttons the instant it's tapped
  toast("Fetching "+fname+" over BLE…", {work:true, sticky:true});
  try{
    state.dl = { mode:null, expect:0, got:0, chunks:[], name:fname, sink:"upload" };
    await state.chars.cmd.writeValue(enc.encode("L"));
    log("Remote: fetching current session ("+fname+") to upload…");
  }catch(e){ log("Remote: could not request the session — "+e.message); toast("Could not request the session — "+e.message, {err:true}); setUploadBusy(false); }
}
