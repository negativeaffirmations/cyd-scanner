// Copies the web app (../webapp) into www/, the folder Capacitor bundles into the APK, and adds the app-only layer
// (native/: the Web Bluetooth stand-in, platform stand-ins, app CSS) to www/index.html. The web app itself is not
// modified, so the GitHub Pages site never loads any of it. See docs/android-app.md.
import { cpSync, rmSync, mkdirSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, basename } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";

// Libraries the web app loads from CDNs, bundled so the app opens with no signal (map tiles still need it).
// Pinned by SHA-256: a changed file fails the build instead of shipping.
const UNPKG = "https://unpkg.com/";
const VENDOR = [
  ["leaflet@1.9.4/dist/leaflet.css", "a7837102824184820dfa198d1ebcd109ff6d0ff9a2672a074b9a1b4d147d04c6"],
  ["leaflet@1.9.4/dist/leaflet.js", "db49d009c841f5ca34a888c96511ae936fd9f5533e90d8b2c4d57596f4e5641a"],
  ["leaflet@1.9.4/dist/images/layers.png", "1dbbe9d028e292f36fcba8f8b3a28d5e8932754fc2215b9ac69e4cdecf5107c6"],
  ["leaflet@1.9.4/dist/images/layers-2x.png", "066daca850d8ffbef007af00b06eac0015728dee279c51f3cb6c716df7c42edf"],
  ["leaflet@1.9.4/dist/images/marker-icon.png", "574c3a5cca85f4114085b6841596d62f00d7c892c7b03f28cbfa301deb1dc437"],
  ["leaflet@1.9.4/dist/images/marker-icon-2x.png", "00179c4c1ee830d3a108412ae0d294f55776cfeb085c60129a39aa6fc4ae2528"],
  ["leaflet@1.9.4/dist/images/marker-shadow.png", "264f5c640339f042dd729062cfc04c17f8ea0f29882b538e3848ed8f10edb4da"],
  ["leaflet.markercluster@1.5.3/dist/MarkerCluster.css", "614dea0a98ff3f4ead74f04918f6b1d1b9ba435c25b5fc23b21a394d1e3e4d87"],
  ["leaflet.markercluster@1.5.3/dist/MarkerCluster.Default.css", "61258232d98d64dc2a7b1e02130d67421bc5b9bda5994eef70228ff97570c170"],
  ["leaflet.markercluster@1.5.3/dist/leaflet.markercluster.js", "1e4e1d22972a3926f48598e0caf14e3fe7049835d428a344fed4f9e3665b3508"],
];
// Google Fonts answers with woff2 files for a current Chrome; the CSS and the fonts are saved side by side.
const FONT_UA = "Mozilla/5.0 (Linux; Android 16) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Mobile Safari/537.36";

async function get(url, headers){
  const r = await fetch(url, { headers });
  if (!r.ok) throw new Error(url + " -> HTTP " + r.status);
  return Buffer.from(await r.arrayBuffer());
}

const here = dirname(fileURLToPath(import.meta.url));
const src = join(here, "..", "..", "webapp");
const nat = join(here, "..", "native");
const out = join(here, "..", "www");
const SKIP = new Set(["scss", "node_modules", "package.json", "package-lock.json"]);

if (!existsSync(join(src, "index.html"))) throw new Error("web app not found at " + src);
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
cpSync(src, out, { recursive: true, filter: p => !SKIP.has(basename(p)) && !p.endsWith(".map") });
cpSync(nat, join(out, "native"), { recursive: true });

// Inject: app CSS after the web app's stylesheet; the stand-ins as classic scripts before its module script
// (classic scripts run first, so navigator.bluetooth exists before any module looks at it).
const indexPath = join(out, "index.html");
let html = readFileSync(indexPath, "utf8");
const cssAt = html.search(/<link[^>]+href="style\.css[^"]*"[^>]*>/);
const jsAt = html.search(/<script\s+type="module"[^>]*src="js\/main\.js/);
if (cssAt < 0 || jsAt < 0) throw new Error("index.html: style.css link or main.js module script not found");
const cssEnd = html.indexOf(">", cssAt) + 1;
html = html.slice(0, cssEnd) + '\n<link rel="stylesheet" href="native/app.css">' + html.slice(cssEnd);
const js2 = html.search(/<script\s+type="module"[^>]*src="js\/main\.js/);
html = html.slice(0, js2) +
  '<script src="native/shim-bluetooth.js"></script>\n<script src="native/shim-platform.js"></script>\n' +
  '<script src="native/shim-background.js"></script>\n' +
  html.slice(js2);
// Bundle the CDN files: download (hash-checked), save under www/vendor/, point index.html at the local copies.
const vend = join(out, "vendor");
for (const [path, sha] of VENDOR) {
  const buf = await get(UNPKG + path);
  const got = createHash("sha256").update(buf).digest("hex");
  if (got !== sha) throw new Error("vendor file changed: " + path + " (sha256 " + got + ")");
  const dest = join(vend, path);
  mkdirSync(dirname(dest), { recursive: true });
  writeFileSync(dest, buf);
  html = html.split('"' + UNPKG + path + '"').join('"vendor/' + path + '"');
}
let fontN = 0;
for (const m of [...html.matchAll(/href="(https:\/\/fonts\.googleapis\.com\/css2\?[^"]+)"/g)]) {
  const cssUrl = m[1].replace(/&amp;/g, "&");
  let css = (await get(cssUrl, { "User-Agent": FONT_UA })).toString("utf8");
  for (const f of [...new Set(css.match(/https:\/\/fonts\.gstatic\.com\/[^)\s'"]+/g) || [])]) {
    const name = "f" + createHash("sha1").update(f).digest("hex").slice(0, 12) + ".woff2";
    mkdirSync(join(vend, "fonts"), { recursive: true });
    writeFileSync(join(vend, "fonts", name), await get(f));
    css = css.split(f).join(name);
  }
  const cssName = "fonts/font" + (++fontN) + ".css";
  writeFileSync(join(vend, cssName), css);
  html = html.split('"' + m[1] + '"').join('"vendor/' + cssName + '"');
}
html = html.replace(/<link rel="preconnect"[^>]*>\r?\n?/g, "");   // font-host hints: nothing loads from there now
if (/https:\/\/(unpkg\.com|fonts\.googleapis\.com|fonts\.gstatic\.com)/.test(html)) throw new Error("index.html still loads a CDN file");
writeFileSync(indexPath, html);
console.log("www/ <- " + src + " (+ native/ layer, " + VENDOR.length + " vendor files, " + fontN + " font sheets)");
