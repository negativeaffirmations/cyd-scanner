# COINTEL-FLO Android app — stage 1 plan

Status: **PLAN, not started** (2026-11-21). Owner decisions are marked **[DECIDE]**.

## Where this fits

The app is built in two stages (owner-approved 2026-11-21):

1. **Stage 1 (this plan): the web app as an Android app.** The existing web app (`webapp/`) runs inside a
   native shell (Capacitor), bundled into the APK. Bluetooth moves from Web Bluetooth (which Android's web
   view does not have) to a native BLE plugin. Builds are published on GitHub and installed / updated on the
   phone with Obtainium. When stage 1 is done the app does everything the web app does, no browser flags needed.
2. **Stage 2: background (DONE, build 10).** A foreground service, then a screen-off drive test. If the web code keeps
   running, stop there; if not, move the BLE link, GPS / time push, live upload and notifications into native code.
   Result: the web code keeps running (with `CfloWebView`, see "Stage 2"), so nothing moved into native code.

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
- **The web app stays** (owner, 2026-11-22: Pages is kept, not frozen; both stay working).

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
    lock screen; sound + vibration remain) — **owner confirmed 2026-11-22: keep details off the lock screen**. Superseded
    by Alert style (below): lock screen is now a setting, still off by default. SEC-A2 a server QR
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

