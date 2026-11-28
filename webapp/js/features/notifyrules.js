// Custom notifications (owner idea 2026-11-27 "per-threat custom notifications"), on top of the quick on/off tiles of
// Settings -> Notifications (those stay as they are). A rule = a name, TRIGGERS (any one fires it), CONDITIONS (all
// must hold) and its own alert style (sound / vibration / details / lock screen; a part left unset = "all alerts").
// Each list grows with a + button: a row is a dropdown of the kinds plus the input that kind needs.
// A threat tile's three-dot menu (tap it, or long-press the tile) opens "Custom notification" for that category.
//
// Saved inside notify.js's "cflo.notify" record as cfg.rules = [{id, name, on, trig:[{t,v}], cond:[{c,v}], style:{}}].
// Evaluated by notify.js each snapshot (evalRules) BEFORE its quick rules: a device a custom rule covers gets the
// custom alert only, not a second quick one (coveredMacs); the same for the deauth / evil-twin events (coveredEvent).
// Android: each rule's style is its own notification channel kind "r<id>" (the app deletes a kind's older channels when
// a style changes, so rules must not share one). NEW file (stale-module rule); notify.js imports it.
import { state } from '../state.js';
import { $, esc as escStr, toast, log } from '../util/dom.js';
import { askConfirm } from '../ble/auth.js';
const esc = v => escStr(String(v == null ? "" : v));   // dom.js esc() takes strings only; ids and dBm here are numbers
import { CAT_COL, TIER_NAMES, FLAG_BADGES, DEAUTH_ALERT } from '../config.js';
import { VIB, STYLE_DEFAULTS, parsePattern, styleFor } from './notifystyle.js';

const CAT_NAMES = ["","Flock","Axon","ALPR","Cam","Ring","Raven","Glass","Tracker","Drone","Deauth","Flipper","Skim","Other"];
const CAT_SHOWN = 12;
const RULES_MAX = 30, ROWS_MAX = 8;
const normMac = m => String(m || "").trim().toUpperCase();
const tierOf = r => typeof r.tier === "number" ? r.tier : 0;
const app = () => (window.__cfloApp && window.__cfloApp.notifyStyle) || null;

// ---- what a rule can say ----
// field: what input the row needs. event: the trigger is a scanner event (no device), so device-only conditions skip it.
const TRIG = [
  ["cat",    "Threat category",          "cat"],
  ["tier",   "Any threat at tier",       "tier"],
  ["mac",    "A specific device (MAC)",  "mac"],
  ["name",   "Device name contains",     "text"],
  ["sig",    "Signature contains",       "text"],
  ["flag",   "Detector flag",            "flag"],
  ["follow", "Possible follower",        ""],
  ["rid",    "Drone (Remote-ID)",        ""],
  ["deauth", "Deauth flood",             "", true],
  ["evil",   "Evil-twin access point",   "", true],
];
const COND = [
  ["rssi",   "Signal at least",              "num",  -70, "dBm"],
  ["tier",   "Threat tier at least",         "tier", 1],
  ["follow", "Only if it is following me",   ""],
  ["moving", "Only while I'm moving",        "num",  10,  "km/h"],
  ["time",   "Only between",                 "time2", ["22:00", "06:00"]],
  ["once",   "Once per device per drive",    ""],
];
const TRIG_BY = Object.fromEntries(TRIG.map(t => [t[0], t]));
const COND_BY = Object.fromEntries(COND.map(c => [c[0], c]));
const DEVICE_ONLY = new Set(["rssi", "tier", "follow"]);   // conditions about the device: not for deauth / evil events

let C = null, SAVE = null, H = null;   // notify.js cfg, save(), hooks {show, quietMs, ensureOn}
const seen = new Map();                // "ruleId|mac" -> last ms matched (cooldown, like the quick rules)
const once = new Set();                // "ruleId|mac|session" for the once-per-drive condition
const evState = {};                    // "ruleId|deauth" -> {on, at}
let pos = [];                          // [{t, lat, lon}] the scanner's position (STATUS pos), for "while moving"

