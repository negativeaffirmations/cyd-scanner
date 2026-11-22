// "Alert style" for phone notifications (Settings -> Notifications; used by notify.js).
// One style for all alerts, and per alert type an optional override of each part:
//   details     show the device (category, name, MAC, RSSI) in the notification, or only a plain line such as
//               "Threat alert". Android's notification history keeps what a notification said, so hiding details
//               keeps bystanders' device identifiers out of it. Default: shown.
//   lock        (app only) also show the notification on the lock screen. Default: off.
//   sound       (app only) "default" (the phone's notification sound), "" (silent) or a sound the phone's own sound
//               picker returned (content://...).
//   vib         a vibration preset id, or "custom" with vibCustom = [on, off, on, ...] ms (typed or recorded here).
// Saved inside notify.js's "cflo.notify" record: cfg.style (all alerts) and cfg.kinds[<type>] (only the parts set).
// The app (android-app/native/shim-platform.js) turns a style into an Android notification channel; in a browser
// only details and vibration apply (and a browser may ignore vibration patterns).
import { $, esc, toast } from '../util/dom.js';

export const KINDS = [
  ["threat", "Threats"], ["watch", "Watched devices"], ["follow", "Possible followers"],
  ["rid", "Drones (Remote-ID)"], ["deauth", "Deauth floods"], ["evil", "Evil-twin access points"],
];
const KIND_NAME = Object.fromEntries(KINDS);
const PLAIN_TITLE = { threat:"Threat alert", watch:"Watched device alert", follow:"Possible follower",
  rid:"Drone alert", deauth:"Wi-Fi attack alert", evil:"Wi-Fi attack alert", many:"New scanner alerts",
  general:"Scanner alert" };

// Vibration presets: [on, off, on, ...] ms. null = the phone's default, [] = none.
export const VIB = [
  ["default", "Phone default", null], ["none", "None", []],
  ["short", "Short", [150]], ["long", "Long", [800]],
  ["double", "Double", [200, 120, 200]], ["triple", "Triple", [150, 100, 150, 100, 150]],
  ["heartbeat", "Heartbeat", [100, 80, 250]], ["rapid", "Rapid", [60, 60, 60, 60, 60, 60, 60, 60, 60]],
  ["alarm", "Alarm", [500, 250, 500, 250, 500]],
  ["sos", "SOS", [150, 100, 150, 100, 150, 300, 450, 100, 450, 100, 450, 300, 150, 100, 150, 100, 150]],
  ["custom", "Custom...", null],
];
const VIB_BY_ID = Object.fromEntries(VIB.map(v => [v[0], v]));
const MAX_STEPS = 24, MAX_ON = 5000, MAX_OFF = 5000, MAX_TOTAL = 15000;

export const STYLE_DEFAULTS = { details:true, lock:false, sound:"default", soundTitle:"Phone default", vib:"default", vibCustom:[] };

const app = () => (window.__cfloApp && window.__cfloApp.notifyStyle) || null;

/** "200, 100, 200" -> [200,100,200] (on, off, on...), clamped; null when nothing usable. */
export function parsePattern(text){
  const nums = String(text || "").split(/[\s,;]+/).filter(Boolean).map(Number);
  if(!nums.length || nums.some(n => !isFinite(n) || n < 0)) return null;
  const out = []; let total = 0;
  for(let i = 0; i < nums.length && out.length < MAX_STEPS; i++){
    const on = i % 2 === 0;
    const v = Math.round(Math.min(on ? MAX_ON : MAX_OFF, Math.max(on ? 10 : 0, nums[i])));
    if(total + v > MAX_TOTAL) break;
    out.push(v); total += v;
  }
  return out.length ? out : null;
}
const patternText = p => (p || []).join(", ");

/** Fill missing / bad fields of a saved record (called from notify.js load()). */
export function cleanStyle(cfg){
  const s = cfg.style && typeof cfg.style === "object" ? cfg.style : {};
  if(!cfg.style && cfg.vibrate === false) s.vib = "none";   // the old single "Vibrate" checkbox
  cfg.style = { ...STYLE_DEFAULTS, ...s };
  if(!VIB_BY_ID[cfg.style.vib]) cfg.style.vib = "default";
  const cleanPat = p => Array.isArray(p) ? parsePattern(p.join(",")) || [] : [];
  cfg.style.vibCustom = cleanPat(cfg.style.vibCustom);
  const k = cfg.kinds && typeof cfg.kinds === "object" ? cfg.kinds : {};
  cfg.kinds = {};
  for(const [id] of KINDS){
    const o = k[id] && typeof k[id] === "object" ? { ...k[id] } : {};
    if("vib" in o && !VIB_BY_ID[o.vib]) delete o.vib;
    if("vibCustom" in o) o.vibCustom = cleanPat(o.vibCustom);
    cfg.kinds[id] = o;
  }
  delete cfg.vibrate;
  return cfg;
}

