// Per-device sighting trail (webapp-side).
//
// The live DETS stream carries no location (see detections.js), but STATUS carries the position the
// scanner is stamping on its log (scannerFix(): fresh phone, else onboard GPS). So each time a MAC
// appears in a live snapshot we log that position as a "seen here" point — the same wardriving model
// the Map page uses. Held / aged positions are skipped so a stale fix can't grow a fake trail. The device-
// detail modal then plots that trail. In-memory for the connected session only (cleared on disconnect),
// bounded per MAC and in the number of distinct MACs tracked.
import { gpsMovedM } from '../util/format.js';
import { scannerFix } from '../gps.js';

const trails = new Map();   // mac -> [{lat,lon,rssi,tier,t}] (oldest-first)
const MAX_PTS  = 240;       // per-MAC point cap (~a long walk); oldest dropped
const MIN_MOVE_M = 8;       // don't append a new point unless we moved this far from the last one
const MAX_MACS = 500;       // soft cap on distinct devices tracked (memory guard)

// Call once per completed live snapshot with its rows. No-op until we have a real GPS fix.
export function recordSightings(rows){
  const g = scannerFix();
  if (!g || g.fixAge !== undefined) return;       // no fix, or only a held / aged one
  const now = Date.now();
  for (const r of (rows || [])){
    if (!r.mac) continue;
    const tier = typeof r.tier === "number" ? r.tier : 0;
    let arr = trails.get(r.mac);
    if (!arr){
      if (trails.size >= MAX_MACS) continue;      // stop tracking new MACs past the cap
      arr = []; trails.set(r.mac, arr);
    }
    const last = arr[arr.length - 1];
    if (last && gpsMovedM(last.lat, last.lon, g.lat, g.lon) < MIN_MOVE_M){
      last.rssi = r.rssi; last.tier = tier; last.t = now;   // same spot: refresh, don't pile duplicates
      continue;
    }
    arr.push({ lat:g.lat, lon:g.lon, rssi:r.rssi, tier, t:now });
    if (arr.length > MAX_PTS) arr.shift();
  }
}
export function getSightings(mac){ return trails.get(mac) || []; }
export function clearSightings(){ trails.clear(); }
export function haveAnyFix(){ return !!scannerFix(); }