function newId(){ let s = ""; for(let i = 0; i < 6; i++) s += "abcdefghijklmnopqrstuvwxyz"[Math.floor(Math.random() * 26)]; return s; }
function cleanRules(){
  const out = [];
  for(const r of Array.isArray(C.rules) ? C.rules : []){
    if(!r || typeof r !== "object") continue;
    const trig = (Array.isArray(r.trig) ? r.trig : []).filter(t => t && TRIG_BY[t.t]).slice(0, ROWS_MAX);
    const cond = (Array.isArray(r.cond) ? r.cond : []).filter(c => c && COND_BY[c.c]).slice(0, ROWS_MAX);
    out.push({ id: /^[a-z]{6}$/.test(r.id) ? r.id : newId(), name: String(r.name || "").slice(0, 40), on: r.on !== false,
               trig, cond, style: r.style && typeof r.style === "object" ? r.style : {} });
  }
  C.rules = out.slice(0, RULES_MAX);
}

// ---- matching ----
function speedKmh(){
  if(pos.length < 2) return 0;
  const a = pos[0], b = pos[pos.length - 1], dt = (b.t - a.t) / 1000;
  if(dt < 20) return 0;
  const R = 6371000, toR = Math.PI / 180;
  const dLat = (b.lat - a.lat) * toR, dLon = (b.lon - a.lon) * toR;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * toR) * Math.cos(b.lat * toR) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(h)) / dt * 3.6;
}
function samplePos(){
  const p = String((state.status || {}).pos || "").split(",").map(Number), now = Date.now();
  if(p.length === 2 && p.every(isFinite) && (p[0] || p[1])) pos.push({ t: now, lat: p[0], lon: p[1] });
  pos = pos.filter(x => now - x.t <= 90000);   // speed over the last ~1.5 min
}
function inTime(v){
  const [a, b] = Array.isArray(v) ? v : ["00:00", "23:59"];
  const m = s => { const [h, mi] = String(s).split(":").map(Number); return (h || 0) * 60 + (mi || 0); };
  const d = new Date(), now = d.getHours() * 60 + d.getMinutes(), from = m(a), to = m(b);
  return from <= to ? now >= from && now <= to : now >= from || now <= to;   // wraps past midnight
}
const session = () => String((state.status || {}).sess || "");
function trigHits(t, r){
  switch(t.t){
    case "cat":    return tierOf(r) >= 1 && (r.cat || 0) === +t.v;
    case "tier":   return tierOf(r) >= (+t.v || 1);
    case "mac":    return !!t.v && normMac(r.mac) === normMac(t.v);
    case "name":   return !!t.v && !!r.name && normMac(r.name) !== normMac(r.mac) && r.name.toLowerCase().includes(String(t.v).toLowerCase());
    case "sig":    return !!t.v && !!r.sig && r.sig.toLowerCase().includes(String(t.v).toLowerCase());
    case "flag":   return !!(r.flags & (+t.v || 0));
    case "follow": return (r.ftier || 0) === 2;
    case "rid":    return !!(r.flags & 128);
  }
  return false;
}
function condOk(c, r, ruleId){   // r = a device row, or null for an event
  if(!r && DEVICE_ONLY.has(c.c)) return true;
  switch(c.c){
    case "rssi":   return r.rssi >= (+c.v || -200);
    case "tier":   return tierOf(r) >= (+c.v || 1);
    case "follow": return (r.ftier || 0) === 2;
    case "moving": return speedKmh() >= (+c.v || 0);
    case "time":   return inTime(c.v);
    case "once":   return !once.has(ruleId + "|" + (r ? normMac(r.mac) : "event") + "|" + session());
  }
  return true;
}
function ruleStyle(rule){
  const base = styleFor(C, "general"), s = rule.style || {};
  const vibId = s.vib || C.style.vib;
  let vibration = base.vibration;
  if(s.vib){
    if(s.vib === "custom") vibration = Array.isArray(s.vibCustom) && s.vibCustom.length ? s.vibCustom.slice() : null;
    else { const v = VIB.find(x => x[0] === vibId); vibration = v ? (v[2] ? v[2].slice() : v[2]) : null; }
  }
  return { kind: "r" + rule.id, name: "Custom: " + (rule.name || "rule"),
    details: "details" in s ? !!s.details : base.details, lockscreen: "lock" in s ? !!s.lock : base.lockscreen,
    sound: "sound" in s ? String(s.sound) : base.sound, vibration };
}
function markAlerted(r, rule){
  if(!state.alerted) state.alerted = new Map();
  state.alerted.set(normMac(r.mac), { t: Date.now(), low: false, kinds: ["custom: " + (rule.name || "rule")] });
}
const devLine = r => [CAT_NAMES[r.cat || 0] && r.cat && r.cat <= CAT_SHOWN ? CAT_NAMES[r.cat] : "",
  r.name && normMac(r.name) !== normMac(r.mac) ? r.name : r.mac, r.rssi + " dBm"].filter(Boolean).join(" · ");
