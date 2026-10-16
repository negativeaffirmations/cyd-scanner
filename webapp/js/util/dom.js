// Small DOM helpers used everywhere.

export const $ = id => document.getElementById(id);

export function log(msg){
  const el = $("log");
  el.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\n`;
  el.scrollTop = el.scrollHeight;
}

export function setConn(s){
  const p = $("conn");
  // Bluetooth icon in the app bar: connected (white) / searching (yellow) / disabled (gray).
  const icon = s === "connected" ? "bluetooth" : s === "connecting" ? "bluetooth_searching" : "bluetooth_disabled";
  p.className = "conn " + (s === "connected" ? "up" : s === "connecting" ? "wait" : "down");
  p.title = s;
  p.innerHTML = '<span class="material-symbols-outlined">' + icon + '</span>';
}

// HTML-escape for text interpolated into innerHTML.
export function esc(s){
  return (s || "").replace(/[<>&]/g, c => ({ '<':'&lt;', '>':'&gt;', '&':'&amp;' }[c]));
}

// Transient on-screen toast. The Activity log lives only on the Home page, so actions taken from
// other pages (upload/download) need feedback that shows wherever the user is. opts: {work} = a
// pending "working…" state, {err} = an error, {sticky} = stay until the next toast() call,
// {ms} = custom auto-dismiss (default ~4.5 s). Call with the same handle to replace in place.
let toastTimer = null;
export function toast(msg, opts){
  const el = $("toast"); if(!el) return;
  const o = opts || {};
  el.textContent = msg;
  el.className = "toast show" + (o.work ? " work" : "") + (o.err ? " err" : "");
  el.hidden = false;
  if(toastTimer){ clearTimeout(toastTimer); toastTimer = null; }
  const ms = o.sticky ? 0 : (o.ms || 4500);
  if(ms > 0) toastTimer = setTimeout(()=>{ el.classList.remove("show"); el.hidden = true; toastTimer = null; }, ms);
}
