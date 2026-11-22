# COINTEL-FLO Android app — stage 1 plan

Status: **PLAN, not started** (2026-11-21). Owner decisions are marked **[DECIDE]**.

## Where this fits

The app is built in two stages (owner-approved 2026-11-21):

1. **Stage 1 (this plan): the web app as an Android app.** The existing web app (`webapp/`) runs inside a
   native shell (Capacitor), bundled into the APK. Bluetooth moves from Web Bluetooth (which Android's web
   view does not have) to a native BLE plugin. Builds are published on GitHub and installed / updated on the
   phone with Obtainium. When stage 1 is done the app does everything the web app does, no browser flags needed.
2. **Stage 2 (later): background.** A foreground service, then a screen-off drive test. If the web code keeps
   running, stop there; if not, move the BLE link, GPS / time push, live upload and notifications into native code.

Stage 1 deliberately changes **nothing** for the scanners (no firmware work) and **nothing** for the web app on
GitHub Pages, which keeps working as it does today.

## What you will be able to do at the end of stage 1

- Install **COINTEL-FLO** from GitHub through Obtainium, and get every later build as a one-tap update. No cable.
- Connect to any of your scanners from the app. Units you have used before are listed by name, and the
  "Switch scanner" button sees which ones are in range without any Chrome / Vivaldi flags.
- Everything the web app does today: Home, Scanner, Map, Data (downloads, uploads, time-range export, SD files),
  Settings (devices, notifications, fleet, remote server), alerts.
- Downloads save to the phone's Downloads folder or go to the share sheet.

Not yet in stage 1: staying connected with the screen off (that is stage 2). Keep the screen on as you do now.

## How the code is laid out

```
webapp/               unchanged, still served by GitHub Pages
android-app/          NEW: the Capacitor project
  package.json        Capacitor + plugins (pinned versions)
  capacitor.config.json
  scripts/build-www.mjs   copies webapp/ into www/ and adds the native layer
  native/             NEW web-side files that only the app loads
    shim-bluetooth.js navigator.bluetooth on top of the native BLE plugin
    shim-platform.js  downloads, notifications, links, back button, QR scan
  android/            the generated Android Studio project (committed, minus build output)
.github/workflows/android.yml   build, sign, publish
```

**The key idea: a Web Bluetooth stand-in.** The web app talks to Bluetooth in about 60 places across 20 files
(`writeValue`, `startNotifications`, `getDevices`, `watchAdvertisements`, …). Instead of changing all of them,
`shim-bluetooth.js` provides the same `navigator.bluetooth` objects (device, GATT server, service,
characteristic, the `characteristicvaluechanged` / `gattserverdisconnected` events) backed by the native
plugin. The web app's code stays the same file for the browser and the app, so a fix lands in both. The shim
is only loaded inside the app, injected by `build-www.mjs`, so the Pages site never sees it.

The same trick covers the other browser features the web view lacks (`shim-platform.js`):

| Web app uses | In the app |
|---|---|
| Web Bluetooth (`ble/*`, `switcher.js`, `devices.js`) | native BLE plugin through the shim |
| Chrome's device chooser | an in-app scanner list: remembered units by name (Android resolves a bonded scanner's private address to its identity, so we can recognise it), others as "New scanner" |
| `watchAdvertisements` presence (`switcher.js`) | a native scan filtered on the CFLO service UUID |
| `<a download>` saves (`util/dom.js`) | write to Downloads, or the share sheet |
| Notifications + service worker (`notify.js`, `notify-sw.js`) | native local notifications |
| `window.open` (Wi-Fi download page, `download.js`, `wififlow.js`) | open in the phone's browser |
| key QR → `#devkey=` link (`auth.js`) | an in-app QR scanner feeding the same code; paste keeps working |
| Android back gesture vs `history.pushState` (`settingspages.js`) | the app's back-button event follows the page history, exits only from the top |
| Leaflet, markercluster, icon font from CDNs | bundled into the APK so the app opens offline (map tiles still need signal) |

