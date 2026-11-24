// Phone analysis, increment 3d: who made a device, from the full public lists (docs/phone-analysis.md phase 3).
// data/oui.tsv (IEEE MA-L, 24-bit prefixes) and data/cid.tsv (Bluetooth SIG company identifiers), built by
// tools/vendordb/build.py. ~1.3 MB, so they are fetched only the first time something asks (a device popup), not on page
// load. A randomized / locally set address (bit 1 of the first byte) has no maker: it returns "".
// Reached through state.vendors (one instance; see engine.js for why). NEW file (stale-module rule).
import { state } from '../state.js';

let oui = null, cid = null, loading = null;
const listeners = new Set();

async function loadTsv(url){
  const r = await fetch(url, { cache:"force-cache" });
  if(!r.ok) throw new Error("HTTP " + r.status);
  const m = new Map();
  for(const ln of (await r.text()).split("\n")){
    const i = ln.indexOf("\t");
    if(i > 0) m.set(ln.slice(0, i), ln.slice(i + 1).replace(/\r$/, ""));   // CRLF-safe
  }
  return m;
}
function load(){
  if(!loading) loading = Promise.all([loadTsv("data/oui.tsv"), loadTsv("data/cid.tsv")])
    .then(([o, c])=>{ oui = o; cid = c; listeners.forEach(fn=>{ try{ fn(); }catch(e){} }); })
    .catch(()=>{ loading = null; });   // try again next time
  return loading;
}

// Maker of a MAC from the IEEE list; "" when unknown / random, null while the list is still loading.
export function makerFor(mac){
  const m = String(mac || "").toUpperCase().replace(/[^0-9A-F]/g, "");
  if(m.length < 6) return "";
  if(parseInt(m.slice(0, 2), 16) & 0x02) return "";   // locally administered: made up, not assigned
  if(!oui){ load(); return null; }
  return oui.get(m.slice(0, 6)) || "";
}
// Company behind a BLE manufacturer ID ("004C" / "4c" / 76); "" when unknown, null while loading.
export function companyFor(id){
  if(id === undefined || id === null || id === "") return "";
  const k = (typeof id === "number" ? id.toString(16) : String(id)).toUpperCase().replace(/^0X/, "").padStart(4, "0");
  if(!cid){ load(); return null; }
  return cid.get(k) || "";
}

export function initVendors(){
  state.vendors = { makerFor, companyFor, onVendorsLoaded:fn=>listeners.add(fn) };
}
