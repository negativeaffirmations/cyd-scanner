// Server history in the device popup (#detDlg): what the paired scan-analyzer server knows about this MAC.
//
// One GET per popup open: <server>/api/v1/devices/{mac} (scan-analyzer docs/api-contract.md "Read API - v1"),
// Bearer = the paired key, which needs the owner-granted "read" permission. Shows the device's totals, the
// enrichment (vendor / company / likely models), the location estimate, the cross-day verdict, and the per-day
// timeline ("when and where"), plus an optional map of the days' positions and the estimate.
// Settings -> Remote Server also gets a "Reading server data" line from GET /api/v1/whoami.
//
// Like notify.js this hooks nothing in the older modules: it watches #detInfo (rewritten on every popup open,
// live or Explore) and reads the MAC from its rows. New file on purpose (stale-module rule, CLAUDE.md).
//
// Server epochs are the logs' LOCAL seconds (UTC + the phone's offset at the time), so they are formatted with
// UTC getters: the clock time the owner saw on the device, whatever zone the phone is in now.
import { $, esc } from '../util/dom.js';
import { TIER_COL, CAT_COL } from '../config.js';
import { isPaired, serverUrl } from './remote.js';
import { newMap } from './map.js';

const TIMEOUT_MS = 10000;
const DAYS_SHOWN = 7;   // timeline rows before "Show all"
// threat_cat keys (server) -> CAT_COL index / label (the CYD Category enum order, as config.js / categories.js).
const CAT_KEYS = ["", "flock", "axon", "alpr", "cam", "ring", "raven", "glass", "tracker", "drone", "deauth", "flipper", "skim"];
const CAT_LABEL = ["Other", "Flock", "Axon", "ALPR", "Cam", "Ring", "Raven", "Glass", "Tracker", "Drone", "Deauth", "Flipper", "Skim"];
const TIER_IDX = { none:0, suspect:1, likely:2, confirmed:3 };
const LEVEL_TXT = { confirmed:"Fixed in place (confirmed)", likely:"Probably fixed in place", possible:"Possibly fixed in place",
                    "single-day":"Seen on one day only", mobile:"Moves around", "no-fix":"No GPS fixes" };
const CLASS_TXT = { stationary:"stationary", mobile:"moving", indeterminate:"unclear" };

let openSeq = 0;          // bumps on every popup (re)render; a reply for an older one is dropped
let curMac = "";
let curData = null;
let srvMap = null, srvLayer = null;

const normMac = m => String(m || "").trim().toUpperCase();
function api(path){ return serverUrl().replace(/\/+$/, "") + path; }
function key(){ try{ return localStorage.getItem("rs.key") || ""; }catch(e){ return ""; } }

async function getJson(path){
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), TIMEOUT_MS);
  try{
    const r = await fetch(api(path), { headers:{ "Authorization":"Bearer " + key() }, signal:ctl.signal, cache:"no-store" });
    let body = null; try{ body = await r.json(); }catch(e){}
    return { status:r.status, body };
  }catch(e){
    return { status:0, body:null, err: e.name === "AbortError" ? "no reply in " + (TIMEOUT_MS/1000) + " s" : e.message };
  }finally{ clearTimeout(t); }
}

// ---- formatting (local epochs -> UTC getters) ----
const MON = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
const WD = ["Sun","Mon","Tue","Wed","Thu","Fri","Sat"];
function hm(e){
  const d = new Date(e * 1000); let h = d.getUTCHours(); const ap = h < 12 ? "AM" : "PM"; h = h % 12 || 12;
  return h + ":" + String(d.getUTCMinutes()).padStart(2, "0") + " " + ap;
}
function dateTxt(e, withYear){
  const d = new Date(e * 1000);
  return MON[d.getUTCMonth()] + " " + d.getUTCDate() + (withYear ? ", " + d.getUTCFullYear() : "");
}
function whenTxt(e){ return e > 0 ? dateTxt(e, true) + " " + hm(e) : "unknown"; }
function dayTxt(day){   // "YYYY-MM-DD" -> "Thu Nov 20"
  const d = new Date(day + "T00:00:00Z");
  return isNaN(d) ? day : WD[d.getUTCDay()] + " " + MON[d.getUTCMonth()] + " " + d.getUTCDate();
}
const okLL = (a, b) => a != null && b != null && isFinite(a) && isFinite(b) && !(a === 0 && b === 0);
const plural = (n, w) => n + " " + w + (n === 1 ? "" : "s");

