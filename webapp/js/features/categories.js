// Threat-category breakdown (mirrors the CYD HOME readout): one chip per category with a live
// threat device, coloured by its worst tier, severity-sorted. Tap a chip -> device list modal ->
// device detail modal (reuses the detDlg modal; no per-detection GPS in the live stream).
import { state } from '../state.js';
import { $, esc } from '../util/dom.js';
import { tierNum, flagBadges } from '../util/format.js';
import { TIER_COL, TIER_NAMES, FLAG_BADGES, CAT_COL } from '../config.js';
import { fillDetailWhitelist } from './whitelist.js';
import { renderDetailMap } from './detail.js';
import { getSightings, haveAnyFix } from './sightings.js';

// DETS v5 category id -> name (must match the CYD's Category enum order).
const CAT_NAMES = ["","Flock","Axon","ALPR","Cam","Ring","Raven","Glass","Tracker","Drone","Deauth","Flipper","Skim","Other"];
// What signatures each category matches (shown atop the category device list).
const CAT_DESC = ["",
  "Flock OUIs, SoftAP SSID, GATT UUID",
  "Axon OUIs + name prefixes (AB#/AXON)",
  "ALPR OUIs (Motorola / Genetec)",
  "Camera OUIs (Wyze / Hikvision / Axis / Arlo)",
  "Ring camera / doorbell OUIs",
  "Raven BLE service UUIDs (0x3100–0x3500)",
  "Smart-glasses BLE company IDs / UUIDs (Meta / Snap)",
  "Find My + tracker BLE UUIDs (Tile / AirTag / SmartTag)",
  "OpenDroneID presence (BLE 0xFFFA / Wi-Fi IE)",
  "Deauth frames / evil-twin APs / pwnagotchi",
  "Flipper Zero OUI / BLE UUID / name",
  "Card-skimmer BLE modules",
  "Suspected device, no specific category match"];

const rowTier = r => typeof r.tier === "number" ? r.tier : tierNum(r.tier);

// Threat categories shown as cards: the full fixed taxonomy Flock..Skim (ids 1..12), always, 3 per
// row — exactly like the CYD HOME readout (Other=13 is not carded). Each card's outline is its
// category colour at ALL times (so the user associates a hue with each threat before ever seeing
// one); the fill is grey at 0 and a tint of that colour when the count is > 0.
const CAT_SHOWN = 12;
export function renderCatChips(rows){
  const el = $("catChips"); if (!el) return;
  const cnt = {};
  (rows||[]).forEach(r=>{
    const tn = rowTier(r), c = r.cat||0;
    if (r.muted || tn < 1 || !c || c > CAT_SHOWN) return;   // threats only, categorized, not whitelisted
    cnt[c] = (cnt[c]||0) + 1;
  });
  let html = "";
  for (let c = 1; c <= CAT_SHOWN; c++){
    const n = cnt[c]||0, col = CAT_COL[c]||"#8b949e", hot = n > 0;
    html += `<button class="tcard${hot?" hot":""}" data-cat="${c}" `+
      `style="border-color:${col};${hot?`background:${col}22;`:""}">`+
      `<span class="tn">${esc(CAT_NAMES[c]||"?")}</span>`+
      `<span class="tc" style="color:${hot?col:""}">${n}</span></button>`;
  }
  el.style.display = "grid";
  el.innerHTML = html;
  el.querySelectorAll(".tcard").forEach(b=>{ b.onclick = ()=> openCatModal(+b.dataset.cat); });
}
function catRows(catId){
  return state.lastDetRows.filter(r=> (r.cat||0)===catId && !r.muted && rowTier(r)>=1)
    .sort((a,b)=> (rowTier(b)-rowTier(a)) || (b.rssi-a.rssi));
}
export function openCatModal(catId){
  const rows = catRows(catId);
  $("catTitle").textContent = (CAT_NAMES[catId]||"Category") + " — " + rows.length + " device(s)";
  $("catDesc").textContent = CAT_DESC[catId] || "";
  const el = $("catList");
  if (!rows.length){
    el.innerHTML = '<div class="sub" style="padding:6px">No devices in this category right now.</div>';
  } else {
    el.innerHTML = rows.map((r,ri)=>{
      const tn = rowTier(r), nm = esc(r.name) || esc(r.mac) || "&lt;hidden&gt;";
      return `<div class="det clk" data-ri="${ri}"><span class="tiercol"><span class="badge b${tn}"></span></span>`+
             `<span class="nm t${tn}">${nm}${flagBadges(r.flags||0)}</span>`+
             `<span class="rssi">${r.rssi} dBm</span></div>`;
    }).join("");
    el.querySelectorAll(".det[data-ri]").forEach(d=>{ d.onclick = ()=> openLiveDetail(rows[+d.dataset.ri]); });
  }
  $("catDlg").showModal();
}
// Live-detection detail (reuses the detDlg modal). The live DETS stream carries no per-detection
// GPS, so the map plots this device's SIGHTING TRAIL — the phone's own positions each time the MAC
// was seen this session (see sightings.js). Shown when a live row OR a category-modal row is tapped;
// carries the whitelist add/manage action.
export function openLiveDetail(r){
  const tn = rowTier(r);
  const sig = (r.sources||[]).slice().sort((a,b)=>b.rssi-a.rssi).map(s=>s.tag+" "+s.rssi+" dBm").join(", ") || "?";
  const rowsKv = [["Name", r.name||r.mac||"<hidden>"], ["MAC", r.mac],
    ["Category", CAT_NAMES[r.cat||0] || "Other"],
    ["Signals", sig],
    ["Strongest RSSI", r.rssi + " dBm"], ["Threat tier", TIER_NAMES[tn] || "none"]];
  if (r.muted) rowsKv.push(["Whitelisted","yes (muted)"]);
  if (r.cid) rowsKv.push(["BLE company", r.cid]);
  if (r.flags) rowsKv.push(["Flags", FLAG_BADGES.filter(f=>r.flags&f[0]).map(f=>f[2]).join(", ")]);
  if (r.ie && !/^0+$/.test(r.ie)) rowsKv.push(["IE fingerprint", r.ie]);
  const rid = (state.rids||{})[String(r.mac||"").toLowerCase()];   // DETS v6 Remote-ID, if this MAC is an emitter
  if (rid){
    if (rid.uas) rowsKv.push(["Remote-ID · UAS ID", rid.uas]);
    if (rid.oid) rowsKv.push(["Remote-ID · Operator ID", rid.oid]);
    if (rid.h != null) rowsKv.push(["Remote-ID · Height", rid.h + " m"]);
    if (rid.alt != null) rowsKv.push(["Remote-ID · Geo alt", rid.alt + " m"]);
    if (rid.spd != null) rowsKv.push(["Remote-ID · Speed", rid.spd.toFixed(1) + " m/s"]);
    if (rid.hdg != null) rowsKv.push(["Remote-ID · Heading", rid.hdg + "°"]);
  }
  $("detInfo").innerHTML = rowsKv.map(([k,v])=>
    `<div class="kv"><span class="k">${k}</span><span class="vv t${k==="Threat tier"?tn:0}">${esc(String(v))}</span></div>`).join("");
  fillDetailWhitelist(r.mac, r.muted);
  const pts = getSightings(r.mac);
  const msg = pts.length ? "" : (haveAnyFix()
    ? "No GPS-tagged sightings yet — move while scanning to map where this device is seen."
    : "Enable GPS (Sync time & GPS in Settings) to map where devices are seen.");
  renderDetailMap(pts, msg, rid ? {dlat:rid.dlat, dlon:rid.dlon, olat:rid.olat, olon:rid.olon} : undefined);
  $("detDlg").showModal();
}