const pick = (cfg, kind, f) => { const o = cfg.kinds[kind]; return o && f in o ? o[f] : cfg.style[f]; };
function vibPattern(vibId, custom){
  if(vibId === "custom") return custom && custom.length ? custom.slice() : null;
  const v = VIB_BY_ID[vibId]; return v ? (v[2] ? v[2].slice() : v[2]) : null;
}

/** The style one notification uses. kind: a KINDS id, "many" (folded: pass kinds) or "general"/"test". */
export function styleFor(cfg, kind, kinds){
  if(kind === "many"){
    const order = ["watch", "follow", "rid", "threat"];
    const ks = order.filter(k => (kinds || []).includes(k));
    const top = ks[0] || "threat";
    const s = styleFor(cfg, top);
    if(ks.length) s.details = ks.every(k => styleFor(cfg, k).details);   // any type that hides details hides the folded one
    return s;
  }
  const own = KIND_NAME[kind] ? kind : null;
  const vibId = own ? pick(cfg, own, "vib") : cfg.style.vib;
  const custom = own && cfg.kinds[own] && "vib" in cfg.kinds[own] ? cfg.kinds[own].vibCustom : cfg.style.vibCustom;
  return {
    kind: own || "general", name: own ? KIND_NAME[own] : "Scanner alerts",
    details: !!(own ? pick(cfg, own, "details") : cfg.style.details),
    lockscreen: !!(own ? pick(cfg, own, "lock") : cfg.style.lock),
    sound: String(own ? pick(cfg, own, "sound") : cfg.style.sound),
    vibration: vibPattern(vibId, custom),
  };
}
export const plainTitle = kind => PLAIN_TITLE[kind] || PLAIN_TITLE.general;
export const PLAIN_BODY = "Open COINTEL-FLO to see it.";

// ---- vibration preview / recorder ----
let recTimer = 0, recStop = null;   // recStop: ends the running recorder without saving
function vibrateNow(p){
  const a = app();
  if(!p || !p.length){ stopVibrate(); return; }
  if(a) a.vibrate(p); else if(navigator.vibrate) navigator.vibrate(p);
}
function stopVibrate(){ const a = app(); if(a) a.stopVibrate(); else if(navigator.vibrate) navigator.vibrate(0); }

// ---- UI ----
// Rendered into #nfStyle (made here when index.html has none, so a cached older page still gets it).
let C = null, SAVE = null, TEST = null;
const isApp = () => !!app();

function selectHtml(f, cur, inherit){
  const opts = [];
  if(inherit) opts.push(["", "Same as all alerts (" + inherit + ")"]);
  return `<select data-f="${f}">` + opts.concat(f === "vib" ? VIB.map(v => [v[0], v[1]]) :
    [["1", f === "details" ? "Show details" : "Show"], ["0", f === "details" ? "Hide details" : "Hide"]])
    .map(([v, t]) => `<option value="${v}"${String(cur) === v ? " selected" : ""}>${esc(t)}</option>`).join("") + "</select>";
}
const onOff = (f, v) => f === "details" ? (v ? "shown" : "hidden") : (v ? "shown" : "hidden");
const vibName = id => (VIB_BY_ID[id] || VIB_BY_ID.default)[1].replace("...", "");

