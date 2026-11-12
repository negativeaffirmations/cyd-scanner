// Upload one .cflo scan archive to the paired scan-analyzer (docs/cflo-format.md §4): POST /api/v1/uploads with the
// archive as the body. A server that predates .cflo answers 415 (and 413 over its old 50 MB limit) and stores
// nothing, so on those two codes the caller falls back to today's per-file NDJSON bodies. Never keyed on `detail`.
// remote.js hands its server helpers over as state.remoteApi (an internal hook, so a stale cached remote.js just
// means no archive upload: we report "fallback").
import { state } from '../state.js';
import { log, toast } from '../util/dom.js';
import { CFLO_TYPE } from '../data/cflo.js';

// -> true (stored) | false (failed) | "auth" (401/403: stop) | "fallback" (send plain NDJSON instead)
export async function uploadCflo(blob, manifest, name){
  const ra = state.remoteApi;
  if(!ra) return "fallback";
  const nm = name || "scan.cflo";
  log("Remote: uploading "+nm+" ("+blob.size+" bytes, "+manifest.files.length+" file(s))…");
  toast("Uploading "+nm+" ("+Math.max(1, Math.round(blob.size/1024))+" KB)…", {work:true, sticky:true});
  let r;
  try{
    r = await fetch(ra.api("/api/v1/uploads"), {method:"POST", body:blob,
      headers: ra.authHeaders({"Content-Type":CFLO_TYPE, "X-Session-Name":nm.slice(0,120), "X-Client-Tz":ra.clientTz()})});
  }catch(e){
    log("Remote: upload error — "+e.message+" (HTTPS + server CORS for "+location.origin+" required).");
    toast("Upload error — "+e.message, {err:true});
    return false;
  }
  if(r.status===415 || r.status===413){
    log("Remote: the server did not take the archive ("+r.status+"); sending the files one by one.");
    return "fallback";
  }
  if(r.status===401 || r.status===403){
    log("Remote: upload rejected ("+r.status+") — the device key may be revoked. Re-pair in Settings.");
    toast("Upload rejected ("+r.status+") — re-pair in Settings.", {err:true});
    return "auth";
  }
  if(!r.ok){
    const j = await r.json().catch(()=>null);
    const why = j && typeof j.detail==="string" ? " — "+j.detail.slice(0,160) : "";
    log("Remote: upload failed ("+r.status+" "+r.statusText+")"+why+".");
    toast("Upload failed ("+r.status+")"+why, {err:true});
    return false;
  }
  const j = await r.json().catch(()=>null);
  if(!j){ log("Remote: upload accepted."); toast("Uploaded "+nm+" — accepted."); return true; }
  const s = j.sightings??0, ev = j.events??0, nf = Array.isArray(j.files) ? j.files.length : manifest.files.length;
  const dup = Array.isArray(j.files) ? j.files.filter(f=>f.duplicate).length : 0;
  const msg = j.duplicate ? "Already on the server — nothing new in "+nm+"."
    : "Uploaded "+nm+": "+s+" new sighting(s), "+ev+" new event(s) from "+nf+" file(s)"+(dup ? " ("+dup+" already on the server)" : "")+".";
  log("Remote: "+msg); toast(msg);
  return true;
}