## Alert style (2026-11-22, after the owner's stage-1 test)
Owner request: show / hide notification details (Android's notification history keeps what a notification said, so
hiding details keeps bystanders' identifiers out of it; **details shown by default**), and custom sounds + vibration
patterns, as one style for all alerts plus overrides per alert type.
- **Web app:** `webapp/js/features/notifystyle.js` (new file; notify.js only gains internals) renders "Alert style" into
  `#nfStyle` in Settings -> Notifications: All alerts = details / lock screen / sound / vibration; each type (Threats,
  Watched devices, Possible followers, Drones, Deauth floods, Evil-twin APs) overrides only the parts set, with its own
  "Send a test". Vibration = presets, or Custom (typed ms "on, off, on...", max 24 steps / 15 s) with a hold-to-vibrate
  recorder. Saved in `cflo.notify` (`style`, `kinds`); the old Vibrate checkbox migrates (off -> None). Hidden details =
  a plain title ("Threat alert") + "Open COINTEL-FLO to see it."; a folded notification hides details if ANY of its
  types does, and takes the style of its top type (watch > follow > rid > threat). In a browser only details and
  vibration apply; sound / lock screen rows show only in the app (`window.__cfloApp.notifyStyle`).
- **App:** Android fixes a channel's sound / vibration / lock-screen visibility at creation, so `shim-platform.js` maps
  each distinct style to channel `cflo-<kind>-<hash>`; `CfloNotifyPlugin.java` creates it (lock screen PRIVATE when on =
  the phone's own lock-screen setting decides, else SECRET; IMPORTANCE_HIGH) and deletes that kind's older channels and
  build 6's `cflo-alerts`. Sounds come from the phone's notification-sound picker (`RingtoneManager`, Default / Silent /
  the phone's sounds / ones the owner added), vibration preview + recorder through `Vibrator` (`VIBRATE` permission
  added). Changing a style replaces that type's channel, which also drops edits made to it in Android's settings.
- Review (2026-11-22) fixed: no JS channel cache (a style A -> B -> A reused a deleted channel and Android dropped every
  alert silently); a channel still showing a notification is not deleted until later (deleting it clears the shade);
  renotify removes the shown notification first (the plugin's alert-only-once made repeat tests silent); every Android call
  guarded (a plugin throw crashes the app); typed `getParcelableExtra` only on API 34+; recorder / pattern-box edge cases.
  **To verify on the phone:** a sound the owner ADDED to the phone (content://media/external/...) may play as the phone
  default, since the app holds no read grant for it; the built-in sounds are fine.

## Stage 2: background (DONE 2026-11-22: walk 2 passed on build 9, build 10 checked)
Owner bug (build 6): left in the background or with the screen off, alerts only arrived on returning to the app. Android
froze the app process, and with it the web view. Nothing was lost on the scanner.
- **Foreground service** `CfloBgService.java` (types `connectedDevice`, + `location` when the app holds a location
  permission; partial wake lock, non-counted, 12 h cap renewed every hour while it runs; `stopWithTask`). It runs while a scanner is
  connected: one low-importance "Connected to TOLUBA" notification (tap = open, **Disconnect** button = the page
  disconnects every scanner the normal way). Started / stopped by `native/shim-background.js` from link events that
  `shim-bluetooth.js` now emits (`__cfloApp.bleLink`); a stop waits 10 s so Switch scanner (up to 8 s) doesn't flap it. A link lost
  while the app is in the background leaves a dismissible "Lost the link to X" note (own id, `LOST_ID`). Android refuses
  a service start from the background, so the notification text is only repainted while the app is visible, and a
  `startForeground` refused with `location` retries without it.
- **Why the page keeps running:** Capacitor never calls `WebView.onPause()` / `pauseTimers()`, so with the process
  kept alive the page's JavaScript runs on; BLE notifications reach it as plugin events. Two things still stop in a
  hidden page and have stand-ins:
  - **GPS:** Chromium stops geolocation while the page is hidden. `shim-background.js` replaces `navigator.geolocation`
    (watchPosition / clearWatch / getCurrentPosition) with `CfloBgPlugin`'s LocationManager feed (GPS 1 s, network
    2 s used only while GPS has been quiet 5 s; fix time = now − the fix's age, so a skewed phone clock can't make
    it look stale to gps.js's 10 s check). Permission denied → error code 1, location off → 2, timeout → 3.
  - **Timers:** a hidden page's timers are throttled (to once a minute after ~5 min). The plugin sends a 1 s `tick`
    while the activity is stopped and the service runs; the shim turns it into a window `cflo-bg-tick` event and
    `notify.js` runs its alert check on it (tick() is safe to run twice).
- **Battery:** Settings → Notifications gets a "Run in the background" card (app only): Android's "let the app run
  in the background" prompt (`REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`, falls back to the app-info page). A toast points
  at it on the first connection of an app start while not allowed. Samsung also lists the app under Battery →
  "Unrestricted" once allowed.
- notify.js's status line in the app says "also with the screen off".
- Mock test `test/shim-background.test.mjs` (20 checks: service follows the link, switch doesn't stop it, lost note,
  tick only while hidden, Disconnect button, geolocation fixes / maximumAge / timeout / denied, battery card).
- **Owner test (screen-off drive):** update in Obtainium; connect (Android may ask for notifications + location
  again); Settings → Notifications → Allow background running; check the "Connected to …" notification; lock the
  phone and walk / drive 10+ min; alerts should arrive with the screen off, the scanner's STATUS should keep a
  phone GPS source (blue) the whole time, and live upload rows should have no gap. Then the Disconnect button in the
  notification, and walking out of range (the "Lost the link" note).
- **If the web code still stops in the background** (the plan's fallback): move the BLE link, GPS / time push, live
  upload and alert checks into native code.
- **Review (2026-11-22, reviewer agent), fixed before the first phone build:** REV-001 a location grant after the start
  never added the service's location type (the repaint key now includes it); REV-002 a service left by a destroyed /
  recreated page (the plugin's `handleOnDestroy` stops it, and every page start stops a leftover); REV-003 a stop that
  lands before `onStartCommand` crashes the app (`startPending`: the plugin waits for it; `CHANGE_NETWORK_STATE`
  declared so the connectedDevice type can't be refused for a missing grant); REV-004 the page follows a service that
  ended on its own (status every 5 s while visible, restarted while still connected); REV-005 a one-shot started from
  a failed watch's error callback got a 30 s timeout instead of the real error; minor: no-default getCurrentPosition
  timeout, hourly wake-lock renewal, callback owner check, tick only beside the service, a cached GPS fix no longer
  mutes network fixes, coarse-only grant keeps network fixes, the "link lost" note on its own default-importance
  channel `cflo-link-lost`, 10 s stop delay, tip toast hides only its own text, notify.js's line checks the service.
  Confirmed by reading Capacitor 8.6.0: `Bridge.onPause` pauses the web view only when the `KeepRunning` preference is
  false (default true, not set here), so JavaScript keeps running in the background. A renderer crash kills the app
  (Capacitor returns false from onRenderProcessGone); keep the web view's default renderer priority.
- **Owner walk 1 (build 8, 2026-11-22, fleet of 3):** alerts kept coming for ~1-2 min after the screen went off or the
  app went to the background, then stopped. Cause not known yet; candidates: the page's JavaScript freezes, the whole
  process is frozen (Samsung), the scanner link stalls, or simply nothing new to alert on (a device alerts once per
  quiet time).
- **Background log (build 9):** `CfloDiag.java` keeps a private text log (2 × 256 KB) the owner reads in Settings →
  Notifications → Background log (Save → Downloads). The service writes a heartbeat line every 30 s: ticks sent / answered
  by the page (+ seconds since the last answer), GPS fixes, wake lock, foreground types, the page's cumulative counters
  (BLE notifications, DETS packets, alerts shown / failed, links, visible) and the phone state (screen, Doze, light Doze,
  battery saver, exemption, background-restricted). Plus app foreground / background switches, service start / stop,
  scanner connect / disconnect / lost, alert failures. Counts and states only: no MACs, names or positions. Reading it: a
  gap between heartbeats = the process was frozen; `ticks+` without `acks+` = the page's JavaScript stopped; `dets`
  not growing = the scanner link stalled; `dets` growing but `shown` not = nothing new to alert on (or check `failed`).
- **Owner report on walk 1:** service allowed + notifications on; on reopening the app a BURST of alerts arrived at once.
  So the data and rules were fine and the page's JavaScript was paused in the background (its queued plugin events
  ran on return). Chromium pauses a web view whose window is not visible (and lowers its renderer's priority), even with
  the app process kept alive by the service.
- **Fix (build 9): `CfloWebView.java`** (a `CapacitorWebView` subclass, used through the app's own
  `res/layout/capacitor_bridge_layout_main.xml`, which overrides Capacitor's layout of the same name; id `webview`
  kept). While the service runs, `onWindowVisibilityChanged` passes VISIBLE to Chromium, so the page keeps running; when
  the service stops (`CfloBgService.onStopped`), the real visibility is handed over. Because the page now stays
  `visibilityState === "visible"` in the background, the shims take the app state from the plugin's new `appState`
  event (`{fg}`, also `status().fg`): shim-background's `visible()` uses it (ticks, the lost note, the repaint), and
  shim-bluetooth pauses the Switch-scanner presence scan while the app is in the background (resumed for the same
  watchers on return). Web app code that checks `document.visibilityState` now sees "visible" in the background while
  a scanner is connected (wake lock attempt, log-cache reload, Switch scanner): all harmless. Tests 59/59.
- adb on this PC: port 5037 sits in a Windows reserved range (Hyper-V); use `ANDROID_ADB_SERVER_PORT=15037`.
- **Owner walk 2 (build 9, 2026-11-22): PASSED.** Alerts arrived live with the screen off / app in the background. The
  saved log covered 21 min in the background: ticks 28-30 per 30 s all answered by the page, 30 GPS fixes per 30 s,
  shown 2 -> 41, failed 0, light doze from 2 min in without effect (exempt=1). Two bugs found, fixed in build 10:
  `ble`/`dets` read 0 throughout (shim-platform.js replaced the shared `__cfloApp.diag` object after shim-bluetooth.js
  took a reference; both now fill in the one object), and Save looked like it did nothing (the file was written, but
  the toast draws behind the modal log dialog; the dialog now shows "Saving..." then the result, through
  `__cfloApp.saveFile`, which returns a promise).
- **Build 10 check (2026-11-22, owner, short at home):** Save reported its result and the file arrived; counters now
  real (ble=372 dets=312 over ~30 s), 8/8 background ticks answered, 30 fixes per 30 s. A longer screen-off walk on
  build 10 would give a full log with the counters; not required.
- **Reading the log:** a heartbeat gap = the process was frozen; `ticks+` without `acks+` = the page's JS paused;
  `dets` flat while `links=1` = the scanner link stalled; `dets` growing with `shown` flat = nothing new to alert on.
  `vis` and `page{}` are the page's last answer (sent only in the background), so a heartbeat just after returning
  to the app can still say `vis=0`.
- **UI gotcha:** the shims' toast draws BEHIND an open modal `<dialog>` (top layer). Anything triggered from inside a
  dialog must show its result in the dialog.