const titleOf = rule => rule.name || summary(rule) || "Custom alert";

let covered = new Set(), coveredEv = new Set();
/** Called by notify.js with each new snapshot. Fires the custom rules; returns nothing (see coveredMacs). */
export function evalRules(rows){
  covered = new Set();
  if(!C || !C.rules.length) return;
  const now = Date.now(), q = H.quietMs();
  for(const rule of C.rules){
    if(!rule.on || !rule.trig.some(t => !TRIG_BY[t.t][3])) continue;
    const hits = [];
    for(const r of rows){
      const mt = rule.trig.filter(t => !TRIG_BY[t.t][3] && trigHits(t, r));
      if(!mt.length) continue;
      if(r.muted && !mt.some(t => t.t === "mac")) continue;            // whitelisted: only a MAC trigger still alerts
      if(!rule.cond.every(c => condOk(c, r, rule.id))) continue;
      const mac = normMac(r.mac), key = rule.id + "|" + mac, last = seen.get(key);
      covered.add(mac);
      seen.set(key, now);
      if(last !== undefined && now - last <= q) continue;               // still the same appearance
      hits.push(r);
    }
    if(!hits.length) continue;
    hits.forEach(r => { markAlerted(r, rule); if(rule.cond.some(c => c.c === "once")) once.add(rule.id + "|" + normMac(r.mac) + "|" + session()); });
    hits.sort((a, b) => tierOf(b) - tierOf(a) || b.rssi - a.rssi);
    const r = hits[0];
    log("Alert (custom " + titleOf(rule) + "): " + hits.map(devLine).join("; "));
    if(hits.length === 1) H.show(titleOf(rule), devLine(r) + (devLine(r).indexOf(r.mac) < 0 ? "\n" + r.mac : ""),
      { mac: r.mac, kind: "rule" }, "cflo-r" + rule.id + "-" + normMac(r.mac), ruleStyle(rule));
    else H.show(titleOf(rule) + ": " + hits.length + " devices", hits.slice(0, 5).map(devLine).join("\n") +
      (hits.length > 5 ? "\n+" + (hits.length - 5) + " more" : ""), { kind: "many" }, "cflo-r" + rule.id, ruleStyle(rule));
  }
  for(const [k, t] of seen) if(now - t > q) seen.delete(k);
}
/** MACs a custom rule matched in the last snapshot: notify.js gives them no quick alert. */
export const coveredMacs = () => covered;
/** "deauth" / "evil": a custom rule handles that event right now, so the quick one stays quiet. */
export const coveredEvent = k => coveredEv.has(k);

