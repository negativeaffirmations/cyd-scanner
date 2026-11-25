// Phone analysis, increment 3c: alerts from the phone's own verdicts (docs/phone-analysis.md phase 3).
//
//  - Whole-drive follower: a device the engine (analysis/engine.js) calls following over the whole scan. Skipped when the
//    scanner whitelisted it, when it is a rotating BLE address with no signature or detector flag (phones / watches,
//    the same guard the scanners use), or when the saved scans show it on >= OWN_DAYS earlier days with no signature
//    (it travels with you every day: your own car / phone); the popup still shows those.
//  - An alert that fired here before: a new evil-twin / deauth line in this scan naming devices that the saved scans
//    (analysis/history.js) tie to the same kind of alert on an earlier day. The scanner has already alerted; this adds
//    "seen here before, so probably a fixed setup" to it.
// Shown through features/notify.js's state.notifyShow, so the Notifications switches (Possible follower, Evil twin,
// Deauth flood), the permission, the alert style and the quiet time all apply. Reaches the engine / history through
// state (one module instance each). NEW file (stale-module rule).
import { state } from '../state.js';
import { connGen } from '../ble/xfer.js';

const EVERY_MS = 15000, OWN_DAYS = 3, DAY_S = 86400;
const EVT_RULE = { eviltwin:"evil", deauth:"deauth" };
const EVT_NAME = { eviltwin:"Evil-twin alert", deauth:"Deauth burst" };

let gen = -1, evIdx = 0, lastRun = 0;
const told = new Map();   // "kind|mac" -> ms

const ymd = t=>new Date(t * 1000).toISOString().slice(0, 10);
const recent = key=>{ const t = told.get(key), q = state.notifyQuietMs ? state.notifyQuietMs() : 15 * 60000; return t && Date.now() - t < q; };

function followerAlerts(){
  for(const a of state.engine.followers()){
    if(a.wl) continue;
    const plain = !a.tier && !a.flags.length;
    if(a.rotating && plain) continue;
    const h = state.history && state.history.historyFor(a.mac);
    if(plain && h && h.days.filter(d=>d !== ymd(Date.now() / 1000)).length >= OWN_DAYS) continue;   // travels with you daily
    const key = "fol|" + a.mac;
    if(recent(key)) continue;
    told.set(key, Date.now());
    const name = a.names[0] || a.mac;
    // The server may know it as fixed in place (features/screen.js): then it can't be following.
    const g = gen, v = state.screen ? state.screen.follower(a.mac) : Promise.resolve(null);
    v.then(r=>{
    if(g !== connGen()) return;
    const low = !!(r && r.low);
    state.notifyShow("follow", (low ? "Probably a false alarm: " : "") + "Following you (phone analysis)",
      name + "\n" + a.verdict.replace(/^Following: s/, "S").replace(/\.$/, "") + (a.sig ? "\n" + a.sig : "") + (low ? "\n" + r.why : ""),
      { mac:a.mac, kind:"follow" }, "cflo-pfol-" + a.mac, "follow", low);
    });
  }
}

function eventAlerts(){
  const { next, list } = state.engine.eventsFrom(evIdx);
  evIdx = next;
  if(!state.history) return;
  for(const ev of list){
    const rule = EVT_RULE[ev.evt];
    if(!rule || !ev.t) continue;
    const macs = ev.evt === "eviltwin" ? String(ev.macs || "").split(",") : [ev.from, ev.to, ev.bssid];
    const before = new Set();
    for(const m of macs){
      const h = m && state.history.historyFor(String(m).trim());
      if(h) h.events.forEach(e=>{ if(e.evt === ev.evt && e.t && ev.t - e.t > DAY_S / 2) before.add(ymd(e.t)); });
    }
    if(!before.size) continue;
    const key = ev.evt + "|" + (ev.ssid || macs.join(","));
    if(recent(key)) continue;
    told.set(key, Date.now());
    const dates = [...before].sort();
    state.notifyShow(rule, EVT_NAME[ev.evt] + " seen here before",
      (ev.ssid ? ev.ssid + ": " : "") + "the same devices set it off on " + dates.slice(-3).join(", ") +
      (dates.length > 3 ? " and " + (dates.length - 3) + " more day(s)" : "") + ". Probably a fixed setup, not an attack.",
      { kind:rule }, "cflo-prev-" + ev.evt, rule);
  }
}

function tick(){
  if(!state.engine || !state.notifyShow || !state.connected) return;
  if(Date.now() - lastRun < EVERY_MS) return;
  lastRun = Date.now();
  if(gen !== connGen()){ gen = connGen(); evIdx = 0; told.clear(); }
  if(state.engine.engineGen && state.engine.engineGen() !== gen) return;   // the engine hasn't seen this connection's data yet
  try{ followerAlerts(); eventAlerts(); }catch(e){}
}

export function initAlerts(){
  setInterval(tick, 2000);
  window.addEventListener("cflo-bg-tick", tick);   // Android app: hidden-page timers are throttled
}
