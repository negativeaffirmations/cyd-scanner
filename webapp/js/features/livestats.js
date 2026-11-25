// Live-upload timing, to see where a delay is: one line every 30 s while pushed log data arrives or is posted. In the
// Android app it goes into the background log, in a browser to the console. Counts and times only: no MACs, names
// or positions.
//   push  the scanner's own statistics (fw >= 0.45.2, "LL=~" frames): bytes / s sent, refused sends, longest gap between
//         push calls (ms), bytes still waiting per file
//   rx    bytes / s that reached the phone, and per unit the age (s) of its newest row on arrival
//   post  loglines posts, bytes, the slowest reply (ms), failures
// NEW file, reached through state hooks (stale-module rule).
import { state } from '../state.js';

const EVERY_MS = 30000, TAIL_MAX = 4096;
let t0 = Date.now(), push = null, rx = 0, posts = null, carry = new Map(), newest = new Map();
const dec = new TextDecoder();
const freshPost = ()=>({ n:0, bytes:0, max:0, fail:0 });
posts = freshPost();

// "LL=~ms,bytes,frames,refused,calls,maxgap;<fid>,<sent to>,<size>[;...]"
function onStat(text){
  const parts = text.slice(4).split(";"), h = parts[0].split(",").map(Number);
  if(h.length !== 6 || h.some(x=>!Number.isFinite(x))) return;
  if(!push) push = { ms:0, bytes:0, refused:0, gap:0, wait:[] };
  push.ms += h[0]; push.bytes += h[1]; push.refused += h[3]; push.gap = Math.max(push.gap, h[5]);
  push.wait = parts.slice(1).map(p=>{ const f = p.split(","); return Math.max(0, (+f[2] || 0) - (+f[1] || 0)); });
}

// Pushed pieces split lines anywhere: keep the unfinished end per file.
function onPiece(ev){
  const d = ev.detail;
  rx += d.bytes.length;
  const nowLocal = Math.floor(Date.now() / 1000) - new Date().getTimezoneOffset() * 60;   // log epochs are LOCAL seconds
  const lines = ((carry.get(d.fid) || "") + dec.decode(d.bytes)).split("\n");
  const rest = lines.pop();
  carry.set(d.fid, rest.length > TAIL_MAX ? "" : rest);
  if(carry.size > 8) carry.clear();
  for(const ln of lines){
    const e = /"epoch":(\d+)/.exec(ln);
    if(!e || +e[1] === 0) continue;
    const u = (/"scanner":"([0-9a-f]{12})"/.exec(ln) || [])[1] || "own";
    const r = newest.get(u) || { epoch:0, max:0 };
    if(+e[1] > r.epoch){ r.epoch = +e[1]; r.max = Math.max(r.max, nowLocal - r.epoch); newest.set(u, r); }
  }
}

function say(m){
  try{
    const bg = window.Capacitor && window.Capacitor.Plugins && window.Capacitor.Plugins.CfloBg;
    if(bg && bg.diagNote){ Promise.resolve(bg.diagNote({ m })).catch(()=>{}); return; }
  }catch(e){}
  console.log("[live] " + m);
}

function tick(){
  const ms = Date.now() - t0;
  if(ms < EVERY_MS) return;
  if(push || rx || posts.n || posts.fail){
    const kbs = (b, m)=>(b / 1024 / Math.max(1, m / 1000)).toFixed(1);
    const age = [...newest].map(([u, r])=>(u === "own" ? "own" : u.slice(0, 4)) + "=" + r.max).join(" ");
    say("live" + (push ? " push " + kbs(push.bytes, push.ms) + "KB/s refused=" + push.refused + " gap=" + push.gap + " wait=" + push.wait.join("/") : "") +
        " rx " + kbs(rx, ms) + "KB/s age " + (age || "-") + " post n=" + posts.n + " b=" + posts.bytes + " max=" + posts.max + " fail=" + posts.fail);
  }
  t0 = Date.now(); push = null; rx = 0; posts = freshPost();
  for(const r of newest.values()) r.max = 0;
}

export function initLiveStats(){
  state.onLiveStat = onStat;
  state.onLivePost = (bytes, ms, ok)=>{ posts.n++; posts.bytes += bytes; posts.max = Math.max(posts.max, Math.round(ms)); if(!ok) posts.fail++; };
  window.addEventListener("cflo-livelog", onPiece);
  setInterval(tick, 5000);
  window.addEventListener("cflo-bg-tick", tick);   // Android app: hidden-page timers are throttled
}
export const _test = { onStat, onPiece, tick, back(ms){ t0 -= ms; } };