function evalEvents(){
  coveredEv = new Set();
  if(!C || !C.rules.length) return;
  const s = state.status || {}, now = Date.now(), q = H.quietMs();
  const live = { deauth: (+s.deauth || 0) >= DEAUTH_ALERT, evil: (+s.evil || 0) > 0 };
  for(const rule of C.rules){
    if(!rule.on) continue;
    for(const k of ["deauth", "evil"]){
      if(!rule.trig.some(t => t.t === k)) continue;
      const st = evState[rule.id + "|" + k] || (evState[rule.id + "|" + k] = { on: false, at: 0 });
      const ok = live[k] && rule.cond.every(c => condOk(c, null, rule.id));
      if(ok) coveredEv.add(k);
      if(ok && !st.on && now - st.at > q){
        st.at = now;
        if(rule.cond.some(c => c.c === "once")) once.add(rule.id + "|event|" + session());
        const body = k === "deauth" ? (+s.deauth) + " deauth / disassoc frames per second nearby"
                                    : (+s.evil) + " SSID(s) broadcast by APs with different security";
        log("Alert (custom " + titleOf(rule) + "): " + body);
        H.show(titleOf(rule), body, { kind: k }, "cflo-r" + rule.id + "-" + k, ruleStyle(rule));
      }
      st.on = ok;
    }
  }
}
/** notify.js tick(): position samples + the event triggers. */
export function tickRules(){ samplePos(); evalEvents(); }

// ---- UI: the list in the Notifications card ----
function summary(rule){
  const t = rule.trig.map(x => trigText(x)).join(" or ");
  const c = rule.cond.map(x => condText(x)).join(", ");
  return t + (c ? " · " + c : "");
}
function trigText(x){
  const d = TRIG_BY[x.t]; if(!d) return "";
  if(x.t === "cat") return CAT_NAMES[+x.v] || "category";
  if(x.t === "tier") return "any threat " + (TIER_NAMES[+x.v] || "suspect") + "+";
  if(x.t === "flag"){ const f = FLAG_BADGES.find(b => b[0] === +x.v); return f ? f[2] : "flag"; }
  if(d[2] === "text" || d[2] === "mac") return d[1].toLowerCase() + " " + (x.v || "?");
  return d[1];
}
function condText(x){
  const d = COND_BY[x.c]; if(!d) return "";
  if(x.c === "rssi") return "signal >= " + x.v + " dBm";
  if(x.c === "tier") return TIER_NAMES[+x.v] + "+";
  if(x.c === "moving") return "moving >= " + x.v + " km/h";
  if(x.c === "time") return (x.v || [])[0] + "-" + (x.v || [])[1];
  return d[1].toLowerCase();
}
function ensureBox(){
  let box = $("nrBox");
  if(!box){
    const anchor = $("nfStyle") || $("nfTestBtn"); if(!anchor) return null;
    box = document.createElement("div"); box.id = "nrBox"; box.className = "nrbox";
    anchor.parentNode.insertBefore(box, anchor);
  }
  return box;
}
function renderList(){
  const box = ensureBox(); if(!box) return;
  box.innerHTML = `<strong class="nfhdr">Custom notifications</strong>` +
    `<div class="sub" style="margin:.2em 0 .5em">On top of the switches above: pick what triggers it, add conditions, and give it its own sound and vibration.</div>` +
    (C.rules.length ? C.rules.map(r => `<div class="sessrow keyrow nrrule${r.on ? "" : " off"}" data-id="${r.id}">` +
      `<span class="material-symbols-outlined" style="color:${ruleColor(r)}">${r.on ? "notifications_active" : "notifications_off"}</span>` +
      `<div class="sessmain"><span class="sessdate">${esc(r.name || "Untitled")}</span><span class="sesssub">${esc(summary(r) || "no trigger yet")}</span></div>` +
      `<label class="nrsw" title="On / off"><input type="checkbox" data-act="onoff"${r.on ? " checked" : ""}></label>` +
      `<button class="secondary" data-act="edit">Edit</button></div>`).join("") : "") +
    `<button class="secondary nrnew" data-act="new"><span class="material-symbols-outlined">add</span> Create custom notification</button>`;
}
function ruleColor(r){ const c = r.trig.find(t => t.t === "cat"); return c ? CAT_COL[+c.v] || "" : ""; }

