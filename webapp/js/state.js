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

  // --- LOGDATA download/stream reassembly (see ble/download.js) ---
  dl: { mode:null, expect:0, got:0, chunks:[], name:"scanlog.jsonl", sink:"save" },

  // --- GPS (see gps.js) ---
  gpsWatch: null,       // geolocation watch id, or null
  lastGps: { t:0, lat:0, lon:0 },

  // --- live detections / explore ---
  lastDetRows: [],      // latest live DETS snapshot, for the follower + category modals
  expAll: [],           // raw parsed rows of the loaded Explore session (never mutated in place)
  exploreWant: false,   // populateSessions auto-loads the newest session when set
};