function partHtml(kind){
  const all = kind === "all", o = all ? C.style : C.kinds[kind];
  const has = f => all || f in o;
  const val = f => has(f) ? o[f] : C.style[f];
  let h = "";
  // details
  h += `<label class="k nfl">Details in the notification</label>`;
  h += all ? `<label class="nfchk"><input type="checkbox" data-f="details"${C.style.details ? " checked" : ""}> Show the device (category, name, MAC)</label>`
           : selectHtml("details", has("details") ? (o.details ? "1" : "0") : "", onOff("details", C.style.details));
  if(isApp()){
    h += `<label class="k nfl">Lock screen</label>`;
    h += all ? `<label class="nfchk"><input type="checkbox" data-f="lock"${C.style.lock ? " checked" : ""}> Show alerts on the lock screen</label>`
             : selectHtml("lock", has("lock") ? (o.lock ? "1" : "0") : "", onOff("lock", C.style.lock));
    h += `<label class="k nfl">Sound</label><div class="nfrow">` +
      `<button class="secondary" data-act="sound">${esc(has("sound") ? val("soundTitle") || "Custom sound" : "Same as all alerts (" + (C.style.soundTitle || "Phone default") + ")")}</button>` +
      (!all && has("sound") ? `<button class="secondary" data-act="soundreset">Reset</button>` : "") + `</div>`;
  }
  h += `<label class="k nfl">Vibration</label><div class="nfrow">` +
    selectHtml("vib", has("vib") ? val("vib") : "", all ? null : vibName(C.style.vib)) +
    `<button class="secondary" data-act="try">Try</button></div>`;
  if(has("vib") && val("vib") === "custom"){
    const cust = all ? C.style.vibCustom : (o.vibCustom || []);
    h += `<div class="sub nfl">Milliseconds: vibrate, pause, vibrate, ...</div>` +
      `<div class="nfrow"><input type="text" inputmode="numeric" data-f="vibCustom" placeholder="200, 100, 200" value="${esc(patternText(cust))}">` +
      `<button class="secondary" data-act="rec">Record</button></div>` +
      `<div class="nfrec" hidden><button class="nfpad" data-act="pad">Hold to vibrate</button>` +
      `<div class="nfrow"><span class="sub nfrecinfo">Tap out your pattern: hold = vibrate, let go = pause.</span>` +
      `<button data-act="recdone">Done</button></div></div>`;
  }
  if(!all) h += `<button class="secondary nftest" data-act="test">Send a test</button>`;
  return h;
}
function summaryOf(kind){
  const o = C.kinds[kind], n = Object.keys(o).filter(f => f !== "soundTitle" && f !== "vibCustom").length;
  return n ? "custom" : "same as all alerts";
}
function render(){
  const box = ensureBox(); if(!box) return;
  if(recStop){ recStop(); recStop = null; }   // a repaint replaces the pad: end a recording (and its buzz) first
  const openNow = new Set([...box.querySelectorAll("details[open]")].map(d => d.dataset.k));
  let h = `<strong class="nfhdr">Alert style</strong><div class="nfpart" data-k="all">${partHtml("all")}</div>`;
  h += `<div class="sub" style="margin:.8em 0 .2em">Each alert type (changes only the parts you set):</div>`;
  for(const [k, name] of KINDS){
    h += `<details class="nfkind" data-k="${k}"${openNow.has(k) ? " open" : ""}><summary>${esc(name)} ` +
      `<span class="sub nfsum">${summaryOf(k)}</span></summary><div class="nfpart" data-k="${k}">${partHtml(k)}</div></details>`;
  }
  if(!isApp()) h += `<div class="sub" style="margin-top:.5em">Sounds and the lock screen are set in the COINTEL-FLO Android app. A browser may ignore vibration patterns.</div>`;
  else h += `<div class="sub" style="margin-top:.5em">Android keeps each alert type's sound and vibration in its own notification category; changing them here replaces that category, which also undoes changes made to it in the phone's settings.</div>`;
  box.innerHTML = h;
}
function ensureBox(){
  let box = $("nfStyle");
  if(!box){
    const test = $("nfTestBtn"); if(!test) return null;
    box = document.createElement("div"); box.id = "nfStyle"; box.className = "nfstyle";
    test.parentNode.insertBefore(box, test);
  }
  const old = $("nfVib"); const lab = old && old.closest("label"); if(lab) lab.hidden = true;   // replaced by Vibration
  return box;
}

