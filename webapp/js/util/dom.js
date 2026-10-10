// Small DOM helpers used everywhere.

export const $ = id => document.getElementById(id);

export function log(msg){
  const el = $("log");
  el.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\n`;
  el.scrollTop = el.scrollHeight;
}

export function setConn(s){
  const p = $("conn");
  p.textContent = s;
  p.className = "pill " + (s === "connected" ? "up" : s === "connecting" ? "wait" : "down");
}

// HTML-escape for text interpolated into innerHTML.
export function esc(s){
  return (s || "").replace(/[<>&]/g, c => ({ '<':'&lt;', '>':'&gt;', '&':'&amp;' }[c]));
}
