// Per-scanner state reset hooks. A module that holds data that came FROM a scanner (session list, Explore rows,
// the device-sourced Map, whitelist view, fleet members, live-list buffers, status readouts) registers a reset
// here; ble/connection.js runs them all when a DIFFERENT scanner is about to connect (Switch scanner, or the
// chooser picking another one), so nothing from the previous unit survives. Data loaded from a local file stays.
// NEW file: old modules only import it, so a stale cached copy of one of them just skips its reset.
const hooks = [];
export function onUnitReset(fn){ hooks.push(fn); }
export function runUnitReset(){ for(const fn of hooks){ try{ fn(); }catch(e){ console.warn("unit reset hook failed", e); } } }
