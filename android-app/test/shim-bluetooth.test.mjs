// Mock-plugin test of native/shim-bluetooth.js: node test/shim-bluetooth.test.mjs (no phone needed).
import { readFileSync } from "node:fs";
const src = readFileSync(new URL("../native/shim-bluetooth.js", import.meta.url), "utf8");
const listeners = new Map(); const calls = [];
let inFlight = 0, maxInFlight = 0;
const SVC="9a1e0000-2b7e-4c1a-9b00-1a2b3c4d5e6f", CH="9a1e0003-2b7e-4c1a-9b00-1a2b3c4d5e6f", ST="9a1e0004-2b7e-4c1a-9b00-1a2b3c4d5e6f";
const emit = (ev, d) => (listeners.get(ev)||[]).forEach(f=>f(d));
const slow = async (name, ret) => { calls.push(name); inFlight++; maxInFlight=Math.max(maxInFlight,inFlight); await new Promise(r=>setTimeout(r,20)); inFlight--; return ret; };
const BLE = {
  initialize: async()=>{ calls.push("init"); }, isEnabled: async()=>({value:true}),
  connect: async(o)=>{ calls.push("connect "+o.deviceId); },
  disconnect: async(o)=>{ calls.push("disconnect"); emit("disconnected|"+o.deviceId); },
  getServices: async()=>({services:[{uuid:SVC,characteristics:[{uuid:CH,properties:{write:true}},{uuid:ST,properties:{read:true,notify:true}}]}]}),
  read: (o)=>slow("read",{value:"6869"}), write:(o)=>slow("write "+o.value), writeWithoutResponse:(o)=>slow("wnr "+o.value),
  startNotifications:(o)=>slow("startN"), stopNotifications:(o)=>slow("stopN"),
  requestLEScan: async()=>{ calls.push("scan"); }, stopLEScan: async()=>{ calls.push("stopscan"); },
};
globalThis.window = globalThis;
globalThis.localStorage = { m:{}, getItem(k){return this.m[k]??null}, setItem(k,v){this.m[k]=String(v)} };
globalThis.Capacitor = { Plugins:{BluetoothLe:BLE}, addListener:(p,ev,cb)=>{ if(!listeners.has(ev)) listeners.set(ev,[]); listeners.get(ev).push(cb); return {remove(){ const a=listeners.get(ev); a.splice(a.indexOf(cb),1);} }; } };
Object.defineProperty(globalThis,"navigator",{value:{},configurable:true,writable:true});
eval(src);
const ok = (c,m)=>{ console.log((c?"PASS ":"FAIL ")+m); if(!c) process.exitCode=1; };
const [d] = [await (async()=>{ const B=navigator.bluetooth; ok(!!B,"navigator.bluetooth defined"); ok(typeof BluetoothDevice.prototype.watchAdvertisements==="function","presence supported"); return [null]; })()];
// device via getDevices after a remembered entry
localStorage.setItem("cflo.app.devices", JSON.stringify([{id:"AA:BB:CC:DD:EE:FF",name:"CFLO"}]));
const devs = await navigator.bluetooth.getDevices(); const dev = devs[0];
ok(dev.id==="AA:BB:CC:DD:EE:FF" && (await navigator.bluetooth.getDevices())[0]===dev, "same device object per id");
let discEvents = 0; dev.addEventListener("gattserverdisconnected", ()=>discEvents++);
const server = await dev.gatt.connect(); ok(dev.gatt.connected, "connected");
const svc = await server.getPrimaryService(SVC.toUpperCase());
let nf; try{ await svc.getCharacteristic("9a1e0099-2b7e-4c1a-9b00-1a2b3c4d5e6f"); }catch(e){ nf=e.name; } ok(nf==="NotFoundError","missing char -> NotFoundError");
const cmd = await svc.getCharacteristic(CH), st = await svc.getCharacteristic(ST);
// concurrent ops are serialized
await Promise.all([cmd.writeValue(new TextEncoder().encode("G")), st.readValue(), cmd.writeValue(new Uint8Array([1,2])), st.startNotifications()]);
ok(maxInFlight===1, "GATT ops serialized (max in flight "+maxInFlight+")");
ok(calls.includes("write 47") && calls.includes("write 0102"), "hex encoding of writes");
const v = await st.readValue(); ok(new TextDecoder().decode(v)==="hi" && st.value===v, "readValue -> DataView");
let got=""; st.addEventListener("characteristicvaluechanged", ev=>{ got = new TextDecoder().decode(ev.target.value); });
emit(`notification|${dev.id}|${SVC}|${ST}`, {value:"4f4b"}); ok(got==="OK", "notification -> characteristicvaluechanged with target.value");
// local disconnect: connected false at once, one event, then a quick reconnect survives
dev.gatt.disconnect(); ok(!dev.gatt.connected, "disconnect: connected=false immediately");
await new Promise(r=>setTimeout(r,30)); ok(discEvents===1, "one gattserverdisconnected after local disconnect ("+discEvents+")");
await dev.gatt.connect(); await new Promise(r=>setTimeout(r,3200)); ok(dev.gatt.connected && discEvents===1, "reconnect not torn down by the old disconnect / fallback timer");
// remote drop
emit("disconnected|"+dev.id); await new Promise(r=>setTimeout(r,10)); ok(!dev.gatt.connected && discEvents===2, "remote drop -> event");
let ne; try{ await cmd.writeValue(new Uint8Array([1])); }catch(e){ ne=e.name; } ok(ne==="NetworkError","write after drop -> NetworkError");
// presence
let adv=0; dev.addEventListener("advertisementreceived", e=>{ if(e.rssi===-50) adv++; });
const ctl = new AbortController(); await dev.watchAdvertisements({signal:ctl.signal});
emit("onScanResult",{device:{deviceId:dev.id},rssi:-50}); emit("onScanResult",{device:{deviceId:"other"},rssi:-50});
ok(adv===1, "advertisementreceived only for its own id"); ctl.abort(); ok(calls.at(-1)==="stopscan","scan stops when last watcher leaves");