// ---- UI: the editor dialog ----
let dlg = null, draft = null, isNew = false;
function ensureDlg(){
  if(dlg) return dlg;
  dlg = document.createElement("dialog");
  dlg.id = "nrDlg"; dlg.className = "nrdlg";
  document.body.appendChild(dlg);
  dlg.addEventListener("click", onDlgClick);
  dlg.addEventListener("change", onDlgChange);
  dlg.addEventListener("input", e => { if(e.target.id === "nrName") draft.name = e.target.value.slice(0, 40); });
  return dlg;
}
function optionList(list, cur){ return list.map(([v, t]) => `<option value="${esc(v)}"${String(cur) === String(v) ? " selected" : ""}>${esc(t)}</option>`).join(""); }
const CAT_OPTS = () => Array.from({ length: CAT_SHOWN }, (_, i) => [i + 1, CAT_NAMES[i + 1]]);
const TIER_OPTS = () => [[1, "Suspect or worse"], [2, "Likely or worse"], [3, "Confirmed"]];
const FLAG_OPTS = () => FLAG_BADGES.map(b => [b[0], b[2]]);
function fieldHtml(kind, v, unit){
  switch(kind){
    case "cat":   return `<select data-v="1">${optionList(CAT_OPTS(), v)}</select>`;
    case "tier":  return `<select data-v="1">${optionList(TIER_OPTS(), v)}</select>`;
    case "flag":  return `<select data-v="1">${optionList(FLAG_OPTS(), v)}</select>`;
    case "mac":   return `<input type="text" data-v="1" placeholder="AA:BB:CC:DD:EE:FF" value="${esc(v || "")}" autocapitalize="characters" spellcheck="false">`;
    case "text":  return `<input type="text" data-v="1" placeholder="text" value="${esc(v || "")}" spellcheck="false">`;
    case "num":   return `<span class="nrnum"><input type="number" data-v="1" value="${esc(v)}" step="1">${unit ? `<span class="sub">${esc(unit)}</span>` : ""}</span>`;
    case "time2": { const [a, b] = Array.isArray(v) ? v : ["22:00", "06:00"];
      return `<span class="nrnum"><input type="time" data-v="0" value="${esc(a)}"><span class="sub">and</span><input type="time" data-v="1" value="${esc(b)}"></span>`; }
  }
  return "";
}
const defaultOf = (kind, def) => kind === "cat" || kind === "tier" ? 1 : kind === "flag" ? FLAG_BADGES[0][0] : def !== undefined ? def : "";
function rowsHtml(list, defs, key){
  return list.map((x, i) => {
    const d = defs[x[key]];
    return `<div class="nrrow" data-list="${key}" data-i="${i}"><select data-k="1">${optionList(Object.values(defs).map(z => [z[0], z[1]]), x[key])}</select>` +
      fieldHtml(d[2], x.v, d[4]) + `<button class="xbtn" data-act="del" aria-label="Remove">&times;</button></div>`;
  }).join("") || `<div class="sub nrempty">${key === "t" ? "Add at least one trigger." : "None: it alerts whenever a trigger matches."}</div>`;
}
function styleHtml(){
  const s = draft.style, base = C.style, a = app();
  const vibName = id => (VIB.find(v => v[0] === id) || VIB[0])[1].replace("...", "");
  let h = `<div class="nrsec"><strong>Alert</strong></div>`;
  if(a) h += `<label class="k nfl">Sound</label><div class="nfrow"><button class="secondary" data-act="sound">` +
    esc("sound" in s ? s.soundTitle || "Custom sound" : "Same as all alerts (" + (base.soundTitle || "Phone default") + ")") + `</button>` +
    ("sound" in s ? `<button class="secondary" data-act="soundreset">Reset</button>` : "") + `</div>`;
  h += `<label class="k nfl">Vibration</label><div class="nfrow"><select data-s="vib">` +
    optionList([["", "Same as all alerts (" + vibName(base.vib) + ")"], ...VIB.map(v => [v[0], v[1]])], s.vib || "") +
    `</select><button class="secondary" data-act="try">Try</button></div>`;
  if(s.vib === "custom") h += `<div class="sub nfl">Milliseconds: vibrate, pause, vibrate, ...</div>` +
    `<input type="text" inputmode="numeric" data-s="vibCustom" placeholder="200, 100, 200" value="${esc((s.vibCustom || []).join(", "))}">`;
  h += `<label class="k nfl">Details in the notification</label><select data-s="details">` +
    optionList([["", "Same as all alerts (" + (base.details ? "shown" : "hidden") + ")"], ["1", "Show details"], ["0", "Hide details"]],
               "details" in s ? (s.details ? "1" : "0") : "") + `</select>`;
  if(a) h += `<label class="k nfl">Lock screen</label><select data-s="lock">` +
    optionList([["", "Same as all alerts (" + (base.lock ? "shown" : "hidden") + ")"], ["1", "Show"], ["0", "Hide"]],
               "lock" in s ? (s.lock ? "1" : "0") : "") + `</select>`;
  return h;
}
function renderDlg(){
  const d = ensureDlg();
  const hasEvent = draft.trig.some(t => TRIG_BY[t.t] && TRIG_BY[t.t][3]);
  d.innerHTML = `<div class="modalhdr"><strong>${isNew ? "New custom notification" : "Custom notification"}</strong>` +
    `<button class="xbtn" data-act="close" aria-label="Close">&times;</button></div>` +
    `<label class="k nfl">Name</label><input type="text" id="nrName" maxlength="40" placeholder="e.g. Flock close by" value="${esc(draft.name)}">` +
    `<div class="nrsec"><strong>Triggers</strong><span class="sub">any one of these</span>` +
    `<button class="nradd" data-act="add" data-list="t" aria-label="Add a trigger"${draft.trig.length >= ROWS_MAX ? " disabled" : ""}><span class="material-symbols-outlined">add</span></button></div>` +
    rowsHtml(draft.trig, TRIG_BY, "t") +
    `<div class="nrsec"><strong>Conditions</strong><span class="sub">all of these</span>` +
    `<button class="nradd" data-act="add" data-list="c" aria-label="Add a condition"${draft.cond.length >= ROWS_MAX ? " disabled" : ""}><span class="material-symbols-outlined">add</span></button></div>` +
    rowsHtml(draft.cond, COND_BY, "c") +
    (hasEvent && draft.cond.some(c => DEVICE_ONLY.has(c.c)) ? `<div class="sub nfl">Signal, tier and following apply to devices only, not to deauth / evil-twin events.</div>` : "") +
    styleHtml() +
    `<div class="nrbtns"><button data-act="save">Save</button><button class="secondary" data-act="test">Send a test</button>` +
    (isNew ? "" : `<button class="danger" data-act="delete">Delete</button>`) + `</div>`;
  if(!d.open) d.showModal();
}
function openEditor(rule, preset){
  if(!C) return;
  isNew = !rule;
  draft = rule ? JSON.parse(JSON.stringify(rule)) : { id: newId(), name: "", on: true, trig: [], cond: [], style: {} };
  if(preset){ draft.trig.push(preset); if(preset.t === "cat") draft.name = CAT_NAMES[+preset.v] || ""; }
  renderDlg();
}
function rowsOf(list){ return list === "t" ? draft.trig : draft.cond; }
async function onDlgClick(e){
  if(e.target === dlg){ dlg.close(); return; }   // backdrop
  const b = e.target.closest("[data-act]"); if(!b) return;
  const act = b.dataset.act;
  if(act === "close") dlg.close();
  else if(act === "add"){
    const list = b.dataset.list, rows = rowsOf(list);
    if(rows.length >= ROWS_MAX) return;
    const def = list === "t" ? TRIG[0] : (COND.find(c => !draft.cond.some(x => x.c === c[0])) || COND[0]);
    rows.push(list === "t" ? { t: def[0], v: defaultOf(def[2], def[3] === true ? undefined : def[3]) } : { c: def[0], v: defaultOf(def[2], def[3]) });
    renderDlg();
  }
  else if(act === "del"){ const row = b.closest(".nrrow"); rowsOf(row.dataset.list).splice(+row.dataset.i, 1); renderDlg(); }
  else if(act === "try"){
    const p = ruleStyle(draft).vibration || [200, 100, 200], a = app();
    if(a) a.vibrate(p); else if(navigator.vibrate) navigator.vibrate(p);
  }
  else if(act === "sound"){
    const a = app(); if(!a) return;
    try{
      const r = await a.pickSound("sound" in draft.style ? draft.style.sound : C.style.sound, (draft.name || "Custom notification") + ": sound");
      if(!r || r.cancelled) return;
      draft.style.sound = String(r.uri); draft.style.soundTitle = String(r.title || "Custom sound"); renderDlg();
    }catch(err){ toast("Sound picker failed: " + err.message, { err: true }); }
  }
  else if(act === "soundreset"){ delete draft.style.sound; delete draft.style.soundTitle; renderDlg(); }
  else if(act === "test"){
    if(!(await H.ensureOn())) return;
    if(await H.show("Test: " + titleOf(draft), (summary(draft) || "Custom notification") + "\n(This is only a test.)",
                    { kind: "test" }, "cflo-test-r" + draft.id, ruleStyle(draft))) toast("Test sent.");
  }
  else if(act === "save"){
    if(!draft.trig.length){ toast("Add a trigger first.", { err: true }); return; }
    const bad = draft.trig.find(t => (t.t === "mac" && !/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(normMac(t.v))) ||
                                     ((t.t === "name" || t.t === "sig") && !String(t.v || "").trim()));
    if(bad){ toast(bad.t === "mac" ? "Type the MAC as AA:BB:CC:DD:EE:FF." : "Type the text to look for.", { err: true }); return; }
    draft.trig.forEach(t => { if(t.t === "mac") t.v = normMac(t.v); });
    draft.name = (draft.name || "").trim();
    const i = C.rules.findIndex(r => r.id === draft.id);
    if(i >= 0) C.rules[i] = draft;
    else { if(C.rules.length >= RULES_MAX){ toast("That's " + RULES_MAX + " custom notifications already.", { err: true }); return; } C.rules.push(draft); }
    SAVE(); renderList(); dlg.close();
    await H.ensureOn();
    toast("Saved: " + titleOf(draft) + ".");
  }
  else if(act === "delete"){
    dlg.close();   // the confirm is a modal of its own: one at a time
    if(!(await askConfirm({ title: "Delete " + (draft.name || "this custom notification") + "?",
                            msg: "Its triggers, conditions and alert style are removed. The switches above are not changed.",
                            ok: "Delete", cancel: "Keep", danger: true }))){ renderDlg(); return; }
    C.rules = C.rules.filter(r => r.id !== draft.id); SAVE(); renderList(); toast("Deleted.");
  }
}
function onDlgChange(e){
  const el = e.target;
  const row = el.closest(".nrrow");
  if(row){
    const list = row.dataset.list, x = rowsOf(list)[+row.dataset.i], defs = list === "t" ? TRIG_BY : COND_BY, key = list;
    if(el.dataset.k){
      const d = defs[el.value];
      x[key] = el.value; x.v = defaultOf(d[2], list === "t" ? undefined : d[3]);
      renderDlg(); return;
    }
    if(el.dataset.v !== undefined){
      const d = defs[x[key]];
      if(d[2] === "time2"){ const v = Array.isArray(x.v) ? x.v.slice() : ["22:00", "06:00"]; v[+el.dataset.v] = el.value; x.v = v; }
      else if(d[2] === "num") x.v = +el.value;
      else if(d[2] === "cat" || d[2] === "tier" || d[2] === "flag") x.v = +el.value;
      else x.v = el.value;
    }
    return;
  }
  const f = el.dataset.s; if(!f) return;
  if(f === "vib"){
    if(el.value) draft.style.vib = el.value; else { delete draft.style.vib; delete draft.style.vibCustom; }
    if(el.value === "custom" && !(draft.style.vibCustom || []).length) draft.style.vibCustom = [200, 100, 200];
    renderDlg();
  } else if(f === "vibCustom"){
    const p = parsePattern(el.value);
    if(!p){ toast("Type numbers like 200, 100, 200.", { err: true }); el.value = (draft.style.vibCustom || []).join(", "); return; }
    draft.style.vibCustom = p; el.value = p.join(", ");
  } else if(f === "details" || f === "lock"){
    if(el.value === "") delete draft.style[f]; else draft.style[f] = el.value === "1";
  }
}

