// Fleet card (Settings) — docs/fleet-mode.md §10. Every op is CMD "M<seq>:<op>[:args]"; the scanner answers each
// with ONE "FLT=<n>:<seq>" header + <n> bytes on LOGDATA ("OK\t…" / "ERR\t<why>"). Own LOGDATA listener (like
// files.js), one request at a time. STATUS "flt=<role>,<joined>/<enrolled>,<ch>" drives the card state; the member
// list ("M:l") is polled every 5 s while the Settings page is open on a leader.
// Roles: a mini can lead (it keeps its phone link); a CYD can only be a client, and a CYD client has no phone
// link at all (it reboots into fleet mode), so this card manages a CYD only up to "make it a client".
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, esc, log, toast } from '../util/dom.js';
import { gattWrite } from '../ble/gattq.js';
import { askConfirm, listKeys } from '../ble/auth.js';
import { buildAddCmd } from '../ble/fleetcrypto.js?v=20261031d';

const REPLY_MS = 25000, POLL_MS = 5000;
const STATE_TXT = ["enrolled", "joining", "joined", "lost"], STATE_COL = ["#8b949e", "#d29922", "#3fb950", "#f85149"];

let pending = null, listenOn = null, seqCounter = 0, busy = false;
let members = [], lastFlt = "", lastPoll = 0, addCtr = 0;

function onFltData(ev){
  if (!pending) return;
  const dv = ev.target.value, v = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  if (pending.expect < 0) {
    const m = /^FLT=(\d+):(\d+)$/.exec(new TextDecoder().decode(v));
    if (!m || +m[2] !== pending.seq) return;     // another stream, or a late reply to a request we gave up on
    pending.expect = +m[1];
    if (pending.expect === 0) settle("");
    return;
  }
  pending.chunks.push(new Uint8Array(v)); pending.got += v.length;
  if (pending.got >= pending.expect) {
    const all = new Uint8Array(pending.got); let o = 0;
    for (const c of pending.chunks) { all.set(c, o); o += c.length; }
    settle(new TextDecoder().decode(all));
  }
}
function settle(text){ const p = pending; pending = null; clearTimeout(p.timer); p.resolve(text); }

// One "M<seq>:<body>" write -> its reply body. Rejects on "ERR\t…", a timeout, or no link.
function fltReq(body){
  return new Promise((resolve, reject) => {
    if (!state.connected || !state.chars.cmd || !state.chars.log) { reject(new Error("not connected")); return; }
    if (pending) { reject(new Error("busy")); return; }
    if (state.dl.mode !== null) { reject(new Error("a download is in progress")); return; }
    if (listenOn !== state.chars.log) { state.chars.log.addEventListener("characteristicvaluechanged", onFltData); listenOn = state.chars.log; }
    seqCounter = seqCounter % 255 + 1;
    const seq = seqCounter;
    pending = { seq, expect:-1, got:0, chunks:[],
      resolve: t => t.startsWith("ERR\t") ? reject(new Error(t.slice(4).trim() || "scanner error")) : resolve(t), reject,
      timer: setTimeout(() => { pending = null; reject(new Error("no reply from the scanner")); }, REPLY_MS) };
    gattWrite(state.chars.cmd, enc.encode("M" + seq + ":" + body))
      .catch(e => { if (pending) { clearTimeout(pending.timer); pending = null; } reject(e); });
  });
}

// flt=<role>,<joined>/<enrolled>,<ch>[,<canLead>]  (no canLead from the very first P2 bench builds: guess by name)
function fltStatus(){
  const m = /^(\d),(\d+)\/(\d+),(\d+)(?:,(\d))?$/.exec(state.status.flt || "");
  return m ? { role:+m[1], joined:+m[2], enrolled:+m[3], ch:+m[4], canLead: m[5] != null ? m[5] === "1" : null } : null;
}
function isMini(){ const f = fltStatus(); return f && f.canLead != null ? f.canLead : /-MINI-/i.test(state.scannerName || ""); }
const settingsOpen = () => $("page-settings").classList.contains("visible");

