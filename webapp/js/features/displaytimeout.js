// displaytimeout.js — Settings → Device(s) → "Display timeout" card (CYD fw >= 0.40.0).
// The scanner reports its setting in STATUS part 2 as dto=<mode>,<dimS>,<offS> (mode 0 always on / 1 dim only /
// 2 dim then off; 0 s = never) and takes CMD "V:<mode>:<dimS>:<offS>". Only a CYD sends dto=, so the card stays
// hidden on a mini or older firmware.
// Each time dropdown ends in "Custom…": it opens a row (number + seconds/minutes + Set / Cancel); nothing is sent
// until Set. The firmware accepts 5 s .. 7200 s (clampDispS in src/cyd/main.cpp).
// Hooks: none in the old modules (stale-module rule). It polls state.status, which status.js keeps merged and
// clears on disconnect.
import { $, log } from '../util/dom.js';
import { enc } from '../config.js';
import { state } from '../state.js';

const IDS = ["dtoMode", "dtoDim", "dtoOff"];
const CUSTOM = { dtoDim: "dtoDimCustom", dtoOff: "dtoOffCustom" };   // select id -> its custom row
const MIN_S = 5, MAX_S = 7200;
let shown = "";           // the dto= value the selects show ("" = none reported: card hidden)
let sending = false;
let holdUntil = 0;        // after a change, keep the user's choice until the scanner's STATUS echoes it
const prev = { dtoDim: "30", dtoOff: "120" };   // the confirmed value, restored by Cancel

function fmtS(s){
  if(!s) return "Never";
  if(s % 60 === 0){ const m = s / 60; return m + (m === 1 ? " minute" : " minutes"); }
  return s + " seconds";
}

// Make sure a value set elsewhere (custom, CYD screen) has an option to show; kept above "Custom…".
function ensureOption(sel, v){
  v = String(v);
  if([...sel.options].some(o => o.value === v)) return;
  const o = document.createElement("option");
  o.value = v; o.textContent = fmtS(+v);
  const c = [...sel.options].find(x => x.value === "c");
  sel.insertBefore(o, c || null);
}

const isOpen = id => !$(CUSTOM[id]).hidden;
const anyOpen = () => Object.keys(CUSTOM).some(isOpen);

function showErr(msg){ const e = $("dtoCustomErr"); e.textContent = msg || ""; e.hidden = !msg; }

function openCustom(id){
  const v = +prev[id] || 0;
  const mins = v && v % 60 === 0;
  $(id + "Num").value = v ? (mins ? v / 60 : v) : "";
  $(id + "Unit").value = mins || !v ? "60" : "1";
  $(CUSTOM[id]).hidden = false;
  showErr("");
  $(id + "Num").focus();
}
function closeCustom(id, revert){
  $(CUSTOM[id]).hidden = true;
  if(revert) $(id).value = prev[id];
  showErr("");
}

function applyEnables(){
  const mode = +$("dtoMode").value, on = shown !== "";
  const offFor = { dtoDim: mode === 0, dtoOff: mode !== 2 };
  $("dtoMode").disabled = !on || sending;
  for(const id of Object.keys(CUSTOM)){
    if(offFor[id] && isOpen(id)) closeCustom(id, true);   // the mode no longer uses this time
    $(id).disabled = !on || sending || offFor[id];
    for(const s of ["Num", "Unit", "Set", "Cancel"]) $(id + s).disabled = !on || sending;
  }
}

function render(){
  const dto = (state.status && state.status.dto) || "";
  const card = $("dtoCard");
  if(!dto){
    card.hidden = true; shown = "";
    for(const id of Object.keys(CUSTOM)) closeCustom(id, false);
    return;
  }
  card.hidden = false;
  if(!shown) shown = "-";   // reported: enables on
  const mine = $("dtoMode").value + "," + prev.dtoDim + "," + prev.dtoOff;
  if(dto === mine) holdUntil = 0;   // echoed
  const editing = IDS.includes(document.activeElement && document.activeElement.id) || anyOpen();
  if(dto !== shown && !editing && !sending && Date.now() >= holdUntil){
    const [m, d, o] = dto.split(",");
    ensureOption($("dtoDim"), d); ensureOption($("dtoOff"), o);
    $("dtoMode").value = m; $("dtoDim").value = d; $("dtoOff").value = o;
    prev.dtoDim = d; prev.dtoOff = o;
    shown = dto;
  }
  applyEnables();
}

async function send(){
  if(!(state.chars && state.chars.cmd)) return;
  const cmd = "V:" + $("dtoMode").value + ":" + prev.dtoDim + ":" + prev.dtoOff;
  sending = true; applyEnables();
  try{ await state.chars.cmd.writeValue(enc.encode(cmd)); log("Display timeout set."); }
  catch(e){ log("Display timeout failed: " + e.message); }
  finally{ sending = false; holdUntil = Date.now() + 4000; render(); }   // then the scanner's STATUS dto= is the truth
}

function setCustom(id){
  const n = Number($(id + "Num").value), unit = +$(id + "Unit").value;
  const s = Math.round(n * unit);
  if(!Number.isFinite(n) || n <= 0 || s < MIN_S || s > MAX_S){
    showErr("Enter a time from 5 seconds to 120 minutes.");
    $(id + "Num").focus();
    return;
  }
  ensureOption($(id), s);
  $(id).value = String(s);
  prev[id] = String(s);
  closeCustom(id, false);
  send();
}

export function initDisplayTimeout(){
  $("dtoMode").addEventListener("change", () => { applyEnables(); send(); });
  for(const id of Object.keys(CUSTOM)){
    $(id).addEventListener("change", () => {
      if($(id).value === "c"){ openCustom(id); return; }   // nothing is sent until Set
      closeCustom(id, false);
      prev[id] = $(id).value;
      send();
    });
    $(id + "Set").addEventListener("click", () => setCustom(id));
    $(id + "Cancel").addEventListener("click", () => closeCustom(id, true));
    $(id + "Num").addEventListener("keydown", e => { if(e.key === "Enter"){ e.preventDefault(); setCustom(id); } });
  }
  render();
  setInterval(render, 1000);
}