// ---- the threat tiles' three-dot menu (tap the dots, or long-press the tile) ----
let menu = null;
function openTileMenu(cat){
  if(!menu){
    menu = document.createElement("dialog"); menu.id = "nrMenu"; document.body.appendChild(menu);
    menu.addEventListener("click", e => {
      if(e.target === menu){ menu.close(); return; }
      const b = e.target.closest("[data-act]"); if(!b) return;
      menu.close();
      if(b.dataset.act === "close") return;
      if(b.dataset.act === "newcat") openEditor(null, { t: "cat", v: +b.dataset.cat });
      else if(b.dataset.act === "edit"){ const r = C.rules.find(x => x.id === b.dataset.id); if(r) openEditor(r); }
    });
  }
  const mine = C.rules.filter(r => r.trig.some(t => t.t === "cat" && +t.v === cat));
  menu.innerHTML = `<div class="modalhdr"><strong style="color:${CAT_COL[cat] || ""}">${esc(CAT_NAMES[cat])}</strong>` +
    `<button class="xbtn" data-act="close" aria-label="Close">&times;</button></div>` +
    `<div class="sub" style="margin-bottom:.6em">The switch turns ${esc(CAT_NAMES[cat])} alerts on or off. A custom notification adds its own sound, vibration and conditions.</div>` +
    mine.map(r => `<button class="secondary" data-act="edit" data-id="${r.id}" style="margin-bottom:.4em">Edit: ${esc(r.name || "Untitled")}</button>`).join("") +
    `<button data-act="newcat" data-cat="${cat}"><span class="material-symbols-outlined" style="vertical-align:-.2em">add</span> Custom notification</button>`;
  menu.showModal();
}
function wireTiles(){
  const grid = $("nfCats"); if(!grid) return;
  let timer = 0, longFired = false, startXY = null;
  grid.addEventListener("pointerdown", e => {
    const t = e.target.closest(".tcard"); if(!t) return;
    longFired = false; startXY = [e.clientX, e.clientY];
    clearTimeout(timer);
    timer = setTimeout(() => { longFired = true; if(navigator.vibrate) navigator.vibrate(30); openTileMenu(+t.dataset.cat); }, 550);
  });
  const cancel = () => clearTimeout(timer);
  grid.addEventListener("pointerup", cancel); grid.addEventListener("pointercancel", cancel); grid.addEventListener("pointerleave", cancel);
  grid.addEventListener("pointermove", e => { if(startXY && Math.hypot(e.clientX - startXY[0], e.clientY - startXY[1]) > 12) cancel(); });
  grid.addEventListener("contextmenu", e => { if(e.target.closest(".tcard")) e.preventDefault(); });
  // Capture: runs before notify.js's own click handler (which toggles the tile) and can swallow the click.
  grid.addEventListener("click", e => {
    const dots = e.target.closest("[data-nrmenu]");
    if(dots || longFired){
      e.stopImmediatePropagation(); e.preventDefault();
      if(dots) openTileMenu(+dots.dataset.nrmenu);
      longFired = false;
    }
  }, true);
}

/** Called once by notify.js initNotify(). hooks: show(title, body, data, tag, style), quietMs(), ensureOn() -> bool. */
export function initRules(cfg, save, hooks){
  C = cfg; SAVE = save; H = hooks;
  cleanRules();
  renderList();
  const box = ensureBox();
  if(box) box.addEventListener("click", e => {
    const b = e.target.closest("[data-act]"); if(!b) return;
    const row = b.closest(".nrrule"), rule = row && C.rules.find(r => r.id === row.dataset.id);
    if(b.dataset.act === "new") openEditor(null);
    else if(b.dataset.act === "edit" && rule) openEditor(rule);
  });
  if(box) box.addEventListener("change", e => {
    if(e.target.dataset.act !== "onoff") return;
    const row = e.target.closest(".nrrule"), rule = row && C.rules.find(r => r.id === row.dataset.id);
    if(rule){ rule.on = e.target.checked; SAVE(); renderList(); }
  });
  wireTiles();
}