function show(id, on){ $(id).hidden = !on; }

export function renderFleet(){
  const f = fltStatus();
  const conn = state.connected && !!f;
  let txt;
  if (!state.connected) txt = "Connect to a scanner.";
  else if (!f) txt = "This scanner's firmware has no fleet mode (needs 0.20 or later).";
  else if (f.role === 2) txt = `Fleet leader · ${f.joined}/${f.enrolled} member${f.enrolled === 1 ? "" : "s"} joined · channel ${f.ch}`;
  else if (f.role === 1) txt = `Fleet client · ${f.joined ? "joined" : "searching for its leader"} · channel ${f.ch}`;
  else txt = isMini() ? "Not in a fleet. This scanner can lead one, or join one as a client."
                      : "Not in a fleet. A CYD can join a fleet as a client (a mini leads).";
  $("fltState").textContent = txt;
  show("fltLeaderBox", conn && f.role === 2);
  show("fltLeaderOnBtn", conn && f.role === 0 && isMini());
  show("fltClientBtn", conn && f.role === 0);
  show("fltLeaveBtn", conn && f.role === 1 && isMini());
  if (conn && f.role === 2) $("fltChSel").value = String(f.ch);
}

function renderMembers(){
  const box = $("fltMembers");
  if (!members.length) { box.innerHTML = '<div class="sub" style="padding:6px;margin:0">No members yet. Add a scanner below.</div>'; return; }
  box.innerHTML = members.map(m => {
    const live = m.state === 2;
    const sub = [m.sid, STATE_TXT[m.state] || "?", live ? `${m.rssi} dBm` : "", live ? `${m.age} s ago` : "", m.fw ? `fw ${m.fw}` : ""]
      .filter(Boolean).join(" · ");
    return `<div class="sessrow keyrow"><span class="material-symbols-outlined" style="color:${STATE_COL[m.state] || "#8b949e"}">radio_button_checked</span>` +
      `<div class="sessmain"><span class="sessdate">${esc(m.name)}</span><span class="sesssub">${esc(sub)}</span></div>` +
      (live ? `<button class="secondary keyforget" data-act="x" data-sid="${esc(m.sid)}" data-name="${esc(m.name)}">Release</button>` : "") +
      `<button class="danger keyforget" data-act="r" data-sid="${esc(m.sid)}" data-name="${esc(m.name)}">Remove</button></div>`;
  }).join("");
}

async function renderAddChoices(){
  const sel = $("fltAddSel");
  const have = new Set(members.map(m => m.sid));
  const keys = (await listKeys()).filter(k => k.sid !== state.scannerId && !have.has(k.sid));
  sel.innerHTML = keys.length
    ? keys.map(k => `<option value="${esc(k.sid)}">${esc(k.name || "Scanner")} (${esc(k.sid)})</option>`).join("")
    : '<option value="">No other scanner keys stored — add one in Scanner keys</option>';
  $("fltAddBtn").disabled = !keys.length;
}

export async function refreshMembers(){
  if (busy) return;
  busy = true;
  try {
    const t = await fltReq("l");
    addCtr = +(t.split("\n")[0].split("\t")[6] || 0);   // the leader's enrollment counter (header field 7)
    members = t.split("\n").slice(1).filter(Boolean).map(l => {
      const c = l.split("\t");
      return { slot:+c[0], sid:c[1], name:c[2], kg:+c[3], state:+c[4], rssi:+c[5], age:+c[6], fw:c[14] || "" };
    });
    renderMembers();
    await renderAddChoices();
  } catch (e) { if (!/busy|download/.test(e.message)) log("Fleet: " + e.message); }
  finally { busy = false; lastPoll = Date.now(); }
}

