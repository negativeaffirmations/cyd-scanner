// Data page: the saved-log cache readout + "Clear saved logs" button (data/logcache.js).
// NEW file, wired from main.js, so it is live the moment the page updates.
import { $, toast } from '../util/dom.js';
import { onCacheChange, totalBytes, entryCount, clearAll, cacheUsable, CAP_BYTES } from '../data/logcache.js';

const fmt = b=>b>=1048576 ? (b/1048576).toFixed(1)+" MB" : Math.max(b ? 1 : 0, Math.round(b/1024))+" KB";

function render(){
  const btn = $("cacheClearBtn"), info = $("cacheInfo");
  if(!btn || !info) return;
  if(!cacheUsable()){ info.textContent = "Saved logs: unavailable in this browser (every action fetches over Bluetooth)."; btn.disabled = true; btn.textContent = "Clear saved logs"; return; }
  const n = entryCount(), b = totalBytes();
  info.textContent = n ? "Saved on this phone: "+n+" file"+(n===1?"":"s")+", "+fmt(b)+" of "+fmt(CAP_BYTES)+" max. Fetched logs are reused instead of re-downloaded over Bluetooth."
                       : "Nothing saved yet. Fetched logs are kept on this phone and reused instead of re-downloaded over Bluetooth.";
  btn.disabled = !n;
  btn.textContent = "Clear saved logs"+(n ? " ("+fmt(b)+")" : "");
}

export function initCacheUI(){
  onCacheChange(render);
  $("cacheClearBtn").onclick = async ()=>{
    if(!confirm("Delete the saved copies of scanner logs from this phone?\nThe logs on the scanner's SD card are not touched; they will be fetched again next time.")) return;
    await clearAll();
    toast("Saved logs cleared.");
  };
  render();
}
