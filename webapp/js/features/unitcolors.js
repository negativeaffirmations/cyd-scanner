// Per-scanner colours (owner idea 2026-11-27 "color coded fleet devices"): every scanner unit gets ONE colour, keyed by
// its scanner id (STATUS sid, 12 hex), used wherever units appear: the live list's "seen by" chips, the device popup,
// the Home card name, the Scanners dialog, the Fleet member list and Scanner keys. Assigned on first sight (the first
// palette colour no other unit has) and kept on this phone (localStorage cflo.unitColors); tap a unit's dot in Settings ->
// Scanner keys to pick another. NEW file (stale-module rule): older modules only import from it.
//
// Name -> sid is learned too (cflo.unitSids: names + Bluetooth device ids), so lists that only know a unit's name or
// device id (the Scanners dialog) can still colour it.
import { state } from '../state.js';
import { esc } from '../util/dom.js';

export const UNIT_PALETTE = [
  "#58a6ff",  // blue
  "#3fb950",  // green
  "#f0883e",  // orange
  "#f778ba",  // pink
  "#a371f7",  // purple
  "#39c5cf",  // cyan
  "#e3b341",  // gold
  "#c9d1d9",  // silver
];
const UNKNOWN = "#6e7681";
const COLORS_KEY = "cflo.unitColors", SIDS_KEY = "cflo.unitSids";
const HEX12 = /^[0-9a-f]{12}$/;

function readJson(k){ try{ return JSON.parse(localStorage.getItem(k) || "{}") || {}; }catch(e){ return {}; } }
function writeJson(k, v){ try{ localStorage.setItem(k, JSON.stringify(v)); }catch(e){} }
// Also the resolved hex per sid (cflo.unitHex): the Android app's own chooser (android-app/native) reads that, no palette copy.
function saveColors(){
  writeJson(COLORS_KEY, colors);
  const hex = {}; for(const [s, i] of Object.entries(colors)) hex[s] = UNIT_PALETTE[i] || UNKNOWN;
  writeJson("cflo.unitHex", hex);
}
let colors = readJson(COLORS_KEY);          // sid -> palette index
let sids = readJson(SIDS_KEY);              // "n:<name>" / "d:<device id>" -> sid
const listeners = new Set();
export function onUnitColorsChange(fn){ listeners.add(fn); }
function changed(){
  listeners.forEach(fn=>{ try{ fn(); }catch(e){} });
  window.dispatchEvent(new Event("cflo-unitcolors"));
}

const normSid = s => String(s || "").toLowerCase();
// Palette index for sid, assigning the first free one on first sight. null = not a scanner id.
function indexFor(sid){
  sid = normSid(sid);
  if(!HEX12.test(sid)) return null;
  if(colors[sid] != null) return colors[sid];
  const used = new Set(Object.values(colors));
  let i = 0;
  while(i < UNIT_PALETTE.length && used.has(i)) i++;
  if(i >= UNIT_PALETTE.length){            // more units than colours: the least used one
    const n = UNIT_PALETTE.map((_, k)=>Object.values(colors).filter(v=>v === k).length);
    i = n.indexOf(Math.min(...n));
  }
  colors[sid] = i; saveColors();
  return i;
}
export function unitColor(sid){ const i = indexFor(sid); return i == null ? UNKNOWN : UNIT_PALETTE[i]; }
export function setUnitColor(sid, i){
  sid = normSid(sid);
  if(!HEX12.test(sid) || !(i >= 0 && i < UNIT_PALETTE.length) || colors[sid] === i) return;
  // A colour another unit has: the two SWAP, so no two scanners ever share one.
  const mine = indexFor(sid);
  const moved = [[sid, i]];
  for(const [s, v] of Object.entries(colors)) if(v === i && s !== sid){ colors[s] = mine; moved.push([s, mine]); }
  colors[sid] = i; saveColors();
  // repaint every dot already on screen (lists that only redraw on their own events)
  for(const [s, k] of moved)
    document.querySelectorAll(`[data-usid="${s}"]`).forEach(el=>{ el.style.background = UNIT_PALETTE[k]; });
  changed();
}

// Learn which sid a name / device id belongs to (any list that knows both calls this; also the 1 s watcher below).
export function learnUnit(sid, name, deviceId){
  sid = normSid(sid);
  if(!HEX12.test(sid)) return;
  let dirty = false;
  if(name && sids["n:" + name] !== sid){ sids["n:" + name] = sid; dirty = true; }
  if(deviceId && sids["d:" + deviceId] !== sid){ sids["d:" + deviceId] = sid; dirty = true; }
  const fresh = colors[sid] == null;
  indexFor(sid);
  if(dirty) writeJson(SIDS_KEY, sids);
  if(dirty || fresh) changed();
}
export const sidForName = name => (name && sids["n:" + name]) || "";
export const sidForDevice = id => (id && sids["d:" + id]) || "";