function target(el){ const p = el.closest(".nfpart"); return p ? p.dataset.k : null; }
function setPart(kind, f, v){
  if(kind === "all") C.style[f] = v;
  else if(v === undefined) delete C.kinds[kind][f];
  else C.kinds[kind][f] = v;
}
function onChange(e){
  const el = e.target, kind = target(el), f = el.dataset.f; if(!kind || !f) return;
  if(f === "details" || f === "lock"){
    setPart(kind, f, kind === "all" ? el.checked : el.value === "" ? undefined : el.value === "1");
  } else if(f === "vib"){
    const v = el.value || undefined;
    setPart(kind, "vib", v);
    if(kind !== "all" && v === undefined) delete C.kinds[kind].vibCustom;
    if(v === "custom"){
      const cur = kind === "all" ? C.style.vibCustom : C.kinds[kind].vibCustom;
      if(!cur || !cur.length) setPart(kind, "vibCustom", [200, 100, 200]);
    }
  } else if(f === "vibCustom"){
    // Saved in place, no repaint: this fires on blur, and a repaint would eat the tap (Try / Record) that caused it.
    const p = parsePattern(el.value);
    const cur = kind === "all" ? C.style.vibCustom : C.kinds[kind].vibCustom;
    if(!p){ toast("Type numbers like 200, 100, 200.", { err:true }); el.value = patternText(cur); return; }
    setPart(kind, "vibCustom", p); el.value = patternText(p);
    SAVE(); return;
  }
  SAVE(); render();
}
async function onClick(e){
  const b = e.target.closest("[data-act]"); if(!b) return;
  const kind = target(b); if(!kind) return;
  const act = b.dataset.act;
  if(act === "try"){ vibrateNow(styleFor(C, kind === "all" ? "general" : kind).vibration || [200, 100, 200]); }
  else if(act === "test"){ TEST(kind); }
  else if(act === "soundreset"){ delete C.kinds[kind].sound; delete C.kinds[kind].soundTitle; SAVE(); render(); }
  else if(act === "sound"){
    const a = app(); if(!a) return;
    const cur = kind === "all" ? C.style.sound : ("sound" in C.kinds[kind] ? C.kinds[kind].sound : C.style.sound);
    try{
      const r = await a.pickSound(cur, (kind === "all" ? "All alerts" : KIND_NAME[kind]) + ": sound");
      if(!r || r.cancelled) return;
      setPart(kind, "sound", String(r.uri)); setPart(kind, "soundTitle", String(r.title || "Custom sound"));
      SAVE(); render();
    }catch(err){ toast("Sound picker failed: " + err.message, { err:true }); }
  }
  else if(act === "rec"){ const part = b.closest(".nfpart"); startRecorder(part, kind); }
}

// Recorder: hold the pad = vibrate (live), release = pause; Done (or 3 s idle) ends it.
function startRecorder(part, kind){
  const rec = part.querySelector(".nfrec"), pad = part.querySelector(".nfpad"), info = part.querySelector(".nfrecinfo");
  if(!rec || !pad || rec.dataset.on) return;   // already recording here: don't add a second set of listeners
  if(recStop){ recStop(); recStop = null; }    // one recorder at a time
  rec.hidden = false; rec.dataset.on = "1";
  const steps = []; let downAt = 0, upAt = 0, total = 0, done = false;
  recStop = () => { done = true; clearTimeout(recTimer); stopVibrate(); };   // abandon without saving
  const finish = () => {
    if(done) return; done = true; recStop = null; clearTimeout(recTimer); stopVibrate();
    if(downAt){ steps.push(Math.max(10, Date.now() - downAt)); downAt = 0; }
    const p = parsePattern(steps.join(","));
    if(p){ setPart(kind, "vibCustom", p); SAVE(); toast("Pattern saved: " + p.length + " steps."); }
    render();
  };
  const idle = () => { clearTimeout(recTimer); recTimer = setTimeout(finish, 3000); };
  pad.addEventListener("pointerdown", ev => {
    ev.preventDefault(); if(done || downAt) return;
    try{ pad.setPointerCapture(ev.pointerId); }catch(_){}
    const now = Date.now();
    if(steps.length && upAt){ steps.push(Math.min(MAX_OFF, now - upAt)); total += steps[steps.length - 1]; }
    downAt = now; clearTimeout(recTimer); pad.classList.add("on");
    vibrateNow([MAX_ON]);
  });
  const up = () => {
    if(!downAt) return;
    const d = Math.min(MAX_ON, Math.max(10, Date.now() - downAt));
    steps.push(d); total += d; downAt = 0; upAt = Date.now(); pad.classList.remove("on"); stopVibrate();
    if(info) info.textContent = Math.ceil(steps.length / 2) + " buzz" + (steps.length > 1 ? "es" : "") + " recorded. Done saves it.";
    if(steps.length >= MAX_STEPS - 1 || total >= MAX_TOTAL) finish(); else idle();
  };
  pad.addEventListener("pointerup", up); pad.addEventListener("pointercancel", up);
  pad.addEventListener("contextmenu", ev => ev.preventDefault());
  part.querySelector('[data-act="recdone"]').onclick = finish;
}

/** Wire the Alert style UI. testKind(kind) sends a test with that type's style. */
export function initStyleUI(cfg, save, testKind){
  C = cfg; SAVE = save; TEST = testKind;
  const box = ensureBox(); if(!box) return;
  box.addEventListener("change", onChange);
  box.addEventListener("click", onClick);
  render();
  // Show the real names of saved sounds (a sound can be renamed or removed on the phone).
  const a = app();
  if(a && a.soundTitle){
    const refresh = async (o) => { if(o && o.sound && o.sound !== "default"){ try{ o.soundTitle = await a.soundTitle(o.sound); }catch(_){} } };
    Promise.all([refresh(cfg.style), ...KINDS.map(([k]) => refresh(cfg.kinds[k]))]).then(render);
  }
}
export function rerenderStyle(){ if(C) render(); }
