// Copies the web app (../webapp) into www/, the folder Capacitor bundles into the APK, and adds the app-only layer
// (native/: the Web Bluetooth stand-in, platform stand-ins, app CSS) to www/index.html. The web app itself is not
// modified, so the GitHub Pages site never loads any of it. See docs/android-app.md.
import { cpSync, rmSync, mkdirSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, basename } from "node:path";
import { fileURLToPath } from "node:url";

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
  html.slice(js2);
writeFileSync(indexPath, html);
console.log("www/ <- " + src + " (+ native/ layer)");
