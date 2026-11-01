// Fleet enrollment crypto (docs/fleet-mode.md §5.1-§5.2, §17). The phone holds each scanner's 128-bit key as a
// non-extractable HMAC CryptoKey (auth.js, IndexedDB "cflo-keys"/"keys"). A member's FLEET key is
//   FK = HMAC(K_member, "CFLO-FLT-KEY-1|" + sid_member)[0..16)
// and reaches the leader masked under the LEADER's key, bound to both sids, the leader's enrollment counter (each
// command is single-use, even across a leader reboot), the key generation and the name:
//   pad     = HMAC(K_leader, "CFLO-FLT-WRAP-1|" + sid_L + "|" + hex(w) + "|" + sid_c)[0..16)
//   wrapped = FK xor pad
//   tag     = HMAC(K_leader, "CFLO-FLT-ADD-2|" + sid_L + "|" + ctr + "|" + hex(w) + "|" + sid_c + "|" + kg + "|" +
//                  hex(wrapped) + "|" + name)[0..8)
// The member's phone key never leaves this phone; the leader only learns FK.
// Reads the key store directly (read-only) instead of importing a new auth.js export, so a phone holding a
// cached auth.js for the 10-minute Pages window still loads this module.
const te = new TextEncoder();
const DB_NAME = "cflo-keys", STORE = "keys";

// Open WITHOUT a version (never trips a VersionError if auth.js raises the schema); never creates the store.
function getKey(sid){
  return new Promise((res, rej) => {
    if(!window.indexedDB){ rej(new Error("IndexedDB unavailable")); return; }
    const r = indexedDB.open(DB_NAME);
    r.onerror = () => rej(r.error);
    r.onsuccess = () => {
      const d = r.result;
      if(!d.objectStoreNames.contains(STORE)){ d.close(); res(null); return; }
      const t = d.transaction(STORE, "readonly").objectStore(STORE).get(sid);
      t.onsuccess = () => { d.close(); res(t.result && t.result.key ? t.result.key : null); };
      t.onerror = () => { d.close(); rej(t.error); };
    };
  });
}

const hex = u8 => Array.from(u8, b => b.toString(16).padStart(2, "0")).join("");
async function hmac(key, str){ return new Uint8Array(await crypto.subtle.sign("HMAC", key, te.encode(str))); }

// The firmware accepts 1-20 of [A-Za-z0-9 _.-]; anything else becomes '-'.
export function memberName(name){
  const s = String(name || "member").replace(/[^A-Za-z0-9 _.\-]/g, "-").slice(0, 20).trim();
  return s || "member";
}

// "a:<sid_c>:<kg>:<ctr>:<w>:<wrapped>:<tag>:<name>" for CMD "M<seq>:" + this. `ctr` = the leader's current enrollment
// counter (last field of the "M:l" header). Throws when a key isn't stored.
export async function buildAddCmd(leaderSid, member, ctr){
  const sidL = String(leaderSid || "").toLowerCase(), sidC = String(member.sid || "").toLowerCase();
  if(!/^[0-9a-f]{12}$/.test(sidL) || !/^[0-9a-f]{12}$/.test(sidC)) throw new Error("bad scanner id");
  if(sidL === sidC) throw new Error("a scanner can't join its own fleet");
  const [kL, kC] = await Promise.all([getKey(sidL), getKey(sidC)]);
  if(!kL) throw new Error("this phone has no key for the leader");
  if(!kC) throw new Error("this phone has no key for " + sidC);
  return addCmdWithKeys(kL, kC, sidL, sidC, member, ctr);
}

// The pure part (exported for the bench cross-check against the firmware): kL / kC are HMAC CryptoKeys.
export async function addCmdWithKeys(kL, kC, sidL, sidC, member, ctr){
  if(!Number.isInteger(ctr) || ctr < 1) throw new Error("no enrollment counter from the leader");
  const kg = String(Number.isInteger(member.kg) && member.kg >= 0 ? member.kg : 0);
  const name = memberName(member.name);
  const c = String(ctr);
  const fk = (await hmac(kC, "CFLO-FLT-KEY-1|" + sidC)).slice(0, 16);
  const w = hex(crypto.getRandomValues(new Uint8Array(16)));
  const pad = (await hmac(kL, "CFLO-FLT-WRAP-1|" + sidL + "|" + w + "|" + sidC)).slice(0, 16);
  const wrapped = new Uint8Array(16);
  for(let i = 0; i < 16; i++) wrapped[i] = fk[i] ^ pad[i];
  fk.fill(0); pad.fill(0);
  const wr = hex(wrapped);
  const tag = hex((await hmac(kL, ["CFLO-FLT-ADD-2", sidL, c, w, sidC, kg, wr, name].join("|"))).slice(0, 8));
  return ["a", sidC, kg, c, w, wr, tag, name].join(":");
}