function kv(k, v, cls){ return `<div class="kv"><span class="k">${esc(k)}</span><span class="vv${cls ? " " + cls : ""}">${v}</span></div>`; }

// ---- popup section ----
function popupMac(){
  for(const row of document.querySelectorAll("#detInfo .kv")){
    const k = row.querySelector(".k"), v = row.querySelector(".vv");
    if(k && v && k.textContent.trim() === "MAC") return normMac(v.textContent);
  }
  return "";
}

function render(html){ const el = $("detServer"); if(el) el.innerHTML = html; }
function head(extra){ return `<div class="srvhead"><span class="material-symbols-outlined">cloud</span><strong>Server history</strong>${extra || ""}</div>`; }

function onPopup(){
  const el = $("detServer"); if(!el) return;
  const mac = popupMac();
  const seq = ++openSeq;
  curMac = mac; curData = null; sight = null;
  if(!mac || !/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/.test(mac) || !isPaired()){ render(""); return; }
  render(head() + `<div class="sub">Asking the server…</div>`);
  load(mac, seq);
}

async function load(mac, seq){
  const r = await getJson("/api/v1/devices/" + encodeURIComponent(mac));
  if(seq !== openSeq) return;   // the popup moved on to another device (or closed and reopened)
  const retry = `<button class="secondary srvretry" id="srvRetry">Try again</button>`;
  if(r.status === 200 && r.body && r.body.device){ curData = r.body; renderData(r.body); return; }
  if(r.status === 404){ render(head() + `<div class="sub">The server has no sightings of this device yet.</div>`); return; }
  if(r.status === 403) render(head() + `<div class="sub">This phone's key may not read server data. Turn on <strong>read</strong> for it on the server's admin page (Keys tab), then try again.</div>` + retry);
  else if(r.status === 401) render(head() + `<div class="sub">The server didn't accept this phone's key. Pair again in Settings → Remote Server.</div>`);
  else render(head() + `<div class="sub">Couldn't reach the server (${esc(r.err || "HTTP " + r.status)}).</div>` + retry);
  const b = $("srvRetry"); if(b) b.onclick = () => { const s = ++openSeq; render(head() + `<div class="sub">Asking the server…</div>`); load(mac, s); };
}