// Colour of the unit behind a DETS `via` bit: 0 = the connected scanner, k = fleet member slot k.
export function viaColor(k){
  if(k === 0) return unitColor(state.scannerId);
  const m = (state.fleetMembers || []).find(x => x.slot === k);
  return m && m.sid ? unitColor(m.sid) : UNKNOWN;
}

// Small round dot (optional letter) in the unit's colour. sid may be "" (grey).
export function unitDot(sid, opts = {}){
  const c = sid ? unitColor(sid) : UNKNOWN;
  const letter = opts.letter ? esc(opts.letter) : "";
  const title = opts.title ? ` title="${esc(opts.title)}"` : "";
  const act = opts.pick && sid ? ` data-unitpick="${esc(normSid(sid))}" role="button" tabindex="0" aria-label="Change colour"` : "";
  const ds = sid ? ` data-usid="${esc(normSid(sid))}"` : "";
  return `<span class="unitdot${letter ? " lt" : ""}${act ? " pick" : ""}" style="background:${c}"${ds}${title}${act}>${letter}</span>`;
}

// ---- colour picker (opened from a dot carrying data-unitpick) ----
let dlg = null, dlgSid = "";
function ensureDlg(){
  if(dlg) return dlg;
  dlg = document.createElement("dialog");
  dlg.id = "unitColorDlg";
  dlg.innerHTML = `<div class="modalhdr"><strong id="ucTitle">Scanner colour</strong>` +
    `<button class="xbtn" id="ucClose" aria-label="Close">&times;</button></div>` +
    `<div class="sub" style="margin:0 0 .7em">Used for this scanner everywhere in the app: the scan list, the device popup, Fleet and Scanners. A colour another scanner has swaps the two. Kept on this phone.</div>` +
    `<div class="ucgrid" id="ucGrid"></div>`;
  document.body.appendChild(dlg);
  dlg.querySelector("#ucClose").addEventListener("click", ()=>dlg.close());
  dlg.addEventListener("click", ev=>{
    const b = ev.target.closest("[data-uci]");
    if(b){ setUnitColor(dlgSid, +b.dataset.uci); dlg.close(); }
    else if(ev.target === dlg) dlg.close();          // tap on the backdrop
  });
  return dlg;
}
export function openUnitColorPicker(sid, name){
  sid = normSid(sid);
  if(!HEX12.test(sid)) return;
  const d = ensureDlg();
  dlgSid = sid;
  d.querySelector("#ucTitle").textContent = (name || "Scanner") + " colour";
  const cur = indexFor(sid);
  const owner = i => Object.entries(colors).filter(([s, v])=>v === i && s !== sid).map(([s])=>{
    const n = Object.entries(sids).find(([k, v])=>v === s && k.startsWith("n:"));
    return (n ? n[0].slice(2) : s).replace(/^CFLO-(MINI-|CYD-)?/i, "");
  });
  d.querySelector("#ucGrid").innerHTML = UNIT_PALETTE.map((c, i)=>{
    const o = owner(i);
    return `<button class="ucbtn${i === cur ? " cur" : ""}" data-uci="${i}" style="--uc:${c}">` +
      `<span class="unitdot" style="background:${c}"></span><span class="uctxt">${i === cur ? "Current" : o.length ? esc(o.join(", ")) : "Free"}</span></button>`;
  }).join("");
  if(!d.open) d.showModal();
}

// Called once from main.js: learn the connected unit (sid, name, device id) and the fleet members each second, and open
// the picker from any dot that carries data-unitpick.
export function initUnitColors(){
  saveColors();                                  // the hex map for the app chooser, also for colours assigned before it existed
  let homeSid = null;
  setInterval(()=>{
    if(state.connected && state.scannerId) learnUnit(state.scannerId, state.scannerName, state.device && state.device.id);
    for(const m of state.fleetMembers || []) if(m.sid) learnUnit(m.sid, m.name);
    // Home card: a dot before the connected unit's name (#unitDot sits beside #unitName, which ble/status.js writes)
    const hd = document.getElementById("unitDot");
    const sid = state.connected && state.scannerId ? normSid(state.scannerId) : "";
    if(hd && sid !== homeSid){ homeSid = sid; hd.innerHTML = sid ? unitDot(sid, { pick:true, title:"Scanner colour" }) : ""; }
  }, 1000);
  const pick = ev=>{
    const el = ev.target.closest && ev.target.closest("[data-unitpick]");
    if(!el) return;
    if(ev.type === "keydown" && ev.key !== "Enter" && ev.key !== " ") return;
    ev.preventDefault(); ev.stopPropagation();
    const row = el.closest(".sessrow");
    const name = row && row.querySelector(".sessdate") ? row.querySelector(".sessdate").textContent
      : normSid(state.scannerId) === el.dataset.unitpick ? (state.scannerName || "") : "";
    openUnitColorPicker(el.dataset.unitpick, name);
  };
  document.addEventListener("click", pick, true);
  document.addEventListener("keydown", pick, true);
}
