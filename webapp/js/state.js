// Cross-module mutable state.
//
// ES module exports are read-only live bindings, so a plain `export let connected` can't be
// reassigned by an importing module. Anything that more than one module both reads AND writes
// lives here as a property of this single object instead (object-property access is always
// current). Module-private state (detState, lmap, detMap, expandedMacs, …) stays local to its
// own module and is NOT duplicated here.
export const state = {
  // --- BLE connection ---
  chars: {},            // {time,gps,cmd,stat,log,dets} GATT characteristics (set on connect)
  device: null,         // BluetoothDevice
  connected: false,
  wifiOn: false,        // Wi-Fi download mode toggle
  scanning: false,      // authoritative value comes back via STATUS scan=
  paused: false,        // live-list view frozen (counts/scanning keep running) — mirrors the CYD PAUSE

  // --- scan provenance (STATUS sid/fw/dbgen) — tags live-POST batches so scan-analyzer knows which
  // scanner produced them (file uploads already carry the CYD's own {"evt":"meta"} first line) ---
  scannerId: "",        // stable eFuse-MAC id of the connected CYD (opaque)
  scannerFw: "",        // CYD firmware version string
  scannerDbGen: 0,      // sig-DB generation

  // --- LOGDATA download/stream reassembly (see ble/download.js) ---
  dl: { mode:null, expect:0, got:0, chunks:[], name:"scanlog.jsonl", sink:"save" },

  // --- GPS (see gps.js) ---
  gpsWatch: null,       // geolocation watch id, or null
  lastGps: { t:0, lat:0, lon:0 },

  // --- live detections / explore ---
  lastDetRows: [],      // latest live DETS snapshot, for the follower + category modals
  rids: {},             // live Remote-ID by lowercase MAC, replaced each snapshot (DETS v6 R: block)
  expAll: [],         // raw parsed rows of the loaded Explore session (never mutated in place)
  exploreWant: false,   // populateSessions auto-loads the newest session when set
};