function renderData(d){
  const dev = d.device, e = d.enrichment, est = d.estimate, h = d.history, id = d.identity, tl = d.timeline || [];
  const tn = TIER_IDX[dev.tier] || 0;
  const ci = Math.max(0, CAT_KEYS.indexOf(dev.threat_cat || ""));
  const rows = [];
  rows.push(kv("Seen", esc(plural(dev.sightings || 0, "sighting") + " in " + plural(dev.sessions || 0, "scan") +
                       (tl.length ? ", on " + plural(tl.length, "day") : ""))));
  rows.push(kv("First seen", esc(whenTxt(dev.first_seen))));
  rows.push(kv("Last seen", esc(whenTxt(dev.last_seen))));
  if(dev.strongest_rssi != null) rows.push(kv("Strongest", esc(dev.strongest_rssi + " dBm")));
  rows.push(kv("Threat tier", esc(dev.tier || "none"), "t" + tn));
  if(ci) rows.push(kv("Category", `<span class="srvcat" style="border-color:${CAT_COL[ci]};color:${CAT_COL[ci]}">${CAT_LABEL[ci]}</span>`));
  if(dev.sig) rows.push(kv("Signature", esc(dev.sig)));
  if(e){
    if(e.vendor) rows.push(kv("Vendor", esc(e.vendor)));
    if(e.company_name) rows.push(kv("BLE company", esc(e.company_name)));
    if(e.brand) rows.push(kv("Brand", esc(e.brand)));
    if(e.likely_models) rows.push(kv("Likely models", esc(String(e.likely_models).split("+").join(", ")) + (e.confidence ? ` <span class="sub">(${esc(e.confidence)} confidence)</span>` : "")));
    if(e.category) rows.push(kv("Device kind", esc(e.category)));
    if(e.notes) rows.push(kv("Notes", esc(e.notes)));
    if(e.is_random_mac) rows.push(kv("Address", "random (changes over time)"));
  }
  if(est && okLL(est.lat, est.lon))
    rows.push(kv("Estimated spot", esc(est.lat.toFixed(5) + ", " + est.lon.toFixed(5) + (est.radius_m != null ? " ± " + Math.round(est.radius_m) + " m" : "")) +
                 (est.classification ? ` <span class="sub">(${esc(CLASS_TXT[est.classification] || est.classification)})</span>` : "")));
  if(h) rows.push(kv("Over time", esc((LEVEL_TXT[h.level] || h.level) + (h.n_days ? ", " + plural(h.n_days, "day") : "")) +
                     (h.stationary_score != null ? ` <span class="sub">(stationary score ${Math.round(h.stationary_score * 100)}%)</span>` : "")));
  if(id) rows.push(kv("Identity", esc("linked to " + plural(id.addresses || 1, "address") + " (one device rotating its address)")));

  const hasPos = tl.some(t => okLL(t.lat, t.lon)) || (est && okLL(est.lat, est.lon));
  render(head() + rows.join("") +
    (tl.length ? `<div class="srvsub">Days seen</div><div id="srvDays" class="srvdays"></div>` : "") +
    ((dev.sightings || 0) > 0 ? `<div id="srvSight" class="srvdays"><button class="secondary" id="srvSightBtn">Show individual sightings</button></div>` : "") +
    (hasPos ? `<button class="secondary" id="srvMapBtn">Show server positions on a map</button>` : "") +
    `<div id="srvMap" hidden></div>`);
  if(tl.length) renderDays(tl, false);
  const mb = $("srvMapBtn"); if(mb) mb.onclick = () => showMap(null);
  const sb = $("srvSightBtn"); if(sb) sb.onclick = () => { sight = { rows:[], ids:new Set(), done:false, busy:false }; loadSightings(); };
}

// ---- raw sightings, newest first, paged (GET /devices/{mac}/sightings?order=desc&limit=&before=) ----
// `before` is exclusive and many rows can share one epoch second, so the next page asks for before = oldest + 1 and
// drops rows already shown (by id); a page that adds nothing new falls back to before = oldest.
const SIGHT_PAGE = 50;
const SRC_TXT = { "2.4":"Wi-Fi 2.4", "5G":"Wi-Fi 5", "BLE":"BLE", "PRB":"Wi-Fi client", "154":"802.15.4" };
let sight = null;   // { rows, ids, done, busy, edge } for the current popup

async function loadSightings(){
  const s = sight; if(!s || s.busy || s.done) return;
  s.busy = true;
  const seq = openSeq, mac = curMac;
  renderSightings("Loading…");
  const oldest = s.rows.length ? s.rows[s.rows.length - 1].epoch : 0;
  const pageRows = async before => {
    const q = "?order=desc&limit=" + SIGHT_PAGE + (before ? "&before=" + before : "");
    return getJson("/api/v1/devices/" + encodeURIComponent(mac) + "/sightings" + q);
  };
  let r = await pageRows(oldest ? oldest + 1 : 0);
  let fresh = r.status === 200 && Array.isArray(r.body) ? r.body.filter(x => !s.ids.has(x.id)) : null;
  if(fresh && !fresh.length && oldest && r.body.length >= SIGHT_PAGE){   // a whole page inside one second: step past it
    r = await pageRows(oldest);
    fresh = r.status === 200 && Array.isArray(r.body) ? r.body.filter(x => !s.ids.has(x.id)) : null;
    if(fresh && fresh.length) s.edge = true;   // rows of that second beyond one page were skipped
  }
  if(seq !== openSeq || s !== sight) return;   // another popup now
  s.busy = false;
  if(!fresh){
    renderSightings(r.status === 403 ? "This phone's key may not read server data." :
                    "Couldn't load sightings (" + (r.err || "HTTP " + r.status) + ").", true);
    return;
  }
  fresh.forEach(x => { s.ids.add(x.id); s.rows.push(x); });
  if(!r.body.length || r.body.length < SIGHT_PAGE || !fresh.length) s.done = true;
  renderSightings("");
}

