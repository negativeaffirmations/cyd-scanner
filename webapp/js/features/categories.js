// Threat-category breakdown (mirrors the CYD HOME readout): one chip per category with a live
// threat device, coloured by its worst tier, severity-sorted. Tap a chip -> device list modal ->
// device detail modal (reuses the detDlg modal; no per-detection GPS in the live stream).
import { state } from '../state.js';
import { $, esc } from '../util/dom.js';
import { tierNum, flagBadges } from '../util/format.js';
import { TIER_COL, TIER_NAMES, FLAG_BADGES } from '../config.js';

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

export function renderCatChips(rows){
  const el = $("catChips"); if (!el) return;
  const cnt = {}, worst = {};
  rows.forEach(r=>{
    const tn = rowTier(r), c = r.cat||0;
    if (r.muted || tn < 1 || !c) return;          // threats only, categorized, not whitelisted
    cnt[c] = (cnt[c]||0) + 1; worst[c] = Math.max(worst[c]||0, tn);
  });
  const cats = Object.keys(cnt).map(Number).sort((a,b)=> (worst[b]-worst[a]) || (cnt[b]-cnt[a]));
  if (!cats.length){ el.innerHTML = ""; el.style.display = "none"; return; }
  el.style.display = "";
  el.innerHTML = '<span class="catlbl">Threats</span>' + cats.map(c=>
    `<button class="catchip" data-cat="${c}" style="border-color:${TIER_COL[worst[c]]}">`+
    `<span class="cn">${esc(CAT_NAMES[c]||"?")}</span>`+
    `<span class="cc" style="color:${TIER_COL[worst[c]]}">${cnt[c]}</span></button>`).join("");
  el.querySelectorAll(".catchip").forEach(b=>{ b.onclick = ()=> openCatModal(+b.dataset.cat); });
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
    el.querySelectorAll(".det[data-ri]").forEach(d=>{ d.onclick = ()=> openCatDevice(rows[+d.dataset.ri]); });
  }
  $("catDlg").showModal();
}
// Live-detection detail (reuses the detDlg modal; no per-detection GPS in the live stream).
function openCatDevice(r){
  const tn = rowTier(r);
  const rowsKv = [["Name", r.name||r.mac||"<hidden>"], ["MAC", r.mac],
    ["Category", CAT_NAMES[r.cat||0] || "Other"],
    ["Signals", (r.sources||[]).map(s=>s.tag).join(", ") || "?"],
    ["RSSI", r.rssi + " dBm"], ["Threat tier", TIER_NAMES[tn] || "none"]];
  if (r.muted) rowsKv.push(["Whitelisted","yes (muted)"]);
  if (r.flags) rowsKv.push(["Flags", FLAG_BADGES.filter(f=>r.flags&f[0]).map(f=>f[2]).join(", ")]);
  if (r.ie && !/^0+$/.test(r.ie)) rowsKv.push(["IE fingerprint", r.ie]);
  $("detInfo").innerHTML = rowsKv.map(([k,v])=>
    `<div class="kv"><span class="k">${k}</span><span class="vv t${k==="Threat tier"?tn:0}">${esc(String(v))}</span></div>`).join("");
  $("detMap").style.display = "none";
  $("detMapMsg").style.display = ""; $("detMapMsg").textContent = "Live detection — open the Map or a session for GPS.";
  $("detDlg").showModal();
}
