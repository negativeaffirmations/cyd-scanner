// Status-icon info modal: tap any app-bar status icon → one modal that names + describes every icon
// and shows its current state (from the latest STATUS key=val map in state.status). Re-renders live
// while open (ble/status.js calls renderStatusInfo on every notify; connection changes too).
import { state } from '../state.js';
import { $, esc } from '../util/dom.js';

let focusId = "";   // the icon that was tapped (its row is highlighted)

const TSRC = ["not set", "phone", "onboard GPS", "RTC (DS3231)"];

function ago(ms){
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
  return s < 60 ? s + " s ago" : s < 3600 ? Math.round(s / 60) + " min ago" : Math.round(s / 3600) + " h ago";
}

// One row per app-bar icon, in app-bar order. status() returns [text, tone] (tone: ok/warn/bad/"").
const ROWS = [
  { id:"timeState", name:"Clock",
    desc:"The scanner's clock, used to timestamp logged detections. White = phone-synced or RTC, " +
         "cyan = kept by the onboard GPS, grey = not set. Rows logged before a sync are back-filled " +
         "with real times once one arrives.",
    status(k){
      if(k.time !== "1") return ["Not set — rows carry time-since-boot only", "warn"];
      return ["Set · source: " + (TSRC[+k.tsrc || 0] || "unknown"), "ok"];
    } },
  { id:"gpsState", name:"Position (GPS)",
    desc:"The location stamped on each logged detection (used by the Map). The phone's GPS is " +
         "preferred; the scanner's onboard GPS module is the fallback (cyan, with its satellite count).",
    status(k){
      const src = +k.gpssrc || 0, sats = +k.sats || 0;
      const lg = state.lastGps, phone = lg && lg.t ? " · phone fix " + lg.lat.toFixed(5) + ", " + lg.lon.toFixed(5) + " (" + ago(lg.t) + ")" : "";
      if(k.gps !== "1") return ["No fix" + (sats ? " · onboard " + sats + " sat" + (sats === 1 ? "" : "s") + " in view" : "") + phone, "warn"];
      return [(src === 2 ? "Fix · onboard GPS, " + sats + " sat" + (sats === 1 ? "" : "s") : "Fix · phone") + phone, "ok"];
    } },
  { id:"db", name:"Signature DB",
    desc:"The surveillance-device signature rules (MAC vendor, names, BLE UUIDs, IE fingerprints) " +
         "used to score detections, loaded from the SD card. White = loaded.",
    status(k){
      if(k.db === "1") return ["Loaded from SD" + (k.dbgen ? " · generation " + k.dbgen : ""), "ok"];
      return ["Not loaded — using the built-in fallback rules", "warn"];
    } },
  { id:"linkState", name:"Scanner link (C5)",
    desc:"The wired link between the host and the C5 radio board, which does the actual Wi-Fi (2.4 + " +
         "5 GHz), BLE and 802.15.4 scanning. Light blue = up and scanning, white = up but not " +
         "scanning, gray = down (no reply from the C5 for ~8 s).",
    status(k){   // tones match the icon: light blue / white / gray
      if(k.link !== "1") return ["Down — the C5 isn't answering (power / wiring)", "muted"];
      return k.scan === "1" ? ["Up · scanning", "scan"] : ["Up · not scanning", "plain"];
    } },
  { id:"battState", name:"Battery",
    desc:"The scanner's battery level, read from its voltage divider. Red under 20 %. Grey = no " +
         "reading (no cell, on USB, or the sense input is out of range).",
    status(k){
      const p = +k.bat;
      if(!("bat" in k) || !Number.isFinite(p) || p < 0) return ["No reading", "warn"];
      return [p + " %", p < 20 ? "bad" : "ok"];
    } },
  { id:"conn", name:"Phone link (Bluetooth)",
    desc:"This web app's Bluetooth connection to the scanner. White = connected, yellow = connecting, " +
         "grey = disconnected. Everything else on this page needs it.",
    status(){
      const c = $("conn").className;
      if(c.includes("up"))   return ["Connected" + (state.device && state.device.name ? " to " + state.device.name : "") +
                                     (state.status.fw ? " · fw " + state.status.fw : ""), "ok"];
      if(c.includes("wait")) return ["Connecting…", "warn"];
      return ["Disconnected", "bad"];
    } },
];

// Clone the live app-bar icon (so the modal shows its exact current look), minus ids/titles.
function iconClone(id){
  const src = $(id); if(!src) return "";
  const c = src.cloneNode(true);
  [c, ...c.querySelectorAll("[id]")].forEach(n => n.removeAttribute("id"));
  c.removeAttribute("title");
  return c.outerHTML;
}

export function renderStatusInfo(){
  const dlg = $("statusDlg"); if(!dlg || !dlg.open) return;
  const k = state.status, live = state.connected;
  $("statusList").innerHTML = ROWS.map(r => {
    const [txt, tone] = (live || r.id === "conn") ? r.status(k) : ["— (not connected)", ""];
    return '<div class="sirow' + (r.id === focusId ? " focus" : "") + '">' +
      '<div class="siicon">' + iconClone(r.id) + '</div>' +
      '<div class="sibody"><div class="siname">' + esc(r.name) + '</div>' +
      '<div class="sistat ' + tone + '">' + esc(txt) + '</div>' +
      '<div class="sidesc">' + esc(r.desc) + '</div>' +
      (r.id === "conn" && !live ? '<button id="siConnect" class="sibtn">Connect</button>' : "") +
      '</div></div>';
  }).join("");
}

export function openStatusInfo(id){
  focusId = id || "";
  $("statusDlg").showModal();
  renderStatusInfo();
}