function renderSightings(note, retry){
  const el = $("srvSight"); const s = sight; if(!el || !s) return;
  const rows = s.rows.map((x, i) => {
    const pos = okLL(x.lat, x.lon);
    const bits = [SRC_TXT[x.source] || x.source || ""];
    if(x.rssi != null) bits.push(x.rssi + " dBm");
    if(x.channel != null) bits.push("ch " + x.channel);
    if(x.is_live) bits.push("live");
    if(pos && x.fix_age != null) bits.push("fix " + x.fix_age + " s old");
    return `<div class="srvday${pos ? " clk" : ""}" data-i="${i}">` +
      `<span class="d">${esc(x.epoch > 0 ? dateTxt(x.epoch) + " " + hm(x.epoch) : "no time")}</span>` +
      `<span class="n">${esc(bits.filter(Boolean).join(" · "))}</span>` +
      (pos ? `<span class="material-symbols-outlined pin" title="Show on map">location_on</span>` : "") + `</div>`;
  }).join("");
  const more = !s.done && !s.busy ? `<button class="secondary" id="srvSightMore">${s.rows.length ? "Load older sightings" : "Try again"}</button>` : "";
  el.innerHTML = `<div class="srvsub">Sightings${s.rows.length ? " (newest " + s.rows.length + ")" : ""}</div>` + rows +
    (note ? `<div class="sub">${esc(note)}</div>` : "") +
    (s.edge ? `<div class="sub">Some sightings that share one second were skipped.</div>` : "") +
    (retry ? `<button class="secondary" id="srvSightMore">Try again</button>` : more) +
    (s.done && s.rows.length ? `<div class="sub">That's all of them.</div>` : "");
  el.querySelectorAll(".srvday.clk").forEach(r => { r.onclick = () => showMap(s.rows[+r.dataset.i]); });
  const b = $("srvSightMore"); if(b) b.onclick = () => { if(retry) s.busy = false; loadSightings(); };
}

function renderDays(tl, all){
  const el = $("srvDays"); if(!el) return;
  const list = all ? tl : tl.slice(0, DAYS_SHOWN);
  el.innerHTML = list.map((t, i) => {
    const pos = okLL(t.lat, t.lon);
    const span = t.first_epoch && t.last_epoch ? hm(t.first_epoch) + (t.last_epoch - t.first_epoch >= 60 ? " – " + hm(t.last_epoch) : "") : "";
    return `<div class="srvday${pos ? " clk" : ""}" data-i="${i}">` +
      `<span class="d">${esc(dayTxt(t.day))}</span>` +
      `<span class="n">${esc(plural(t.n || 0, "sighting"))}${span ? " · " + esc(span) : ""}${t.rssi_max != null ? " · " + esc(t.rssi_max + " dBm") : ""}</span>` +
      (pos ? `<span class="material-symbols-outlined pin" title="Show on map">location_on</span>` : "") + `</div>`;
  }).join("") +
  (!all && tl.length > DAYS_SHOWN ? `<button class="secondary" id="srvAllDays">Show all ${tl.length} days</button>` : "");
  el.querySelectorAll(".srvday.clk").forEach(r => { r.onclick = () => showMap(list[+r.dataset.i]); });
  const b = $("srvAllDays"); if(b) b.onclick = () => renderDays(tl, true);
}

