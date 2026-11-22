// COINTEL-FLO app only (docs/android-app.md): stand-ins for browser features Android's web view lacks.
//   - "Scan QR" buttons beside the key / server paste boxes (the phone camera can't hand a QR link to the app)
//   - window.open of an outside page (Wi-Fi download page, links) -> the phone's browser
// Classic script, injected by scripts/build-www.mjs before the web app's module; does nothing outside the app.
(function(){
  "use strict";
  const C = window.Capacitor;
  if(!C || !C.Plugins || !C.isNativePlatform || !C.isNativePlatform()) return;
  const P = C.Plugins;

  // ---------- QR scanning (Google code scanner via ML Kit: no camera permission, Google draws the UI) ----------
  async function scanQr(){
    const S = P.BarcodeScanner;
    if(!S) throw new Error("QR scanner missing from this build");
    try{
      const a = await S.isGoogleBarcodeScannerModuleAvailable();
      if(a && a.available === false){
        await S.installGoogleBarcodeScannerModule();
        throw new Error("The QR scanner is being installed by Google Play services. Try again in a moment.");
      }
    }catch(e){ if(/installed by Google/.test(e.message)) throw e; }
    const r = await S.scan({ formats:["QR_CODE"] });
    const b = r && r.barcodes && r.barcodes[0];
    return b ? (b.rawValue || b.displayValue || "") : "";
  }

  // A key or server link scanned from a QR: its #fragment goes to the web app's own hash handlers
  // (handleDevKeyHash / handlePairHash run on hashchange). Returns true if it was such a link.
  function routeLink(text){
    const i = text.indexOf("#");
    const frag = i >= 0 ? text.slice(i + 1) : "";
    if(/(^|&)(devkey|server)=/.test(frag)){ location.hash = frag; return true; }
    return false;
  }

  function addScanButton(afterId, onText){
    const anchor = document.getElementById(afterId);
    if(!anchor || document.getElementById(afterId + "Qr")) return;
    const b = document.createElement("button");
    b.id = afterId + "Qr";
    b.type = "button";
    b.className = "secondary appqr";
    b.style.marginTop = ".4em";
    b.innerHTML = '<span class="material-symbols-outlined" style="vertical-align:-5px;font-size:20px">qr_code_scanner</span> Scan QR';
    b.addEventListener("click", async () => {
      let text = "";
      try{ text = (await scanQr()).trim(); }
      catch(e){ if(!/cancel/i.test(e && e.message || "")) alertMsg("QR scan failed: " + (e && e.message || e)); return; }
      if(text) onText(text);
    });
    anchor.insertAdjacentElement("afterend", b);
  }
  function alertMsg(msg){
    const t = document.getElementById("toast");
    if(t){ t.textContent = msg; t.className = "toast show err"; t.hidden = false; setTimeout(() => { t.classList.remove("show"); t.hidden = true; }, 4500); }
    else console.warn(msg);
  }
  const fillAndClick = (inputId, btnId) => text => {
    const inp = document.getElementById(inputId), btn = document.getElementById(btnId);
    if(!inp || !btn) return;
    inp.value = text;
    inp.dispatchEvent(new Event("input", { bubbles:true }));
    btn.click();
  };

  function wireQr(){
    // Settings -> Scanner keys, and the "Scanner key needed" dialog: the paste box + its own button do the rest.
    addScanButton("keyAddBtn", fillAndClick("keyPaste", "keyAddBtn"));
    addScanButton("authSaveBtn", fillAndClick("authPaste", "authSaveBtn"));
    // Settings -> Remote Server: the scan-analyzer pairing QR is a link carrying #server=…&token=….
    addScanButton("rsPairBtn", text => { if(!routeLink(text)) alertMsg("That QR is not a server pairing link."); });
  }

  // ---------- outside pages open in the phone's browser ----------
  const origOpen = window.open.bind(window);
  window.open = function(url, target, features){
    try{
      const u = new URL(String(url), location.href);
      if((u.protocol === "http:" || u.protocol === "https:") && u.host !== location.host && P.Browser){
        P.Browser.open({ url:u.href }).catch(() => {});
        return null;
      }
    }catch(e){}
    return origOpen(url, target, features);
  };

  if(document.readyState === "loading") document.addEventListener("DOMContentLoaded", wireQr);
  else wireQr();
  window.__cfloApp = Object.assign(window.__cfloApp || {}, { platform:true });
})();
