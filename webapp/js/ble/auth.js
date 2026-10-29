// BLE phone-link authentication (SEC-H3 Phase A) — see docs/ble-auth.md.
//
// Every scanner holds a random 128-bit key (NVS). The phone learns it once from a QR (key sticker,
// CYD Settings -> Phone key, or the serial `key` command) and proves it on each connection with an
// HMAC-SHA256 challenge-response over the AUTH characteristic; the scanner proves it back (`dp`)
// before we send it our TIME / GPS, so a look-alike device can't harvest the owner's position.
//
// KEY STORE: IndexedDB holding NON-EXTRACTABLE HMAC CryptoKeys keyed by scanner id (sid), plus
// {name, kg, ts}. The raw key hex only lives in memory for the instant it takes to import it — it is
// never written to localStorage / IndexedDB / the Activity log, so other pages on this github.io
// origin can use a stored key only by running code here, never read it out.
//
// This is a NEW file (so it is never served stale from the 10-min cache alongside a fresh main.js);
// its UUID is defined here rather than in config.js for the same reason.
import { $, log, esc, toast } from '../util/dom.js';

export const AUTH_UUID = "9a1e0007-2b7e-4c1a-9b00-1a2b3c4d5e6f";

const te = new TextEncoder(), td = new TextDecoder();
const HEX12 = /^[0-9a-f]{12}$/, HEX32 = /^[0-9a-f]{32}$/, HEX64 = /^[0-9a-f]{64}$/;

// ---------------------------------------------------------------- hex helpers
function toHex(buf){ return Array.from(new Uint8Array(buf), b => b.toString(16).padStart(2, "0")).join(""); }
function fromHex(h){ const a = new Uint8Array(h.length / 2); for(let i = 0; i < a.length; i++) a[i] = parseInt(h.substr(i * 2, 2), 16); return a; }
function randHex(n){ return toHex(crypto.getRandomValues(new Uint8Array(n))); }

