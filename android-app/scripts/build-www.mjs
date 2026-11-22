// Copies the web app (../webapp) into www/, the folder Capacitor bundles into the APK.
// The web app itself is not modified: anything app-only is added here (docs/android-app.md).
import { cpSync, rmSync, mkdirSync, existsSync } from "node:fs";
import { dirname, join, basename } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const src = join(here, "..", "..", "webapp");
const out = join(here, "..", "www");
const SKIP = new Set(["scss", "node_modules", "package.json", "package-lock.json"]);

if (!existsSync(join(src, "index.html"))) throw new Error("web app not found at " + src);
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
cpSync(src, out, {
  recursive: true,
  filter: p => !SKIP.has(basename(p)) && !p.endsWith(".map"),
});
console.log("www/ <- " + src);