function countCats(rows){
  const cnt = {};
  (rows||[]).forEach(r=>{ const tn = rowTier(r), c = r.cat||0;
    if (r.muted || tn < 1 || !c || c > CAT_SHOWN) return; cnt[c] = (cnt[c]||0) + 1; });
  return cnt;
}
// Compact Scanner threat strip: a small color-coded pill for EVERY category (count shown when >0, dim
// at 0), in one horizontally-scrollable row. The whole row is one tap target -> the breakdown modal.
export function renderScanCounts(rows){
  const el = $("scanCounts"); if (!el) return;
  const cnt = countCats(rows);
  let html = "";
  for (let c = 1; c <= CAT_SHOWN; c++){ const n = cnt[c]||0, col = CAT_COL[c]||"#8b949e", hot = n>0;
    html += `<span class="tpill${hot?" hot":""}" title="${esc(CAT_NAMES[c])}: ${n}" `+
      `style="border-color:${col};${hot?`background:${col}33;color:${col}`:""}">${hot?n:""}</span>`;
  }
  el.innerHTML = html;
  el.onclick = openThreatsModal;
}
// Threats breakdown modal: every category as a row (color swatch + name + count), active (count>0)
// first then enum order; tap a row -> that category's device list.
export function openThreatsModal(){
  const cnt = countCats(state.lastDetRows);
  const order = [];
  for (let c = 1; c <= CAT_SHOWN; c++) order.push(c);
  order.sort((a,b)=> (cnt[b]||0)-(cnt[a]||0) || a-b);   // active first, then enum order
  $("threatsList").innerHTML = order.map(c=>{ const n = cnt[c]||0, col = CAT_COL[c]||"#8b949e";
    return `<div class="det clk" data-cat="${c}">`+
      `<span class="tiercol"><span class="swatch" style="background:${col}"></span></span>`+
      `<span class="nm"${n?"":' style="opacity:.55"'}>${esc(CAT_NAMES[c])}</span>`+
      `<span class="rssi"${n?` style="color:${col};font-weight:700"`:""}>${n}</span></div>`;
  }).join("");
  $("threatsList").querySelectorAll(".det[data-cat]").forEach(d=>{
    d.onclick = ()=>{ $("threatsDlg").close(); openCatModal(+d.dataset.cat); };
  });
  $("threatsDlg").showModal();
}
