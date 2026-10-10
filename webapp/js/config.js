// Shared constants for the CYD Scanner web app.
// Values that are used in more than one module live here; single-use constants stay next
// to the code that uses them (e.g. CSV_COLS in data/parse.js, EVT_STYLE in features/map.js).

// --- BLE GATT: service + characteristic UUIDs (must match src/cyd/phone.*) ---
export const SVC  = "9a1e0000-2b7e-4c1a-9b00-1a2b3c4d5e6f";
export const TIME = "9a1e0001-2b7e-4c1a-9b00-1a2b3c4d5e6f";
export const GPS  = "9a1e0002-2b7e-4c1a-9b00-1a2b3c4d5e6f";
export const CMD  = "9a1e0003-2b7e-4c1a-9b00-1a2b3c4d5e6f";
export const STAT = "9a1e0004-2b7e-4c1a-9b00-1a2b3c4d5e6f";
export const LOG  = "9a1e0005-2b7e-4c1a-9b00-1a2b3c4d5e6f";
export const DETS = "9a1e0006-2b7e-4c1a-9b00-1a2b3c4d5e6f";

// One shared text encoder for every BLE write.
export const enc = new TextEncoder();

// frames/s the C5 reports before the alert banner shows (matches the device).
export const DEAUTH_ALERT = 20;

// DETS trailing field: decimal DetFlags bits -> short badge label + title.
export const FLAG_BADGES = [[8,"iB","iBeacon"],[16,"FM","Find My"],[32,"PWN","Pwnagotchi"],[64,"ET","Evil Twin"],[128,"RID","Drone Remote ID"]];

// Threat-tier colors / names (index = none / suspect / likely / confirmed).
export const TIER_COL = ["#3388ff","#e3b341","#db8b2a","#f85149"];
export const TIER_NAMES = ["none","suspect","likely","confirmed"];