// Map of the server's view: one marker per day with a position (newer = larger), the estimate as a circle of its
// radius. `focus` = a timeline row to centre on (a tapped day), else fit everything.
function showMap(focus){
  const d = curData; if(!d) return;
  const box = $("srvMap"); if(!box) return;
  box.hidden = false;
  const btn = $("srvMapBtn"); if(btn) btn.hidden = true;
  if(srvMap && srvMap.getContainer() !== box){ srvMap.remove(); srvMap = null; }   // the section was re-rendered
  if(!srvMap){ srvMap = newMap("srvMap"); srvLayer = L.layerGroup().addTo(srvMap); }
  const col = TIER_COL[TIER_IDX[d.device.tier] || 0];
  setTimeout(() => {
    srvMap.invalidateSize(); srvLayer.clearLayers();
    const all = [];
    const days = (d.timeline || []).filter(t => okLL(t.lat, t.lon));
    days.slice().reverse().forEach((t, i, arr) => {   // oldest first, so the newest draws on top
      const newest = i === arr.length - 1;
      const p = [t.lat, t.lon]; all.push(p);
      L.circleMarker(p, { radius: newest ? 8 : 5, color: col, weight: 2, fillColor: col, fillOpacity: newest ? .8 : .45 })
        .addTo(srvLayer).bindPopup(esc(dayTxt(t.day)) + "<br>" + esc(plural(t.n || 0, "sighting")) + (t.rssi_max != null ? " · " + esc(t.rssi_max + " dBm") : ""));
    });
    const est = d.estimate;
    if(est && okLL(est.lat, est.lon)){
      const p = [est.lat, est.lon]; all.push(p);
      if(est.radius_m > 0) L.circle(p, { radius: est.radius_m, color: "#ffffff", weight: 1, dashArray: "4 4", fillOpacity: .08 }).addTo(srvLayer);
      L.circleMarker(p, { radius: 6, color: "#ffffff", weight: 3, fillColor: col, fillOpacity: .9 })
        .addTo(srvLayer).bindPopup("Estimated spot" + (est.radius_m != null ? " ± " + Math.round(est.radius_m) + " m" : "") +
                                   (est.classification ? "<br>" + esc(CLASS_TXT[est.classification] || est.classification) : ""));
    }
    if(focus && focus.id !== undefined && okLL(focus.lat, focus.lon))   // a tapped raw sighting: its own marker
      L.circleMarker([focus.lat, focus.lon], { radius: 7, color: "#ffffff", weight: 2, fillColor: col, fillOpacity: .9 })
        .addTo(srvLayer).bindPopup(esc(whenTxt(focus.epoch)) + (focus.rssi != null ? "<br>" + esc(focus.rssi + " dBm") : "")).openPopup();
    if(focus && okLL(focus.lat, focus.lon)) srvMap.setView([focus.lat, focus.lon], 17);
    else if(all.length > 1) srvMap.fitBounds(L.latLngBounds(all).pad(0.3));
    else if(all.length) srvMap.setView(all[0], 17);
    box.scrollIntoView({ block: "nearest", behavior: "smooth" });
  }, 80);
}

// ---- Settings -> Remote Server: "Reading server data" ----
let whoSeq = 0;
async function renderReadAccess(){
  const el = $("rsRead"); if(!el) return;
  if(!isPaired()){ el.textContent = ""; return; }
  const seq = ++whoSeq;
  el.textContent = "Reading server data: checking…";
  const r = await getJson("/api/v1/whoami");
  if(seq !== whoSeq) return;
  if(r.status === 200 && r.body){
    const can = Array.isArray(r.body.scopes) && r.body.scopes.includes("read");
    el.innerHTML = can ? "Reading server data: <strong>allowed</strong> (device popups show the server's history)."
      : "Reading server data: <strong>off</strong>. Turn on <strong>read</strong> for this key on the server's admin page (Keys tab).";
  } else if(r.status === 401) el.textContent = "Reading server data: the server didn't accept this key. Pair again.";
  else if(r.status === 404) el.textContent = "Reading server data: this server doesn't offer it yet.";
  else el.textContent = "Reading server data: couldn't reach the server (" + (r.err || "HTTP " + r.status) + ").";
}

export function initServerHistory(){
  const info = $("detInfo");
  if(info) new MutationObserver(onPopup).observe(info, { childList:true });
  const dlg = $("detDlg");
  if(dlg) dlg.addEventListener("close", () => { openSeq++; curMac = ""; curData = null; sight = null; });   // drop a late reply
  const sub = $("set-remote");
  if(sub) new MutationObserver(() => { if(!sub.hidden) renderReadAccess(); }).observe(sub, { attributes:true, attributeFilter:["hidden"] });
  const st = $("rsStatus");   // pair / unpair rewrite it (connect / upload redraw it unchanged: ignored)
  let lastSt = st ? st.textContent : "";
  if(st) new MutationObserver(() => {
    if(st.textContent === lastSt) return;
    lastSt = st.textContent;
    if(!sub || !sub.hidden) renderReadAccess();
  }).observe(st, { childList:true, subtree:true, characterData:true });
}