// One command with the card locked; reports the result. Returns the reply text or null. A tap during a member-list
// poll waits for it (up to ~6 s) instead of being refused.
async function run(body, okMsg){
  for (let i = 0; busy && i < 60; i++) await new Promise(r => setTimeout(r, 100));
  if (busy) { toast("Fleet: the scanner is busy, try again"); return null; }
  busy = true;
  try { const t = await fltReq(body); if (okMsg) toast(okMsg); log("Fleet: " + (okMsg || t)); return t; }
  catch (e) { toast("Fleet: " + e.message); log("Fleet: " + e.message); return null; }
  finally { busy = false; }
}

export async function fleetMakeClient(){
  const cyd = !isMini();
  if (!(await askConfirm({ title:"Make this scanner a fleet client?",
    msg:`${state.scannerName || "It"} restarts and joins any leader that has it as a member.` +
        (cyd ? " On a CYD the phone link turns OFF while it's in the fleet; leave from its screen (Settings → Leave fleet) or have the leader release it."
             : " It keeps its phone link."),
    ok:"Make client", cancel:"Cancel" }))) return;
  await run("j", "Restarting as a fleet client");
}
export async function fleetLeader(on){
  if (!on && !(await askConfirm({ title:"Stop leading the fleet?", msg:"The scanner restarts in normal mode. Its member list is kept.", ok:"Stop leading", cancel:"Cancel", danger:true }))) return;
  await run(on ? "L1" : "L0", on ? "Fleet leader on" : "Leaving the fleet (restarting)");
}
export async function fleetLeave(){
  if (!(await askConfirm({ title:"Leave the fleet?", msg:"The scanner restarts in normal mode.", ok:"Leave", cancel:"Cancel" }))) return;
  await run("L0", "Leaving the fleet (restarting)");
}
export async function fleetChannel(){
  const ch = +$("fltChSel").value;
  if (ch >= 1 && ch <= 13) await run("c:" + ch, "Fleet channel " + ch);
}
export async function fleetAdd(){
  const sid = $("fltAddSel").value;
  if (!sid) return;
  const k = (await listKeys()).find(x => x.sid === sid);
  if (!k) { toast("Fleet: that key is no longer stored"); return; }
  await refreshMembers();                        // fresh enrollment counter: each add command is single-use
  let body;
  try { body = await buildAddCmd(state.scannerId, k, addCtr); }
  catch (e) { toast("Fleet: " + e.message); return; }
  const t = await run(body, `Added ${k.name || sid}. It joins once it's a fleet client in range.`);
  body = null;
  if (t !== null) refreshMembers();
}
export async function onMemberClick(ev){
  const b = ev.target.closest("button[data-act]");
  if (!b) return;
  const { act, sid, name } = b.dataset;
  if (act === "r" && await askConfirm({ title:"Remove from the fleet?", msg:`${name} leaves the session now. Add it again to rejoin.`, ok:"Remove", cancel:"Cancel", danger:true })) {
    if (await run("r:" + sid, `Removed ${name}`) !== null) refreshMembers();
  }
  if (act === "x" && await askConfirm({ title:"Release this member?", msg:`${name} leaves fleet mode and restarts normally (a CYD gets its phone link back). It stays on the member list.`, ok:"Release", cancel:"Cancel" })) {
    if (await run("x:" + sid, `Released ${name}`) !== null) refreshMembers();   // the leader marks it gone at once
  }
}

// Called once from main.js: watch STATUS flt= and poll the member list while it matters.
export function initFleet(){
  setInterval(() => {
    const f = state.status.flt || "";
    if (f !== lastFlt || !state.connected) { lastFlt = f; renderFleet(); }
    const st = fltStatus();
    if (state.connected && st && st.role === 2 && settingsOpen() && Date.now() - lastPoll >= POLL_MS) refreshMembers();
    if (!state.connected && members.length) { members = []; renderMembers(); }
  }, 1000);
  renderFleet();
}
