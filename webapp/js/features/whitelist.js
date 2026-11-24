// Whitelist editor: "Y" lists (WL=<n> + lines), "A:<rule>" adds, "E:<i>" removes, "W" reloads.
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, log, esc } from '../util/dom.js';
import { onUnitReset } from '../unitreset.js';
import { claimWrite } from '../ble/syncwait.js';
import { gattWrite } from '../ble/gattq.js';

onUnitReset(()=>{ $("wlList").innerHTML=""; });   // the whitelist belongs to the scanner it was read from
// "Y" / "A:" / "E:" are answered with a WL= list: claim LOGDATA for it (ble/syncwait.js). While LOGDATA is busy an add or
// remove is still sent as a plain queued write (the scanner applies it after the transfer, and the default state.dl
// still takes its WL= reply); a list refresh just waits for the next open.
export async function wlCmd(c, msg){
  try{
    if(/^[AEY]/.test(c)){
      if(!await claimWrite(c)){
        if(c === "Y") return;
        await gattWrite(state.chars.cmd, enc.encode(c));
      }
    } else await gattWrite(state.chars.cmd, enc.encode(c));
    if(msg) log(msg);
  }catch(e){ log("Whitelist command failed: "+e.message); }
}
export async function refreshWhitelist(){ return wlCmd("Y"); }
export function populateWhitelist(text){
  const rows=(text||"").split("\n").filter(l=>l.trim());
  $("wlHdr").textContent=rows.length;
  const box=$("wlList"); box.innerHTML="";
  if(!rows.length){ box.innerHTML='<div class="sub" style="padding:6px">No rules.</div>'; return; }
  rows.forEach((r,i)=>{
    const f=r.split(","), kind=f[0], pat=f[1], mask=f[2], label=f.slice(3).join(",");
    const d=document.createElement("div"); d.className="det";
    d.innerHTML='<span style="flex:1;min-width:0;overflow-wrap:anywhere"><b>'+esc(kind)+'</b> '+esc(pat)+
      ' <span class="sub">['+esc(mask)+'] '+esc(label)+'</span></span>';
    const b=document.createElement("button"); b.className="danger"; b.textContent="Remove";
    b.style.cssText="width:auto;padding:.3em .7em;margin:0 0 0 .5em";
    b.onclick=async()=>{ await wlCmd("E:"+i, "Removing rule "+(i+1)+"…"); };   // its WL= reply refreshes the list
    d.appendChild(b); box.appendChild(d);
  });
}
export async function addWhitelistRule(){
  const kind=$("wlKind").value, pat=$("wlPat").value.trim(), mask=$("wlMask").value;
  const label=$("wlLabel").value.trim().replace(/[\x00-\x1f]/g," ");
  if(!pat || /[,\x00-\x1f]/.test(pat)){ log("Whitelist: pattern is required and can't contain commas."); return; }
  if(/^(mac|macpfx)$/.test(kind) && !/^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){2,5}$/.test(pat)){
    log("Whitelist: MAC must look like AA:BB:CC:DD:EE:FF (OUI prefix: 3-5 bytes)."); return; }
  if(kind==="mac" && pat.split(":").length!==6){ log("Whitelist: 'mac' needs all 6 bytes (use macpfx for a prefix)."); return; }
  if(kind==="blecid" && !/^[0-9a-fA-F]{1,4}$/.test(pat)){ log("Whitelist: company ID is up to 4 hex digits."); return; }
  const line=[kind,pat,mask,label].join(",");
  await wlCmd("A:"+line, "Adding rule: "+line);
  $("wlPat").value=""; $("wlLabel").value="";   // the A: reply (WL=) refreshes the list
}
export function wlKindChanged(){   // name rules match only the first 23 chars on the device
  const nm=/^n(exact|contains)$/.test($("wlKind").value);
  $("wlPat").maxLength = nm ? 23 : 40;
  if(nm && $("wlPat").value.length>23) $("wlPat").value=$("wlPat").value.slice(0,23);
}
// fw 0.39.0: a fleet leader shares its whole whitelist with its joined clients, and a client mutes its leader's
// rules on top of its own (the leader's rules are not listed here: they live on the leader).
function fleetNote(){
  const n = $("wlFleetNote"); if(!n) return;
  const t = state.fleetRole === "leader" ? "This scanner leads a fleet: every joined client mutes these devices too."
          : state.fleetRole === "client" ? "This scanner is a fleet client: it also mutes everything on its leader's whitelist (edit those on the leader)."
          : "";
  n.textContent = t; n.hidden = !t;
}
export function openWhitelist(){ fleetNote(); $("wlDlg").showModal(); refreshWhitelist(); }

// Fill the detail modal's action slot with a whitelist control for `mac`. Add is one tap
// (A:mac,<MAC>,A); removal is rule/index-based (a device may be matched by a prefix/name rule), so a
// whitelisted device links to the Whitelist manager instead. Only shown while connected (needs CMD).
export function fillDetailWhitelist(mac, muted){
  const el = $("detActions"); if (!el) return;
  if (!mac || !state.connected){ el.innerHTML = ""; return; }
  if (muted){
    el.innerHTML = '<button class="secondary" id="detWlBtn">✓ Whitelisted — manage rules</button>';
    $("detWlBtn").onclick = ()=>{ $("detDlg").close(); openWhitelist(); };
  } else {
    el.innerHTML = '<button class="secondary" id="detWlBtn">+ Add to whitelist</button>';
    $("detWlBtn").onclick = async ()=>{
      await wlCmd("A:mac," + mac + ",A,", "Whitelisting " + mac + "…");
      $("detDlg").close();   // the A: reply (WL=) refreshes the whitelist
    };
  }
}
