// GATT identification results ("evt":"devinfo" log lines, fw >= 0.47.0, docs/gatt-identify.md), kept per MAC for
// this page session. Fed by the phone analysis engine (live copy of the current scan) and by the log parser (Explore /
// Map loads). Read by the live list (features/morelist.js), Explore rows and the device popup (features/gattview.js).
// NEW file (stale-module rule). Always imported WITHOUT a ?v= token, so every importer shares one instance.

const byMac = new Map();   // "AA:BB:.." -> {name, app, mfr, model, fw, hw, nw, auth, t}
const listeners = new Set();
let notifyTimer = null;

const str = v=>v == null ? "" : String(v);

// One parsed devinfo line. Later lines for the same MAC fill in / replace the fields they carry.
export function noteDevinfo(o, t){
  if(!o || o.evt !== "devinfo" || !o.mac) return;
  const mac = String(o.mac).toUpperCase();
  const old = byMac.get(mac) || {};
  const rec = { ...old };
  for(const k of ["name", "mfr", "model", "fw", "hw"]){
    const v = o[k] != null ? str(o[k]).trim() : "";
    if(v && v !== "?" && v !== "-") rec[k] = v;   // placeholders some devices return
  }
  if(typeof o.app === "number") rec.app = o.app;
  if(o.nw) rec.nw = true;
  if(o.auth) rec.auth = true;
  if(t && (!rec.t || t > rec.t)) rec.t = t;
  byMac.set(mac, rec);
  if(!notifyTimer) notifyTimer = setTimeout(()=>{ notifyTimer = null; listeners.forEach(fn=>{ try{ fn(); }catch(e){} }); }, 300);
}

export function devinfoFor(mac){ return byMac.get(String(mac || "").toUpperCase()) || null; }

// A name to show: the read name, else "<maker> <model>", else "" (same rule as the scanner's devinfo::nameOf).
export function devinfoName(mac){
  const r = devinfoFor(mac);
  if(!r) return "";
  if(r.name) return r.name;
  if(r.model) return (r.mfr ? r.mfr + " " : "") + r.model;
  return "";
}

// The name to show for a BLE device with this advertised name: the GATT name when the advert carries none, is only the
// start of it (a truncated local name), or is only a hex ID (8+ hex digits, e.g. GE bulbs "82D7FD3FAEC57791"); else the
// advert. Same rule as the scanner's groupName() (fw 0.47.9). Returns {name, gatt:bool}.
export function preferName(adv, mac){
  const a = adv || "", g = devinfoName(mac);
  if(g && (!a || (g.length > a.length && g.startsWith(a)) || /^[0-9A-Fa-f]{8,}$/.test(a))) return { name:g, gatt:true };
  return { name:a, gatt:false };
}

export function onDevinfo(fn){ listeners.add(fn); }
