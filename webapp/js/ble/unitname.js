// unitname.js — scanner names where the advert can't give one.
// From fw 0.33.0 every unit advertises as plain "CFLO" and its AUTH read only names it at st=1 on an encrypted
// link, and a key added from a pairing QR carries no name. So a key stored that way stays "Scanner" in Settings
// and in the Fleet add list. The name does reach us later from trusted sources: the authenticated unit's own
// STATUS `name=`, and the leader's fleet member list. learnKeyName() writes it into the existing key record.
// It never creates a record and never touches the key itself.
// (A NEW file on purpose: a stale cached auth.js can't break an import of these exports. See CLAUDE.md.)
import { state } from '../state.js';
import { unitNameFor } from './devices.js';
import { renderKeysUI } from './auth.js';

const DB_NAME = "cflo-keys", STORE = "keys";   // the store auth.js owns (same name, version and key path)
const HEX12 = /^[0-9a-f]{12}$/;
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
  dbP.catch(() => { dbP = null; });
  return dbP;
}

// A unit name as the firmware makes them (CFLO-…, printable, short). Anything else is ignored.
export function validUnitName(n){ return typeof n === "string" && n.length >= 3 && n.length <= 32 && /^[\x21-\x7e]+$/.test(n); }

// Name the stored key for `sid`, if one exists and its name differs. Resolves true when a record changed.
const learned = new Map();   // sid -> name already checked this page session (STATUS repeats it on every push)
export async function learnKeyName(sid, name){
  sid = (sid || "").toLowerCase();
  if(!HEX12.test(sid) || !validUnitName(name) || learned.get(sid) === name) return false;
  learned.set(sid, name);
  let changed = false;
  try{
    const d = await db();
    changed = await new Promise((res, rej) => {
      const t = d.transaction(STORE, "readwrite"), s = t.objectStore(STORE);
      let did = false;
      const g = s.get(sid);
      g.onsuccess = () => {
        const rec = g.result;
        if(rec && rec.key && rec.name !== name){ s.put({ ...rec, name }); did = true; }
      };
      t.oncomplete = () => res(did);
      t.onerror = () => rej(t.error); t.onabort = () => rej(t.error);
    });
  }catch(e){ return false; }
  if(changed){ try{ await renderKeysUI(); }catch(e){} }
  return changed;
}

// Name several keys at once (fleet member list). One UI refresh at the end.
export async function learnKeyNames(pairs){
  let any = false;
  for(const p of pairs || []) if(await learnKeyName(p.sid, p.name)) any = true;
  return any;
}

// The best label for the connected unit: its STATUS name, the name remembered for this device, else the advert name.
// Pass {remembered:true} before STATUS has arrived (connecting): state.scannerName may still be the previous unit's.
export function unitLabel(o){
  const dev = state.device;
  const fromStatus = o && o.remembered ? "" : state.scannerName;
  return fromStatus || (dev && unitNameFor(dev.id)) || (dev && dev.name) || "scanner";
}
