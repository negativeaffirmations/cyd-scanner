// gattid.js — Settings → Device(s) → "Identify nearby devices" card (fw >= 0.47.0, docs/gatt-identify.md).
// The scanner reports the setting in STATUS part 2 as gatt=<0|1> and takes CMD "C:<0|1>" (saved on the scanner). Older
// firmware sends no gatt=, so the card stays hidden. Hooks: none in the old modules (stale-module rule): it polls
// state.status, which status.js keeps merged and clears on disconnect.
import { $, log } from '../util/dom.js';
import { enc } from '../config.js';
import { state } from '../state.js';

let holdUntil = 0;   // after a change, keep the user's choice until the scanner's STATUS echoes it

function render(){
  const card = $("gattCard"), box = $("gattOn");
  if(!card || !box) return;
  const v = state.connected && state.status ? state.status.gatt : undefined;
  card.style.display = v === "0" || v === "1" ? "" : "none";
  if(v !== "0" && v !== "1") return;
  box.disabled = false;
  if(Date.now() >= holdUntil) box.checked = v === "1";
}

export function initGattId(){
  const box = $("gattOn");
  if(box) box.addEventListener("change", async ()=>{
    if(!state.connected || !state.chars.cmd) return;
    holdUntil = Date.now() + 5000;
    try{ await state.chars.cmd.writeValue(enc.encode("C:" + (box.checked ? "1" : "0"))); log("Device identification " + (box.checked ? "on." : "off.")); }
    catch(e){ holdUntil = 0; log("Could not change device identification: " + e.message); render(); }
  });
  setInterval(render, 1000);
  render();
}
