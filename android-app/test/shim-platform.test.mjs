// Mock test of native/shim-platform.js downloads: node test/shim-platform.test.mjs (no phone needed).
import { readFileSync } from "node:fs";
const src = readFileSync(new URL("../native/shim-platform.js", import.meta.url), "utf8");
const saved = new Map(); let open = 0, maxOpen = 0, n = 0;
const CfloFiles = {
  begin: async o => { const id = String(++n); saved.set(id, { name:o.name, parts:[] }); open++; maxOpen = Math.max(maxOpen, open); return { id }; },
  append: async o => { await new Promise(r => setTimeout(r, 2)); saved.get(o.id).parts.push(Buffer.from(o.data, "base64")); },
  finish: async o => { open--; const s = saved.get(o.id); s.done = true; return { bytes: Buffer.concat(s.parts).length }; },
  abort: async () => {},
};
class FakeAnchor { constructor(){ this.attrs = {}; } setAttribute(k, v){ this.attrs[k] = v; } getAttribute(k){ return this.attrs[k]; } hasAttribute(k){ return k in this.attrs; } set download(v){ this.attrs.download = v; } }
FakeAnchor.prototype.click = function(){ throw new Error("native click reached"); };
globalThis.window = globalThis;
globalThis.HTMLAnchorElement = FakeAnchor;
globalThis.FileReader = class { readAsDataURL(b){ b.arrayBuffer().then(ab => { this.result = "data:x;base64," + Buffer.from(ab).toString("base64"); this.onload(); }); } };
const toast = { textContent:"", className:"", hidden:true, classList:{ remove(){} } };
globalThis.document = { readyState:"complete", getElementById: id => id === "toast" ? toast : null, addEventListener(){} };
globalThis.location = { href:"https://localhost/", host:"localhost" };
globalThis.localStorage = { m:{}, getItem(k){ return this.m[k] ?? null; }, setItem(k, v){ this.m[k] = String(v); } };
globalThis.open = () => null;
const lnListeners = {}; const scheduled = []; let lnPerm = "prompt";
const LocalNotifications = {
  checkPermissions: async () => ({ display: lnPerm }),
  requestPermissions: async () => { lnPerm = "granted"; return { display: lnPerm }; },
  schedule: async o => { scheduled.push(...o.notifications); },
};
globalThis.Capacitor = { isNativePlatform: () => true, Plugins: { CfloFiles, LocalNotifications },
  addListener: (p, ev, cb) => { lnListeners[ev] = cb; return { remove(){} }; } };
eval(src);
const ok = (c, m) => { console.log((c ? "PASS " : "FAIL ") + m); if(!c) process.exitCode = 1; };
const big = new Uint8Array(2 * 1024 * 1024 + 12345); for(let i = 0; i < big.length; i++) big[i] = (i * 31 + 7) & 255;
function save(bytes, name){ const a = new FakeAnchor(); a.href = URL.createObjectURL(new Blob([bytes])); a.download = name; a.click(); URL.revokeObjectURL(a.href); }
save(big, "big.jsonl"); save(new TextEncoder().encode("hello\nworld"), "small.jsonl");
await new Promise(r => setTimeout(r, 1500));
const files = [...saved.values()];
ok(files.length === 2 && files.every(f => f.done), "both files finished");
ok(Buffer.concat(files[0].parts).equals(Buffer.from(big)), "2 MB file reassembled byte-exact (" + files[0].parts.length + " chunks)");
ok(Buffer.concat(files[1].parts).toString() === "hello\nworld", "small file exact");
ok(files[0].name === "big.jsonl" && files[1].name === "small.jsonl", "names kept");
ok(maxOpen === 1, "saves run one at a time");
ok(/Saved to Downloads\/COINTEL-FLO: small\.jsonl/.test(toast.textContent), "toast says where it went");

// notifications
ok(Notification.permission === "default", "permission starts as default");
ok(await Notification.requestPermission() === "granted" && Notification.permission === "granted", "requestPermission -> granted");
const reg = await navigator.serviceWorker.register("notify-sw.js");
await reg.showNotification("Flock nearby", { body:"AA:BB", tag:"mac-AA", data:{ mac:"AA:BB:CC:DD:EE:FF" } });
await reg.showNotification("Flock nearby", { body:"again", tag:"mac-AA", data:{ mac:"AA:BB:CC:DD:EE:FF" } });
ok(scheduled.length === 2 && scheduled[0].id === scheduled[1].id && scheduled[0].smallIcon === "ic_stat_cflo", "same tag -> same id (replaces), small icon set");
let msg = null; navigator.serviceWorker.addEventListener("message", e => { msg = e.data; });
lnListeners.localNotificationActionPerformed({ notification: scheduled[1] });
ok(msg && msg.type === "cflo-notify-click" && msg.mac === "AA:BB:CC:DD:EE:FF", "tap -> cflo-notify-click message with the mac");
