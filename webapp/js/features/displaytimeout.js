// displaytimeout.js — Settings → Device(s) → "Display timeout" card (CYD fw >= 0.40.0).
// The scanner reports its setting in STATUS part 2 as dto=<mode>,<dimS>,<offS> (mode 0 always on / 1 dim only /
// 2 dim then off; 0 s = never) and takes CMD "V:<mode>:<dimS>:<offS>". Only a CYD sends dto=, so the card stays
// hidden on a mini or older firmware.
// Hooks: none in the old modules (stale-module rule). It polls state.status, which status.js keeps merged and
// clears on disconnect.
import { $, log } from '../util/dom.js';
import { enc } from '../config.js';
import { state } from '../state.js';

const IDS = ["dtoMode", "dtoDim", "dtoOff"];
let shown = "";           // the dto= value the selects show ("" = none reported: card hidden)
let sending = false;
let holdUntil = 0;        // after a change, keep the user's choice until the scanner's STATUS echoes it

function fmtS(s){
  if(!s) return "Never";
  if(s % 60 === 0){ const m = s / 60; return m + (m === 1 ? " minute" : " minutes"); }
  return s + " seconds";
}

// Make sure a value set elsewhere (CYD screen / older app) has an option to show.
function ensureOption(sel, v){
  if([...sel.options].some(o => o.value === String(v))) return;
  const o = document.createElement("option");
  o.value = String(v); o.textContent = fmtS(+v);
  sel.appendChild(o);
}

function applyEnables(){
  const mode = +$("dtoMode").value, on = !!(state.chars && state.chars.cmd) && shown !== "";
  $("dtoMode").disabled = !on || sending;
  $("dtoDim").disabled  = !on || sending || mode === 0;
  $("dtoOff").disabled  = !on || sending || mode !== 2;
}

function render(){
  const dto = (state.status && state.status.dto) || "";
  const card = $("dtoCard");
  if(!dto){ card.hidden = true; shown = ""; return; }
  card.hidden = false;
  const editing = IDS.includes(document.activeElement && document.activeElement.id);
  if(!shown) shown = "-";   // reported: enables on
  const mine = $("dtoMode").value + "," + $("dtoDim").value + "," + $("dtoOff").value;
  if(dto === mine) holdUntil = 0;   // echoed
  if(dto !== shown && !editing && !sending && Date.now() >= holdUntil){
    const [m, d, o] = dto.split(",");
    ensureOption($("dtoDim"), d); ensureOption($("dtoOff"), o);
    $("dtoMode").value = m; $("dtoDim").value = d; $("dtoOff").value = o;
    shown = dto;
  }
  applyEnables();
}

async function send(){
  if(!(state.chars && state.chars.cmd)) return;
  const cmd = "V:" + $("dtoMode").value + ":" + $("dtoDim").value + ":" + $("dtoOff").value;
  sending = true; applyEnables();
  try{ await state.chars.cmd.writeValue(enc.encode(cmd)); log("Display timeout set."); }
  catch(e){ log("Display timeout failed: " + e.message); }
  finally{ sending = false; holdUntil = Date.now() + 4000; render(); }   // then the scanner's STATUS dto= is the truth
}

export function initDisplayTimeout(){
  for(const id of IDS) $(id).addEventListener("change", () => { applyEnables(); send(); });
  render();
  setInterval(render, 1000);
}
