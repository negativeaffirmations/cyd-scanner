// Remember each scanner's fleet role (STATUS flt=<role>,…: 0 off / 1 client / 2 leader) by device id, so the
// Android app's scanner chooser can put the fleet leader first (webapp 2026-11-26). On the 2026-11-26 drive the
// phone joined a member first and the leader + the other member logged ~1 min with no position; fw 0.47.5 also
// relays a member's phone fix, but the leader stays the right unit for the phone (its fleet file, live copy, alerts).
// Read by android-app/native/shim-bluetooth.js (same key). NEW file, wired from main.js (stale-module rule).
import { state } from '../state.js';

export const ROLES_KEY = "cflo.unitRoles";

function save(){
  const d = state.device, f = state.status && state.status.flt;
  if(!d || !d.id || !d.gatt || !d.gatt.connected || typeof f !== "string" || !f) return;
  const role = parseInt(f.split(",")[0], 10);
  if(!(role >= 0 && role <= 2)) return;
  let m = {};
  try{ m = JSON.parse(localStorage.getItem(ROLES_KEY) || "{}") || {}; }catch(e){}
  if(m[d.id] === role) return;
  m[d.id] = role;
  try{ localStorage.setItem(ROLES_KEY, JSON.stringify(m)); }catch(e){}
}

export function initUnitRole(){ setInterval(save, 3000); }
