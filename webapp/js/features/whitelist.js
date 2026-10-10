// Whitelist editor: "Y" lists (WL=<n> + lines), "A:<rule>" adds, "E:<i>" removes, "W" reloads.
import { state } from '../state.js';
import { enc } from '../config.js';
import { $, log, esc } from '../util/dom.js';

export async function wlCmd(c, msg){
  try{ await state.chars.cmd.writeValue(enc.encode(c)); if(msg) log(msg); }
  catch(e){ log("Whitelist command failed: "+e.message); }
}
export function refreshWhitelist(){ state.dl={mode:null,expect:0,got:0,chunks:[],name:"",sink:"save"}; return wlCmd("Y"); }
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
    b.onclick=async()=>{ await wlCmd("E:"+i, "Removing rule "+(i+1)+"…"); setTimeout(refreshWhitelist,300); };
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
  $("wlPat").value=""; $("wlLabel").value="";
  setTimeout(refreshWhitelist,300);
}
export function wlKindChanged(){   // name rules match only the first 23 chars on the device
  const nm=/^n(exact|contains)$/.test($("wlKind").value);
  $("wlPat").maxLength = nm ? 23 : 40;
  if(nm && $("wlPat").value.length>23) $("wlPat").value=$("wlPat").value.slice(0,23);
}
export function openWhitelist(){ $("wlDlg").showModal(); refreshWhitelist(); }
