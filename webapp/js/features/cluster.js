// Grouping for maps of server devices (one dot per device): dots that overlap on screen are drawn as one bubble, and a
// tap on the bubble fans its dots out in place (Leaflet.markercluster's spiderfy), at any zoom. Used by the "Server
// data" dialog map (serverfind.js) and the Map page's "Server devices" layer (servermap.js).
//
// Same setup as scan-analyzer's scan-review map (bridge 2026-11-21 16:52): maxClusterRadius 24 px, no zoom on a tap,
// no coverage outline. The bubble is neutral grey with the device count; when any device in it has a threat category,
// a ring around it is split into one arc per category (CAT_COL, the app-wide category colours), sized by how many
// distinct devices are in that category, plus a red badge with the number of threat devices.
// Each marker must carry options.mac and options.cat (CAT_COL index, 0 = none) BEFORE it is added: the icon reads them.
// Without the library (CDN blocked) it falls back to a plain layer group: stacked again, but nothing breaks.
// NEW file (stale-module rule).
import { CAT_COL } from '../config.js';

export const CLUSTER_OK = () => typeof L !== "undefined" && typeof L.markerClusterGroup === "function";
const CAT_NAME = ["", "Flock", "Axon", "ALPR", "Cam", "Ring", "Raven", "Glass", "Tracker", "Drone", "Deauth", "Flipper", "Skim"];
const RING_R = 18, RING_C = 2 * Math.PI * RING_R;

function clusterIcon(cluster){
  const n = cluster.getChildCount();
  const byCat = new Map();                                   // category index -> Set of MACs
  for(const m of cluster.getAllChildMarkers()){
    const c = m.options.cat || 0;
    if(!c) continue;
    if(!byCat.has(c)) byCat.set(c, new Set());
    byCat.get(c).add(m.options.mac);
  }
  const cats = [...byCat].map(([c, s]) => [c, s.size]).sort((a, b) => b[1] - a[1]);
  const threats = cats.reduce((t, [, k]) => t + k, 0);
  let ring = "", badge = "";
  if(threats){
    let off = 0;
    ring = `<svg class="tc-ring" viewBox="0 0 40 40">` + cats.map(([c, k]) => {
      const len = (k / threats) * RING_C;
      const seg = `<circle cx="20" cy="20" r="${RING_R}" stroke="${CAT_COL[c]}" ` +
        `stroke-dasharray="${len.toFixed(2)} ${RING_C.toFixed(2)}" stroke-dashoffset="${(-off).toFixed(2)}"/>`;
      off += len;
      return seg;
    }).join("") + `</svg>`;
    badge = `<b class="tc-badge">${threats}</b>`;
  }
  return L.divIcon({ html: `${ring}<div><span>${n}</span></div>${badge}`,
                     className: "marker-cluster tc" + (threats ? " tc-threat" : ""), iconSize: L.point(40, 40) });
}

function clusterTip(cluster){
  const n = cluster.getChildCount();
  const byCat = new Map();
  for(const m of cluster.getAllChildMarkers()){
    const c = m.options.cat || 0;
    if(c){ if(!byCat.has(c)) byCat.set(c, new Set()); byCat.get(c).add(m.options.mac); }
  }
  const parts = [...byCat].sort((a, b) => b[1].size - a[1].size).map(([c, s]) => s.size + " " + CAT_NAME[c]);
  const k = [...byCat.values()].reduce((t, s) => t + s.size, 0);
  return n + " device" + (n === 1 ? "" : "s") + (k ? " · " + k + " threat device" + (k === 1 ? "" : "s") + ": " + parts.join(", ") : "") +
         " · tap to spread them out";
}

// A layer for device dots, added to `map`. Add markers with addLayers([...]) in bulk; clearLayers() to empty it.
export function deviceClusterLayer(map){
  if(!CLUSTER_OK()) return L.layerGroup().addTo(map);
  const g = L.markerClusterGroup({ maxClusterRadius: 24, showCoverageOnHover: false, zoomToBoundsOnClick: false,
                                   iconCreateFunction: clusterIcon });
  g.on("clusterclick", e => e.layer.spiderfy());
  g.on("clustermouseover", e => { e.layer.bindTooltip(clusterTip(e.layer)).openTooltip(); });
  return g.addTo(map);
}

// Bulk add that also works on the plain-layer fallback.
export function addDots(layer, markers){
  if(typeof layer.addLayers === "function") layer.addLayers(markers); else markers.forEach(m => layer.addLayer(m));
}
