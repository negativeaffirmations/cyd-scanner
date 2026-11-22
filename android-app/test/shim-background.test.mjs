// Mock test of native/shim-background.js: node test/shim-background.test.mjs (no phone needed).
import { readFileSync } from "node:fs";
const src = readFileSync(new URL("../native/shim-background.js", import.meta.url), "utf8");
const sleep = ms => new Promise(r => setTimeout(r, ms));
const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if(!c) process.exitCode = 1; };

const calls = [];
let exempt = false, locFail = null;
const CfloBg = {
  start: async o => { calls.push(["start", o]); },
  stop: async o => { calls.push(["stop", o]); },
  status: async () => ({ running: false, batteryExempt: exempt, location: true }),
  requestBatteryExemption: async () => { exempt = true; return { batteryExempt: true }; },
  startLocation: async () => { calls.push(["startLocation"]); if(locFail) throw locFail; },
  stopLocation: async () => { calls.push(["stopLocation"]); },
};
const listeners = {};
const linkEvents = new EventTarget();
let disconnectAllCalls = 0;
globalThis.window = globalThis;
{ const wt = new EventTarget(); globalThis.addEventListener = wt.addEventListener.bind(wt); globalThis.dispatchEvent = wt.dispatchEvent.bind(wt); }
window.__cfloApp = { bleLink: { events: linkEvents, disconnectAll(){ disconnectAllCalls++; } } };
globalThis.Capacitor = { isNativePlatform: () => true, Plugins: { CfloBg },
  addListener: (p, ev, cb) => { listeners[p + ":" + ev] = cb; return { remove(){} }; } };
const els = {};
function el(id){ return els[id] || (els[id] = { id, hidden:false, textContent:"", className:"", style:{}, classList:{ remove(){} },
  _click:null, addEventListener(t, f){ if(t === "click") this._click = f; } }); }
const nfCard = { insertAdjacentElement(pos, card){ els.bgCard = card; el("bgState"); el("bgAllowBtn"); el("bgLogBtn"); } };
globalThis.document = { readyState:"complete", visibilityState:"visible", addEventListener(){},
  getElementById: id => id === "nfCard" ? nfCard : (id === "toast" || els[id]) ? el(id) : null,
  createElement: () => ({ style:{}, set innerHTML(v){} }) };
const store = { "cflo.unitNames": JSON.stringify({ dev1: "TOLUBA" }) };
globalThis.localStorage = { getItem: k => store[k] ?? null, setItem: (k, v) => { store[k] = String(v); } };
// Node 22 has a global navigator (getter only); the shim defines geolocation on it.
window.__cfloTestStopDelay = 300;
eval(src);
await new Promise(r => setTimeout(r, 10));
const initialStop = calls.length === 1 && calls[0][0] === "stop" && !calls[0][1].lost;
calls.length = 0;

const link = (id, connected, lost) => { const e = new Event("link"); e.id = id; e.connected = connected; e.lost = !!lost; linkEvents.dispatchEvent(e); };

// service follows the link
ok(initialStop, "page start stops a service left by an earlier page (REV-002)");
link("dev1", true);
await sleep(10);
ok(calls.length === 1 && calls[0][0] === "start" && /TOLUBA/.test(calls[0][1].title), "connect -> service started, notification names the unit");
ok(window.__cfloApp.background === true, "__cfloApp.background set (notify.js status line)");
link("dev1", false, false); await sleep(100); link("dev1", true); await sleep(400);
ok(!calls.some(c => c[0] === "stop"), "switch (disconnect + connect within the stop delay) doesn't stop the service");
document.visibilityState = "hidden";
link("dev1", false, true); await sleep(400);
const st = calls.filter(c => c[0] === "stop");
ok(st.length === 1 && /Lost the link to TOLUBA/.test(st[0][1].lost || ""), "lost in the background -> stop with a 'link lost' note");
document.visibilityState = "visible";
link("dev1", true); await sleep(10); link("dev1", false, false); await sleep(400);
const st2 = calls.filter(c => c[0] === "stop");
ok(st2.length === 2 && !st2[1][1].lost, "a user disconnect -> stop without a note");

// heartbeat + disconnect button
let ticks = 0; window.addEventListener("cflo-bg-tick", () => ticks++);
listeners["CfloBg:tick"](); document.visibilityState = "hidden"; listeners["CfloBg:tick"](); listeners["CfloBg:tick"]();
ok(ticks === 2, "native tick -> cflo-bg-tick only while hidden");
document.visibilityState = "visible";
// The page stays "visible" in the background now (CfloWebView): the app state comes from appState.
listeners["CfloBg:appState"]({ fg:false }); listeners["CfloBg:tick"]();
ok(ticks === 3, "app in background but page still 'visible' -> ticks forwarded (appState)");
listeners["CfloBg:appState"]({ fg:true }); listeners["CfloBg:tick"]();
ok(ticks === 3, "app back on screen -> ticks not forwarded");
listeners["CfloBg:disconnectRequested"]();
ok(disconnectAllCalls === 1, "notification Disconnect -> every scanner disconnected");

// geolocation
const got = [];
const wid = navigator.geolocation.watchPosition(p => got.push(p), () => {});
await sleep(10);
ok(calls.filter(c => c[0] === "startLocation").length === 1, "watchPosition starts native GPS once");
const now = Date.now();
listeners["CfloBg:location"]({ lat: 40.1, lon: -75.2, acc: 5, time: now, spd: 1.5 });
ok(got.length === 1 && got[0].coords.latitude === 40.1 && got[0].coords.longitude === -75.2 && got[0].timestamp === now &&
   got[0].coords.speed === 1.5 && got[0].coords.heading === null, "a native fix reaches the watch as a Position");
let one = null;
navigator.geolocation.getCurrentPosition(p => { one = p; }, null, { maximumAge: 5000 });
await sleep(5);
ok(one && one.coords.latitude === 40.1, "getCurrentPosition answers from a fix within maximumAge");
navigator.geolocation.clearWatch(wid);
await sleep(2200);
ok(calls.filter(c => c[0] === "stopLocation").length === 1, "last watch cleared -> native GPS stopped");
let tErr = null;
navigator.geolocation.getCurrentPosition(() => {}, e => { tErr = e; }, { timeout: 50 });
await sleep(100);
ok(tErr && tErr.code === 3, "getCurrentPosition times out with code 3");
await sleep(2200);
locFail = Object.assign(new Error("Location permission denied"), { code: "denied" });
let dErr = null;
navigator.geolocation.watchPosition(() => {}, e => { dErr = e; });
await sleep(20);
ok(dErr && dErr.code === 1, "permission refused -> error code 1 (gps.js falls back / logs)");
// gps.js's error path: clearWatch, then a one-shot from inside the error callback -> it must get the real error too.
let oneErr = null;
const w2 = navigator.geolocation.watchPosition(() => {}, () => {
  navigator.geolocation.clearWatch(w2);
  navigator.geolocation.getCurrentPosition(() => {}, e => { oneErr = e; });
});
await sleep(30);
ok(oneErr && oneErr.code === 1, "a one-shot started from the error callback reports the real error (REV-005)");

// battery card
await sleep(10);
ok(els.bgAllowBtn && els.bgAllowBtn.hidden === false && /battery saver/.test(els.bgState.textContent), "not exempt -> card shows the Allow button");
await els.bgAllowBtn._click(); await sleep(5);
ok(els.bgAllowBtn.hidden === true && /Allowed/.test(els.bgState.textContent), "after allowing -> card says allowed, button hidden");
process.exit();