// ---------------------------------------------------------------- IndexedDB key store
const DB_NAME = "cflo-keys", STORE = "keys";
let dbP = null;
function db(){
  if(dbP) return dbP;
  dbP = new Promise((res, rej) => {
    if(!window.indexedDB){ rej(new Error("IndexedDB unavailable")); return; }
    const r = indexedDB.open(DB_NAME, 1);
    r.onupgradeneeded = () => r.result.createObjectStore(STORE, { keyPath:"sid" });
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
  dbP.catch(() => { dbP = null; });   // allow a later retry
  return dbP;
}
async function tx(mode, fn){
  const d = await db();
  return new Promise((res, rej) => {
    const t = d.transaction(STORE, mode), s = t.objectStore(STORE);
    const req = fn(s);
    t.oncomplete = () => res(req ? req.result : undefined);
    t.onerror = () => rej(t.error); t.onabort = () => rej(t.error);
  });
}
async function getRec(sid){ return tx("readonly", s => s.get(sid)); }
async function putRec(rec){ return tx("readwrite", s => s.put(rec)); }

// All stored keys' metadata (never the CryptoKey itself), newest first.
export async function listKeys(){
  try{
    const all = await tx("readonly", s => s.getAll());
    return (all || []).map(r => ({ sid:r.sid, name:r.name || "", kg:r.kg, ts:r.ts || 0 })).sort((a, b) => b.ts - a.ts);
  }catch(e){ return []; }
}
export async function forgetKey(sid){ await tx("readwrite", s => s.delete(sid)); }

// Import a raw 128-bit key as a non-extractable HMAC key and store it under sid. kg = key generation
// (null when unknown, e.g. a pasted bare key for a scanner we never read).
// Never silently overwrites a DIFFERENT key already stored for that sid (key planting, REV-005/SEC L2):
// confirmReplace(old) must resolve true first. Returns "stored" | "same" | "cancelled" | false (invalid).
export async function storeKey(sid, keyHex, kg, name, confirmReplace){
  sid = (sid || "").toLowerCase(); keyHex = (keyHex || "").toLowerCase();
  if(!HEX12.test(sid) || !HEX32.test(keyHex)) return false;
  const raw = fromHex(keyHex);
  let key;
  try{ key = await crypto.subtle.importKey("raw", raw, { name:"HMAC", hash:"SHA-256" }, false, ["sign", "verify"]); }
  finally{ raw.fill(0); }
  const old = await getRec(sid).catch(() => null);
  let result = "stored";
  if(old && old.key){
    if(await sameKey(old.key, key)){ key = old.key; result = "same"; }
    else if(!confirmReplace || !(await confirmReplace(old))) return "cancelled";
  }
  await putRec({ sid, key, kg: kg == null ? (old ? old.kg : null) : kg, name: name || (old && old.name) || "", ts: Date.now() });
  return result;
}

// Two non-extractable keys are equal iff they sign a fixed test string identically.
async function sameKey(a, b){
  const m = te.encode("CFLO-KEYCMP-1");
  const [x, y] = await Promise.all([crypto.subtle.sign("HMAC", a, m), crypto.subtle.sign("HMAC", b, m)]);
  return toHex(x) === toHex(y);
}

// A stored key matching this scanner name OR sid, if any (metadata only) — the legacy-path impostor check.
export async function keyHeldFor(name, sid){
  const keys = await listKeys();
  sid = (sid || "").toLowerCase();
  return keys.find(k => (name && k.name && k.name === name) || (sid && k.sid === sid)) || null;
}

// ---------------------------------------------------------------- in-app confirm / notice dialog (#secDlg)
// askConfirm({title, msg, ok, cancel, danger, kind}) -> Promise<boolean>. cancel:null = notice (OK only).
// Button wiring lives in main.js (answerConfirm). A newer prompt resolves an older pending one as false.
let cfm = null;   // {resolve, kind}
export function askConfirm(o){
  if(cfm){ const r = cfm.resolve; cfm = null; r(false); }
  $("secTitle").textContent = o.title || "Confirm";
  $("secMsg").textContent = o.msg || "";
  const ok = $("secOk"), cancel = $("secCancel");
  ok.textContent = o.ok || "OK"; ok.className = o.danger ? "danger" : "";
  cancel.textContent = o.cancel || "Cancel"; cancel.hidden = o.cancel === null;
  return new Promise(resolve => {
    cfm = { resolve, kind:o.kind || "" };
    if(!$("secDlg").open) $("secDlg").showModal();
  });
}
export function answerConfirm(yes){
  const c = cfm; cfm = null;
  if($("secDlg").open) $("secDlg").close();
  if(c) c.resolve(!!yes);
}
function cancelConfirm(kind){ if(cfm && cfm.kind === kind) answerConfirm(false); }
function notice(title, msg){ return askConfirm({ title, msg, ok:"OK", cancel:null, kind:"notice" }); }

// ---------------------------------------------------------------- legacy (no-AUTH) firmware guard
// Old firmware can't prove who it is, so (M2/REV-004): refuse it outright when it looks like a scanner
// we hold a key for (name / sid match) or claims fw >= 0.18.0 (which always has AUTH) — that's an
// impostor or a stale Android GATT cache; otherwise ask once per device name before sending it our
// TIME / GPS. REV-016: a STATUS read of exactly "auth=0" is new firmware seen through a stale cache.
const LEGACY_OK = "cflo.legacyOk.";   // + device name -> "1" send / "0" don't
function lsGet(k){ try{ return localStorage.getItem(k); }catch(e){ return null; } }
function lsSet(k, v){ try{ localStorage.setItem(k, v); }catch(e){} }

function fwAtLeast(fw, maj, min){
  const m = /^(\d+)\.(\d+)/.exec(fw || ""); if(!m) return false;
  return +m[1] > maj || (+m[1] === maj && +m[2] >= min);
}
function kvParse(text){ return parseAuth(text); }

// Pre-subscribe check on the legacy path. statusText = one STATUS read. Returns null when it may
// proceed, or {title,msg} explaining why the connection must be dropped.
export async function legacyRefusal(deviceName, statusText){
  const t = (statusText || "").trim();
  if(t === "auth=0") return { title:"Bluetooth cache is stale",
    msg:"This scanner runs new (authenticated) firmware, but the phone is still using its old Bluetooth " +
        "service list. Toggle Bluetooth off/on, or forget the device in Android Bluetooth settings, then reconnect." };
  const kv = kvParse(t);
  const held = await keyHeldFor(deviceName, kv.sid);
  if(held || fwAtLeast(kv.fw, 0, 18)) return legacyImpostor(deviceName, held, kv.fw);
  return null;
}
function legacyImpostor(name, held, fw){
  return { title:"Connection refused",
    msg:(name || "This device") + " offered no authentication, but " +
        (held ? "this browser holds a key for " + (held.name || held.sid) : "it reports firmware " + fw + ", which always authenticates") +
        ". It may be an impostor, or the phone's Bluetooth cache is stale (toggle Bluetooth off/on or forget the " +
        "device in Android Bluetooth settings). Nothing was sent to it." };
}
// Live check on legacy STATUS notifications (a sid/fw may only show up in a later part).
let heldSids = new Set(), heldNames = new Set();
export async function primeLegacyGuard(){
  const keys = await listKeys();
  heldSids = new Set(keys.map(k => k.sid)); heldNames = new Set(keys.map(k => k.name).filter(Boolean));
}
export function legacyStatusRefusal(deviceName, statusText){
  if(linkSec !== "legacy") return null;
  const kv = kvParse(statusText);
  const held = (kv.sid && heldSids.has(kv.sid)) || (kv.name && heldNames.has(kv.name));
  if(held || fwAtLeast(kv.fw, 0, 18)) return legacyImpostor(deviceName, held ? { sid:kv.sid, name:kv.name } : null, kv.fw);
  return null;
}
export function showRefusal(r){ lastFail = r.msg; renderKeysUI(); return notice(r.title, r.msg); }

// May we send TIME / GPS on this link? Authenticated: yes. Legacy: the remembered per-name choice, or
// ask (ask=true forces a prompt even after a remembered "no", e.g. the user tapped Sync).
export async function mayShareLocation(deviceName, ask){
  if(linkSec === "auth") return true;
  if(linkSec !== "legacy") return false;
  const k = LEGACY_OK + (deviceName || "?"), saved = lsGet(k);
  if(saved === "1") return true;
  if(saved === "0" && !ask) return false;
  const yes = await askConfirm({ title:"Unsecured firmware",
    msg:(deviceName || "This scanner") + " has unsecured firmware (no phone-link authentication), so the app can't " +
        "verify it's really your scanner. Send it your time and location?",
    ok:"Send time & location", cancel:"Don't send", kind:"legacy" });
  if(linkSec !== "legacy") return false;   // disconnected while the prompt was open
  lsSet(k, yes ? "1" : "0");
  return yes;
}

// ---------------------------------------------------------------- link-security state
// "" = not connected · "legacy" = old firmware without AUTH · "auth" = authenticated this connection.
let linkSec = "", linkInfo = null, lastFail = "";
let pending = null;   // {sid, name, kg} of the scanner that last asked for a key (for a bare-key paste)

export function linkSecurity(){ return linkSec; }
export function setLinkSecurity(mode, info){
  linkSec = mode || ""; linkInfo = info || null;
  if(mode === "auth") lastFail = "";
  paintConn(); renderKeysUI();
}
export function clearLinkSecurity(){ cancelConfirm("legacy"); setLinkSecurity(""); }

// Tint the app-bar Bluetooth icon by link security (setConn() owns the base up/wait/down state and
// rewrites the glyph, so call this after it). Legacy firmware gets a warning tint + a small badge.
export function paintConn(){
  const p = $("conn"); if(!p) return;
  p.classList.toggle("legacy", linkSec === "legacy" && p.classList.contains("up"));
  p.classList.toggle("authed", linkSec === "auth" && p.classList.contains("up"));
  const badge = p.querySelector(".secbadge");
  if(linkSec === "legacy" && p.classList.contains("up")){
    p.title = "connected — unsecured firmware (no authentication)";
    if(!badge) p.insertAdjacentHTML("beforeend", '<span class="secbadge material-symbols-outlined">warning</span>');
  } else {
    if(badge) badge.remove();
    if(linkSec === "auth" && p.classList.contains("up")) p.title = "connected — authenticated";
  }
}

// ---------------------------------------------------------------- AUTH value parse
// "k=v;k=v" -> object (order-independent; unknown keys kept).
export function parseAuth(text){
  const o = {};
  (text || "").split(";").forEach(kv => { const i = kv.indexOf("="); if(i > 0) o[kv.slice(0, i).trim()] = kv.slice(i + 1).trim(); });
  if(o.sid) o.sid = o.sid.toLowerCase();
  if(o.n) o.n = o.n.toLowerCase();
  if(o.dp) o.dp = o.dp.toLowerCase();
  return o;
}
async function readAuth(c){ const v = await c.readValue(); return parseAuth(td.decode(v)); }
function kgNum(v){ const n = parseInt(v, 10); return Number.isFinite(n) ? n : null; }

function isTransport(e){
  // GATT failures surface as DOMExceptions (NetworkError "GATT operation failed…", NotSupportedError,
  // InvalidStateError…). Anything not ours (no key / bad value) is thrown as a plain Error with .auth.
  return e && !e.auth;
}

// ---------------------------------------------------------------- authenticate
// Runs the challenge-response on the AUTH characteristic. Resolves to
//   {status:"ok", info} | {status:"nokey"|"rejected"|"locked"|"spoof"|"error", info, wait, rekeyed, msg}
// Retries ONCE, with a fresh read, only on a GATT transport error; never auto-retries a rejection.
export async function authenticate(authChar, device){
  let attempt = null;   // {n, cn, key} of the last H: write (lets a retry verify a dp it missed)
  for(let round = 0; round < 2; round++){
    try{
      return await exchange(authChar, attempt, a => { attempt = a; });
    }catch(e){
      const gone = device && device.gatt && !device.gatt.connected;
      if(round === 0 && isTransport(e) && !gone){ log("Auth: GATT error ("+e.message+") — retrying once."); continue; }
      return { status:"error", msg:e.message };
    }
  }
  return { status:"error", msg:"auth failed" };
}

async function exchange(c, prev, setAttempt){
  let info = await readAuth(c);
  if(!HEX12.test(info.sid || "")){ const e = new Error("bad AUTH value (no sid)"); e.auth = true; throw e; }
  const sid = info.sid, devKg = kgNum(info.kg);
  pending = { sid, name: info.name || "", kg: devKg };

  // A retry after a transport error can find st=1 from our earlier write: verify its dp against that attempt.
  if(info.st === "1"){
    if(prev && info.dp) return finish(info, prev, sid, devKg);
    return { status:"error", info, msg:"scanner reports authenticated without our proof" };
  }
  if(info.st === "2") return { status:"locked", info, wait: +info.wait || 0 };
  if(info.st !== "0" || !HEX32.test(info.n || "")) return { status:"error", info, msg:"unexpected AUTH state" };

  const rec = await getRec(sid).catch(() => null);
  if(!rec || !rec.key) return { status:"nokey", info };

  const n = info.n, cn = randHex(16);
  const mac = toHex(await crypto.subtle.sign("HMAC", rec.key, te.encode("CFLO-AUTH-1|" + sid + "|" + n + "|" + cn)));
  const a = { n, cn, key:rec.key, rec };
  setAttempt(a);
  const msg = te.encode("H:" + cn + ":" + mac);
  if(c.writeValueWithResponse) await c.writeValueWithResponse(msg); else await c.writeValue(msg);

  info = await readAuth(c);
  if(info.st === "1") return finish(info, a, sid, devKg);
  if(info.st === "2") return { status:"locked", info, wait: +info.wait || 0 };
  const rekeyed = rec.kg != null && devKg != null && rec.kg !== devKg;
  return { status:"rejected", info, rekeyed, fails: +info.f || 0 };
}

async function finish(info, a, sid, devKg){
  if(!HEX64.test(info.dp || "")) return { status:"spoof", info, msg:"no device proof" };
  const ok = await crypto.subtle.verify("HMAC", a.key, fromHex(info.dp), te.encode("CFLO-DEV-1|" + sid + "|" + a.n + "|" + a.cn));
  if(!ok) return { status:"spoof", info, msg:"device proof did not verify" };
  // Refresh the metadata (name / generation / last-used) — the CryptoKey itself is re-stored as-is.
  try{
    const rec = a.rec || await getRec(sid);
    if(rec) await putRec({ ...rec, name: info.name || rec.name || "", kg: devKg != null ? devKg : rec.kg, ts: Date.now() });
  }catch(e){}
  pending = null;
  return { status:"ok", info };
}

// ---------------------------------------------------------------- deep link + paste
const OWN = ["devkey", "sid", "kg"];

// Honor `#devkey=<32hex>&sid=<12hex>&kg=<n>` from a key QR. Strips ONLY its own params from the URL
// (synchronously, before any await) so an accompanying `#server=…&token=…` still reaches
// handlePairHash() (features/remote.js) — call this first. Safe to call on every hashchange.
export async function handleDevKeyHash(){
  const h = (location.hash || "").replace(/^#/, "");
  if(!h || !/(^|&)devkey=/.test(h)) return false;
  const p = {}, rest = [];
  h.split("&").forEach(kv => {
    const i = kv.indexOf("="), k = i > 0 ? kv.slice(0, i) : kv;
    if(OWN.includes(k)){ try{ p[k] = decodeURIComponent(kv.slice(i + 1)); }catch(e){ p[k] = ""; } }
    else if(kv) rest.push(kv);
  });
  try{ history.replaceState(null, "", location.pathname + location.search + (rest.length ? "#" + rest.join("&") : "")); }catch(e){}
  const r = parseKeyParams(p);
  if(r.error){ log("Scanner key link ignored: " + r.error); toast("Scanner key link ignored: " + r.error, { err:true }); return false; }
  return saveParsed(r);
}

function parseKeyParams(p){
  const sid = (p.sid || "").trim().toLowerCase(), key = (p.devkey || "").trim().toLowerCase();
  if(!HEX32.test(key)) return { error:"the key isn't 32 hex digits" };
  if(!HEX12.test(sid)) return { error:"the scanner id isn't 12 hex digits" };
  let kg = null;
  if(p.kg != null && p.kg !== ""){ if(!/^\d{1,9}$/.test(p.kg.trim())) return { error:"bad key generation" }; kg = +p.kg.trim(); }
  return { sid, keyHex:key, kg };
}

// Accepts: the full key URL (or just its `#devkey=…` part); "sid key" (either order, any separators,
// the key may be in the CYD's 4-digit groups); or the key alone while a scanner is pending (the one
// whose connection just asked for a key). Returns {sid, keyHex, kg} or {error}.
export function parsePasted(text){
  const t = (text || "").trim();
  if(!t) return { error:"nothing pasted" };
  if(/devkey=/i.test(t)){
    const frag = t.slice(t.search(/devkey=/i));
    const p = {};
    frag.split(/[&#?\s]/).forEach(kv => { const i = kv.indexOf("="); if(i > 0){ const k = kv.slice(0, i).toLowerCase(); try{ p[k] = decodeURIComponent(kv.slice(i + 1)); }catch(e){} } });
    return parseKeyParams(p);
  }
  const toks = t.toLowerCase().split(/[\s,;]+/).filter(Boolean);
  const sidTok = toks.findIndex(x => HEX12.test(x));
  if(sidTok >= 0){
    const key = toks.filter((_, i) => i !== sidTok).join("").replace(/[-:.]/g, "");
    if(!HEX32.test(key)) return { error:"after the 12-digit scanner id, expected the 32-digit key" };
    const sid = toks[sidTok];
    return { sid, keyHex:key, kg: pending && pending.sid === sid ? pending.kg : null };
  }
  const key = toks.join("").replace(/[-:.]/g, "");
  if(HEX32.test(key)){
    if(!pending) return { error:"a bare key needs a scanner — connect to it first, or paste the full key link / \"sid key\"" };
    return { sid:pending.sid, keyHex:key, kg:pending.kg };
  }
  return { error:"not a scanner key (expected the key link, \"sid key\", or the 32-digit key)" };
}

// Store a parsed key; toast + log (never the key itself). Returns true on success.
export async function saveParsed(r){
  try{
    const name = pending && pending.sid === r.sid ? pending.name : "";
    const gen = g => g == null ? "unknown" : String(g);
    const res = await storeKey(r.sid, r.keyHex, r.kg, name, old => askConfirm({
      title:"Replace scanner key?",
      msg:"This browser already holds a DIFFERENT key for " + (old.name || name || "scanner") + " (" + r.sid + "), " +
          "key gen " + gen(old.kg) + ". Replace it with the new key (gen " + gen(r.kg) + ")?\n\nOnly do this if you " +
          "just scanned this scanner's own QR / sticker, or you re-keyed it. A link from anywhere else could be an " +
          "attempt to swap in someone else's key.",
      ok:"Replace key", cancel:"Keep the old key", danger:true, kind:"replace" }));
    r.keyHex = "";   // drop the raw hex from the caller's object
    if(!res) throw new Error("invalid key");
    if(res === "cancelled"){ log("Scanner key NOT replaced (" + r.sid + ") — kept the stored key."); toast("Kept the stored key."); return false; }
    log("Scanner key " + (res === "same" ? "already stored" : "saved") + " for " + (name || r.sid) + (r.kg != null ? " (gen " + r.kg + ")" : "") + ".");
    toast("Scanner key " + (res === "same" ? "already stored" : "saved") + " for " + (name || r.sid) + ".");
    renderKeysUI();
    return true;
  }catch(e){
    log("Could not save the scanner key: " + e.message); toast("Could not save the scanner key: " + e.message, { err:true });
    return false;
  }
}
export async function addPasted(text){
  const r = parsePasted(text);
  if(r.error){ toast(r.error, { err:true }); return null; }
  return (await saveParsed(r)) ? r.sid : null;
}
export function pendingScanner(){ return pending; }

// ---------------------------------------------------------------- UI rendering (wiring lives in main.js)
// Settings → "Scanner keys": current link state + stored keys (name, sid, gen, Forget).
export async function renderKeysUI(){
  const st = $("keyLinkState");
  if(st){
    let t, cls = "";
    if(linkSec === "auth"){ t = "Connected — authenticated" + (linkInfo && linkInfo.name ? " to " + linkInfo.name : "") + (linkInfo && linkInfo.kg != null ? " (key gen " + linkInfo.kg + ")" : "") + "."; cls = "ok"; }
    else if(linkSec === "legacy"){ t = "Connected — UNSECURED firmware: this scanner has no phone-link authentication. Update its firmware."; cls = "warn"; }
    else t = lastFail || "Not connected.";
    st.textContent = t; st.className = "sub keystate " + cls;
  }
  const el = $("keyList"); if(!el) return;
  const keys = await listKeys();
  el.innerHTML = keys.length ? keys.map(k =>
    `<div class="sessrow keyrow"><span class="material-symbols-outlined">key</span>` +
    `<div class="sessmain"><span class="sessdate">${esc(k.name || "Scanner")}</span>` +
    `<span class="sesssub">${esc(k.sid)} · gen ${k.kg == null ? "?" : esc(String(k.kg))}</span></div>` +
    `<button class="danger keyforget" data-sid="${esc(k.sid)}">Forget</button></div>`).join("")
    : '<div class="sub" style="padding:6px;margin:0">No scanner keys stored.</div>';
}

// #authDlg: explain why the connection was refused and how to add the key.
export function openAuthDlg(r){
  const info = (r && r.info) || {};
  const who = info.name || info.sid || "This scanner";
  let msg;
  switch(r && r.status){
    case "nokey":    msg = who + " needs a phone key — this browser doesn't have one for it yet."; break;
    case "rejected": msg = r.rekeyed ? who + " was re-keyed (key generation " + info.kg + ") — the stored key is old. Add the new key."
                                     : who + " rejected the stored key. It may have been re-keyed; add the current key."; break;
    case "locked":   msg = who + " is refusing logins for " + (r.wait || "a few") + " s after too many failed attempts. Wait, then reconnect."; break;
    case "spoof":    msg = who + " did not prove it holds the key (" + (r.msg || "bad proof") + "). It may be an impostor — nothing was sent to it."; break;
    default:         msg = "Authentication failed: " + ((r && r.msg) || "unknown error") + ".";
  }
  lastFail = msg;
  $("authMsg").textContent = msg;
  $("authPaste").value = "";
  renderKeysUI();
  if(!$("authDlg").open) $("authDlg").showModal();
}
