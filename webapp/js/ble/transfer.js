// Promise-based BLE LOGDATA transfer, for flows that queue several fetches back to back (a scan's
// part files, multi-select download/upload/delete). Sends one CMD, resolves with the payload text
// once download.js has reassembled it (any header type: SIZE= file, SESS= list, WL= whitelist;
// "" for an empty payload). The device handles ONE request at a time, so callers must await each
// call before the next. Rejects if nothing arrives for IDLE_MS.
import { state } from '../state.js';
import { enc } from '../config.js';

const IDLE_MS = 20000;

export function resetDl(){ state.dl = { mode:null, expect:0, got:0, chunks:[], name:"scanlog.jsonl", sink:"save" }; }

export function bleFetch(cmd, name){
  return new Promise((resolve, reject)=>{
    if(!state.connected || !state.chars.cmd){ reject(new Error("not connected")); return; }
    if(state.dl.mode!==null){ reject(new Error("another transfer is in progress")); return; }
    let timer = null;
    const finish = (fn, v)=>{ clearInterval(timer); fn(v); };
    state.dl = { mode:null, expect:0, got:0, chunks:[], name:name||"file", sink:"promise", lastRx:Date.now(),
                 resolve: t=>finish(resolve, t), reject: e=>finish(reject, e) };
    const mine = state.dl;
    timer = setInterval(()=>{
      if(state.dl!==mine){ clearInterval(timer); return; }   // completed (download.js reset state.dl)
      if(Date.now()-mine.lastRx > IDLE_MS){ resetDl(); finish(reject, new Error("transfer timed out")); }
    }, 1000);
    state.chars.cmd.writeValue(enc.encode(cmd)).catch(e=>{ if(state.dl===mine) resetDl(); finish(reject, e); });
  });
}
