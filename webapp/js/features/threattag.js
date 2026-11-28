// Threat shown in its scan row (owner idea 2026-11-27 "mark threats in scan list"). A row that counts as a threat gets a
// pill naming it in its category colour (CAT_COL, like the Home cards): "FLOCK · suspect"; a threat with no category
// (Other) shows its tier alone. A row that raised a phone notification this session also carries a bell, or the
// "probably false" mark when the paired server explained it (features/screen.js; label, never hide).
// Used by the live list and the Explore viewer (features/detections.js renderDetList). NEW file (stale-module rule).
// state.alerted (Map MAC -> {t, low, kinds}) is written by features/notify.js when an alert goes out.
import { state } from '../state.js';
import { esc as escStr } from '../util/dom.js';
const esc = v => escStr(String(v == null ? "" : v)).replace(/"/g, "&quot;").replace(/'/g, "&#39;");   // also safe inside attributes
import { CAT_COL, TIER_COL, TIER_NAMES } from '../config.js';
import { tierNum } from '../util/format.js';

const CAT_NAMES = ["","Flock","Axon","ALPR","Cam","Ring","Raven","Glass","Tracker","Drone","Deauth","Flipper","Skim","Other"];
const CAT_SHOWN = 12;               // 13 = Other: no pill of its own, the tier speaks
const ALERT_SHOW_MS = 30 * 60000;   // a bell stays on a row this long after its alert
const normMac = m => String(m || "").trim().toUpperCase();

export function alertOf(mac){
  const a = state.alerted && state.alerted.get(normMac(mac));
  return a && Date.now() - a.t < ALERT_SHOW_MS ? a : null;
}

// HTML for the row (empty when the row isn't a threat and didn't alert).
export function threatTag(r){
  const tn = typeof r.tier === "number" ? r.tier : tierNum(r.tier);
  const cat = r.cat && r.cat <= CAT_SHOWN ? r.cat : 0;
  const a = alertOf(r.mac);
  if(!tn && !cat && !a) return "";
  let html = "";
  if(tn || cat){
    const col = cat ? CAT_COL[cat] : (TIER_COL[tn] || "");
    const label = (cat ? CAT_NAMES[cat] : "Threat") + (tn ? " · " + TIER_NAMES[tn] : "");
    const sig = r.sig || r.signature || "";                      // live rows: sig; Explore (parse.js) rows: signature
    const why = sig ? label + " (" + sig + ")" : label;     // the signature that matched, on long-press / hover
    html += `<span class="ttag"${col ? ` style="--tc:${col}"` : ""} title="${esc(why)}">${esc(label)}</span>`;
    if(sig) html += `<span class="tsig">${esc(sig)}</span>`;
  }
  if(a) html += a.low
    ? `<span class="talert low" title="Alert sent, marked probably a false alarm">probably false</span>`
    : `<span class="talert material-symbols-outlined" title="Alert sent: ${esc(a.kinds.join(", "))}">notifications_active</span>`;
  return html;
}
