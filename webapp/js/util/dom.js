// Small DOM helpers used everywhere.

export const $ = id => document.getElementById(id);

export function log(msg){
  const el = $("log");
  el.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\n`;
  el.scrollTop = el.scrollHeight;
}

export function setConn(s){
  const p = $("conn");
  // Bluetooth icon in the app bar: connected (blue) / searching (amber) / disabled (gray).
  const icon = s === "connected" ? "bluetooth_connected" : s === "connecting" ? "bluetooth_searching" : "bluetooth_disabled";
  p.className = "conn " + (s === "connected" ? "up" : s === "connecting" ? "wait" : "down");
  p.title = s;
  p.innerHTML = '<span class="material-symbols-outlined">' + icon + '</span>';
}

// HTML-escape for text interpolated into innerHTML.
export function esc(s){
  return (s || "").replace(/[<>&]/g, c => ({ '<':'&lt;', '>':'&gt;', '&':'&amp;' }[c]));
}
