// Fleet coverage: which unit is assigned which band. Pure logic, no DOM, so it can be tested in Node.
// Role names are the firmware's planRoleName(). "assigned" is deliberate: a BLE-role unit still runs AP scans, it
// just leaves passive capture to the Wi-Fi roles.
export const ROLE_OPTIONS = [
  ["auto",   "Auto"],
  ["full",   "Everything"],
  ["wifi",   "Wi-Fi, both bands"],
  ["wifi24", "Wi-Fi 2.4 GHz"],
  ["wifi5",  "Wi-Fi 5 GHz"],
  ["ble",    "BLE"],
];
const COVERS = { full:["2.4 GHz", "5 GHz", "BLE"], wifi:["2.4 GHz", "5 GHz"], wifi24:["2.4 GHz"], wifi5:["5 GHz"], ble:["BLE"] };
const BANDS = ["2.4 GHz", "5 GHz", "BLE"];

// units: [{ name, role }] for the units in the fleet right now (the leader and its JOINED members).
// Returns { lines:[{ band, names }], missing:[band] }.
export function coverage(units){
  const lines = BANDS.map(band => ({ band, names: units.filter(u => (COVERS[u.role] || []).includes(band)).map(u => u.name) }));
  return { lines, missing: lines.filter(l => !l.names.length).map(l => l.band) };
}
