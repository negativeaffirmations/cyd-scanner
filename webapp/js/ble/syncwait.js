// Sharing LOGDATA with the live mirror (features/livesync.js). NEW file: old modules only import it.
//  syncWait()        -> wait while a live-sync pass holds LOGDATA. Registers as a waiter, so the pass stops at its next
//                       request boundary (it checks state.liveSyncWaiters) instead of starting another one. A whole-file
//                       copy can't be stopped (the scanner finishes sending it whatever the phone does), so then this
//                       waits for it to end, with a toast saying why.
//  claimWrite(cmd)   -> for commands answered with a list (session list, wipe-all, whitelist): wait, return null when
//                       LOGDATA is busy, else claim state.dl for the reply, write through the GATT queue and return the
//                       claim; a failed write releases it. holdUntil keeps download.js's stall watchdog off it for
//                       HOLD_MS (the scanner reads every log's head + tail to build a list; a stray notify must not
//                       shorten that).
import { state } from '../state.js';
import { enc } from '../config.js';
import { toast } from '../util/dom.js';
import { reqBusy } from './xfer.js';
import { gattWrite } from './gattq.js';
import { resetDl } from './transfer.js';

const WAIT_MS = 15000, HOLD_MS = 45000;

export async function syncWait(){
  if(!state.liveSyncP) return;
  state.liveSyncWaiters = (state.liveSyncWaiters || 0) + 1;
  try{
    if(state.liveSyncFull){
      toast("Finishing the live copy of the scan first…", {work:true, sticky:true});
      await state.liveSyncP;
      toast("Live copy finished — continuing.");
    }
    else await Promise.race([state.liveSyncP, new Promise(r=>setTimeout(r, WAIT_MS))]);
  }
  finally{ state.liveSyncWaiters--; }
}

export async function claimWrite(cmd){
  await syncWait();
  if(reqBusy()) return null;
  const claim = { mode:null, expect:0, got:0, chunks:[], name:"", sink:"save", waiting:true, lastRx:Date.now(),
                  holdUntil:Date.now() + HOLD_MS };
  state.dl = claim;
  try{ await gattWrite(state.chars.cmd, enc.encode(cmd)); }
  catch(e){ if(state.dl === claim) resetDl(); throw e; }
  return claim;
}
