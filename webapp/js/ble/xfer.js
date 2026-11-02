// Shared transfer bookkeeping (NEW file: other modules only import it).
// - Connection generation: bumped whenever a connection starts or drops. A multi-part fetch captures it and
//   aborts when it changes, so one scanner's bytes can never be joined with, or cached under, another's.
// - Single flight: LOGDATA carries one reply at a time. A request is "in flight" from the moment it is written
//   (state.dl.waiting, before any header arrives) until its payload is complete; every requester checks reqBusy()
//   first instead of overwriting state.dl.
import { state } from '../state.js';

let gen = 0;
export const connGen = ()=>gen;
export const bumpGen = ()=>{ gen++; };

export const reqBusy = ()=>state.dl.mode !== null || !!state.dl.waiting;
export const anyBusy = ()=>reqBusy() || (state.xferBusy || 0) > 0;

// Drop the pending request: reject its promise (if it has one) and reset the reassembly state.
export function cancelTransfer(reason){
  const rej = state.dl.reject;
  state.dl = { mode:null, expect:0, got:0, chunks:[], name:"scanlog.jsonl", sink:"save" };
  if(rej) rej(new Error(reason));
}
