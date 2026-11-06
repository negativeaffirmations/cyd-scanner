// BLE link encryption after the HMAC login (SEC-H3 Phase B/C) — see docs/ble-auth.md.
//
// The scanner asks for encryption once the phone has proved the key. A returning phone re-encrypts by
// itself in well under a second; a first connection makes Android show its pairing prompt, which only
// the owner can answer. Web Bluetooth can't see link security, so the AUTH read's enc= / pf= are the only
// signal, and they are polled here BEFORE anything else touches GATT (a background poll could collide
// with the app's own GATT operations).
//
// AUTH sec=0: nothing to do. sec=1: encryption is wanted but the link may go on without it. sec=2: the
// scanner ignores everything until the link is encrypted, so failing here means disconnecting.
//
// NEW file, so it is never served stale alongside a fresh connection.js. Its dialog is built here for the
// same reason: a cached index.html would not have it.
import { parseAuth } from './auth.js';
import { log } from '../util/dom.js';

const POLL_MS = 1000;
const QUIET_MS = 2500;      // a returning phone finishes inside this: no dialog for it
const DEADLINE_MS = 33000;  // just under the scanner's 35 s (ENC_TIMEOUT_MS), so the app reports first
const STALE_HINT_MS = 10000; // no prompt by now usually means the phone holds a pairing the scanner lost
const READ_MS = 3000;       // a read stuck behind pairing must not hold up Cancel or the deadline

const td = new TextDecoder();
const sleep = ms => new Promise(r => setTimeout(r, ms));

function promptDialog(name, required, onCancel){
  const d = document.createElement("dialog");
  const hdr = document.createElement("div"); hdr.className = "modalhdr";
  const title = document.createElement("strong"); title.textContent = "Pair with " + (name || "this scanner");
  hdr.append(title);
  const msg = document.createElement("div"); msg.className = "secmsg";
  msg.textContent = "Tap Pair on the Android Bluetooth prompt. If you don't see it, check your notifications. " +
    "You only do this once per scanner.";
  const row = document.createElement("div"); row.className = "fsactions dlgactions";
  const btn = document.createElement("button"); btn.className = "secondary";
  btn.textContent = required ? "Cancel" : "Skip for now";
  row.append(btn);
  d.append(hdr, msg, row);
  btn.addEventListener("click", onCancel);
  d.addEventListener("cancel", e => { e.preventDefault(); onCancel(); });
  document.body.append(d);
  d.showModal();
  d.hint = () => { msg.textContent = "No prompt? Open Android Bluetooth settings, forget " + (name || "this scanner") +
    ", then connect again. The scanner's pairing was probably cleared."; };
  return d;
}
const readTimeout = () => new Promise((_, rej) => setTimeout(() => rej(new Error("read timeout")), READ_MS));

// Resolves {ok, enc, sec, why, reauth}. why = "cancelled" | "failed" | "timeout" | "gone". ok=false on "gone",
// and at sec=2 on any failure (the caller disconnects); at sec=1 a failure resolves ok:true, enc:false.
// reauth = the scanner voided the proof made in the clear: the caller must authenticate again.
export async function ensureEncrypted(authChar, info, device){
  const sec = parseInt(info && info.sec, 10) || 0;
  if(sec < 1) return { ok:true, enc:false, sec };
  if(info.enc === "1") return { ok:true, enc:true, sec };
  const required = sec >= 2, t0 = Date.now();
  let dlg = null, cancelled = false, hinted = false, why = "timeout";
  while(Date.now() - t0 < DEADLINE_MS){
    if(!device.gatt.connected){ why = "gone"; break; }
    if(cancelled){ why = "cancelled"; break; }
    if(!dlg && Date.now() - t0 >= QUIET_MS) dlg = promptDialog(info.name, required, () => { cancelled = true; });
    if(dlg && !hinted && Date.now() - t0 >= STALE_HINT_MS){ hinted = true; dlg.hint(); }
    let v = null;
    // A read can fail while pairing is running: only the disconnect check above ends the wait.
    try{ v = parseAuth(td.decode(await Promise.race([authChar.readValue(), readTimeout()]))); }catch(e){}
    if(v && v.enc === "1"){
      if(dlg){ dlg.close(); dlg.remove(); }
      log("Link encrypted.");
      return { ok:true, enc:true, sec, reauth:v.st === "0" };
    }
    if(v && v.pf === "1"){ why = "failed"; break; }
    await sleep(dlg ? POLL_MS : 300);
  }
  if(dlg){ dlg.close(); dlg.remove(); }
  log("Link not encrypted ("+why+").");
  return { ok:!required && why !== "gone", enc:false, sec, why };
}

// The refusal shown when a sec=2 scanner could not be paired (auth.js showRefusal takes {title, msg}).
export function pairingRefusal(name, why){
  const who = name || "This scanner";
  if(why === "cancelled") return { title:"Not paired", msg:who + " needs Bluetooth pairing before it will share data. Connect again and tap Pair." };
  return { title:"Pairing failed",
    msg:who + " needs Bluetooth pairing and it did not complete. Connect again and tap Pair on the Android prompt.\n\n" +
        "If this keeps happening, open Android Bluetooth settings, forget " + (name || "the scanner") + ", and connect again. " +
        "That is needed when the scanner's pairing was cleared or its flash was erased." };
}