Unchanged and expected to work in the web view: geolocation (with the app's location permission), IndexedDB
(log cache, keys), localStorage, `crypto.subtle` (HMAC login, fleet key wrap), file pickers, the screen wake
lock, clipboard. Each is still on the test list below.

## Things that change for you

- **Scanner keys go into the app once per scanner.** The web app stores each key in a form it cannot read back
  out (on purpose), so they cannot be copied across. In the app: CYDs → Settings → Phone key, scan the QR with
  the app's own scanner. Minis → paste the key from the serial `key` command (the existing paste box).
  The Bluetooth pairing itself is NOT repeated: Android keeps it for the phone, not for Chrome.
- **Live upload / server reads may need one server change.** The app's pages come from `https://localhost`, not
  from github.io, so the scan-analyzer server has to allow that origin (CORS). I'll ask its session; it is a
  one-line config there. (The fallback, routing requests through native code, would lose the 20 s upload
  timeout handling, so I'd rather not.)
- **The web app stays.** Use either while stage 1 settles. Whether Pages freezes later is a separate decision.

## Steps

Each step ends with something you can check on the phone.

**1. Release pipeline with the current web app (no Bluetooth yet).**
- Create the Capacitor project, app name COINTEL-FLO, icon from `resources/logo/`.
- Make the signing key (`keytool` from Android Studio), store it as GitHub secrets, keep it OUT of the repo
  (`.gitignore` `*.jks`, `*.keystore`, `local.properties`). You keep a backup copy and its password: losing it
  means the next build can't update the installed app (uninstall = the app's data is lost).
- GitHub Actions: on every push to `main` that touches `webapp/` or `android-app/` (plus a manual button),
  build, sign, and publish a release tagged `app-<n>` with `cointel-flo-<n>.apk`.
- Obtainium: add the repo, filter on `app-` tags.
- **Check:** the app installs, opens the Home page, and a second push arrives in Obtainium as an update.

**2. Bluetooth: connect, log in, live data.**
- Native BLE plugin, the shim, the in-app scanner list, permissions (Nearby devices, location).
- Ask for the largest MTU on connect (the firmware sends MTU − 3 per notify, capped at 500, so this may speed up
  downloads beyond the browser's 252 bytes).
- Key entry: in-app QR scanner + the existing paste.
- **Check:** connect to TOLUBA and a mini, log in with the key, encrypted link, live list + STATUS + time / GPS
  writes, commands (start / stop, brightness, lock), Switch scanner between two units with no flags.

**3. Data: transfers, files, uploads.**
- Downloads (full, tail top-up, all-logs export, `.cflo`), the log cache, SD file explorer uploads, saving to
  Downloads / share sheet, Map from current session / file, uploads + live upload to scan-analyzer (after the
  CORS change), Server data / history.
- **Check:** download a 1 MB+ log and the 24 h export, compare speed with the browser, upload a scan, map it.

**4. The rest of the app.**
- Notifications (rules, test notification, a real alert), Wi-Fi download guide (opens in the browser), fleet
  settings (enroll needs the key wrap), back gesture on Settings subpages, offline start.
- **Check:** your normal use for a few days.

**5. Wrap-up.** README section on installing with Obtainium, CLAUDE.md + HANDOFF, review pass
(**reviewer** + **security**: the shim, permissions, where keys live, what the APK contains).

## Testing loop

- **Day to day:** Android Studio → Wireless debugging (pair once, then no cable) → Run. The web side is
  debugged from the PC's Chrome at `chrome://inspect` (debug builds only), with the same console the web app has.
- **Your builds:** Obtainium updates from GitHub releases.
- Release builds turn web debugging off.

## Risks

- **The BLE plugin's limits.** Download speed depends on how fast notifications cross from native to the
  web view (~125 per second at the current rate). Measured in step 3; if it is too slow, the LOGDATA stream
  is the first piece to move native.
- **The scanner list.** Android hides a bonded device's real address behind its private one only while it
  can resolve it; if that doesn't work through the plugin, remembered units fall back to "CFLO" + signal strength.
- **Version drift.** The app bundles one copy of the web app. The firmware already lets old web apps read new
  STATUS keys, so this is the same situation as a phone with a cached page.
- **Google's sideloading rules** (developer verification, US date not set yet): see the discussion in HANDOFF;
  the free hobbyist account or the one-time "advanced flow" setting cover it. Re-check before 2027.

## Owner decisions (all made 2026-11-21)

- App name **COINTEL-FLO**; shell + bundled web app; release via GitHub + Obtainium; stage 2 after stage 1.
- Package ID **`io.github.negativeaffirmations.cointelflo`** (permanent). Development builds use
  `….cointelflo.dev` and show as "COINTEL-FLO dev", so they install beside the released app.
- A build is published automatically on every push to `main` that touches `webapp/`, `android-app/` or the
  workflow (`.github/workflows/android.yml`), plus a manual "Run workflow" button. Release tag `app-0.1.<n>` (Obtainium reads the version from it; build 1 was tagged `app-1`),
  asset `cointel-flo-<n>.apk`, version `0.1.<n>`, versionCode = the workflow run number (never reset it).
- Signing key uploaded to the repo secrets with `gh` (`CFLO_KEYSTORE_B64`, `CFLO_KEYSTORE_PASSWORD`,
  `CFLO_KEY_ALIAS`). The key lives OUTSIDE the repo in the owner's home folder `cointel-flo-signing\`
  (keystore + a README with the password) — **back that folder up**. Certificate: `CN=COINTEL-FLO`, SHA-256
  `f759eb7af7bfc89eac414986386eb77b6bca96917bb7ecec969eafd811658f31`.
- scan-analyzer asked (bridge, 2026-11-21) to allow `https://localhost`: done in its code, live after the owner
  OKs its push + a server restart. LIVE 2026-11-21 (scan-analyzer `ab415fc`, preflight checked by the owner).

## Progress

- **Step 1 DONE (2026-11-21):** builds 2 and 3 published; the owner installed build 2 with Obtainium and
  received build 3 (new icon) as an update. Icons come from `android-app/scripts/make-icons.py`.
- **Step 2 BUILT, not yet phone-tested (2026-11-22):**
  - `native/shim-bluetooth.js`: `navigator.bluetooth` + `BluetoothDevice` over `@capacitor-community/bluetooth-le`
    8.3.0 (raw `Capacitor.Plugins.BluetoothLe`, values as hex). Every GATT operation runs one at a time (the plugin
    doesn't queue). Our own chooser dialog (scan filtered on the service UUID; known units by name from
    `cflo.unitNames`, others "CFLO (new)"); `getDevices()` = units this app connected to (`cflo.app.devices`);
    `watchAdvertisements` shares one LE scan. A local `disconnect()` fires `gattserverdisconnected` only after the
    plugin confirms, so the plugin's late notice can't drop a quick reconnect. Mock test: `npm test` (16 checks, also
    in CI before every build).
  - `native/shim-platform.js`: "Scan QR" buttons (Google code scanner via `@capacitor-mlkit/barcode-scanning` 8.2.2)
    beside the Scanner keys / key-needed / Remote server boxes; a key QR goes through the existing paste + button, a
    server QR's `#server=` fragment through the existing hash handler. `window.open` of an outside page -> the phone's
    browser (`@capacitor/browser`).
  - `native/app.css`: app bar padded by the status-bar inset; SystemBars style `DARK` (light icons).
  - Manifest: `allowBackup=false` + data-extraction rules excluding everything (keys + cached logs never go to Google
    backup or a phone transfer).
  - Plugins for later steps are already in (`filesystem`, `share`, `local-notifications`, `app`).
- **Steps 3-4 (part) BUILT, not yet phone-tested (2026-11-22):**
  - Downloads: `shim-platform.js` remembers each blob behind `URL.createObjectURL`, catches `<a download>` clicks
    (`util/dom.js` saveBlob) and streams the blob in base64 chunks (768 KB) to the app's own plugin
    `CfloFilesPlugin.java` (registered in `MainActivity`), which writes `Download/COINTEL-FLO/<name>` through
    MediaStore (no storage permission; minSdk raised 24 -> 29 = Android 10). Saves run one after another; the toast
    says where the file went.
  - Notifications: `window.Notification` + `navigator.serviceWorker` stand-ins backed by
    `@capacitor/local-notifications` (a tag maps to a stable id, so a newer alert replaces the older like the web);
    a tap posts the same `cflo-notify-click` message `notify-sw.js` sends, so `notify.js` opens the device as before.
    Status-bar icon `res/drawable/ic_stat_cflo.xml` (radar mark), tint `#AA00AA`.
  - Mock tests: `npm test`, run in CI before each build.
- **Review pass (2026-11-22, reviewer + security agents), all fixed in build 6:**
  - REV-001 a presence watch aborted while its scan was still starting left a low-latency scan running all session
    (abort now hooked before the await); REV-002 scan stop debounced 10 s (Android ignores > 5 starts / 30 s) + a
    restart every 25 min (Android throttles > 30 min); REV-003 chooser rows stay in place (no re-sort under the finger);
    REV-004 `disconnect()` during a connect rejects it at once (switch "not found" in 8 s, not 15); REV-005 a GATT op
    refused at once (stack still busy after a timed-out op) is retried once after 400 ms.
  - SEC-A1 alerts go to channel `cflo-alerts` with lock-screen visibility SECRET (bystander MACs / names stay off the
    lock screen; sound + vibration remain) — **owner may prefer them visible; it's one setting**. SEC-A2 a server QR
    that would replace a paired server asks first, naming the new host; that button only takes server links.
  - Unused `SCHEDULE_EXACT_ALARM` / `RECEIVE_BOOT_COMPLETED` removed from the manifest; workflow actions pinned to
    commit SHAs, Gradle wrapper validated, keystore removed by `trap` even on a failed build.
  - Back gesture: closes an open dialog, else page history back, at the top the app goes to the background (keeps the
    scanner link) instead of closing. Scanning the same server QR twice works.
- **Offline start (build 6):** `build-www.mjs` downloads Leaflet 1.9.4, markercluster 1.5.3 (SHA-256 pinned; a changed
  file fails the build) and the two Google font sheets + woff2 files (~600 KB) into `www/vendor/` and rewrites
  index.html; the build fails if any CDN URL is left. Map tiles still need signal.
- Mock tests: 32 checks (Bluetooth 21, platform 11).
  - The debug APK is ~28 MB, mostly the ML Kit / camera libraries the QR plugin brings. Fine for now; revisit if size matters.

## Known issues

- **App bar under the phone's status bar (owner, build 3).** The app draws edge to edge (Android 15+ with
  targetSdk 36), so the web app's top app bar sits on the same line as the phone's clock / battery row.
  Fix in step 2: pad the page by the system-bar insets (Capacitor 8's system-bars inset handling or
  `env(safe-area-inset-*)` with `viewport-fit=cover`), app build only, so the Pages site is unchanged.
  Check the bottom navbar against the gesture bar at the same time.

## Build notes

- **Java 21, not Android Studio's bundled Java 25.** Gradle 8.14.3 (Capacitor 8.6.0) fails on Java 25 with
  "Unsupported class file major version 69". A Temurin 21 lives in `%USERPROFILE%\.jdks\jdk-21.0.13.1+2`; in
  Android Studio set Settings → Build → Gradle → Gradle JDK to it. Command line: `JAVA_HOME` = that folder,
  `ANDROID_HOME` = `%LOCALAPPDATA%\Android\Sdk`.
- Capacitor pinned at **8.6.0** (8.6.1 / 8.6.2 were under two weeks old on 2026-11-21).
- `npm run sync` (in `android-app/`) = copy `webapp/` into `www/` + `cap sync android`. Then
  `android/gradlew assembleDebug` (dev) or let CI build the release.
- Debug builds allow `chrome://inspect`; release builds don't (Capacitor's default).
