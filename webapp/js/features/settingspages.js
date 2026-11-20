// Settings subpages: the Settings page is a menu (#setMenu) of four subpages (#set-devices / -notify / -fleet /
// -remote); opening one shows its cards under a back arrow (#setBack) and the subpage name in the title.
// Android's back gesture closes an open subpage instead of leaving the app (which would drop the BLE link): opening
// one pushes a history entry, and popstate goes back to the menu. The bottom-nav Settings button always lands on
// the menu. The cards inside are unchanged, so every other module keeps finding its elements by id.
// NEW file (the stale-module rule): only main.js imports it.
import { $ } from '../util/dom.js';

const SUBS = { devices:"Device(s)", notify:"Notifications", fleet:"Fleet", remote:"Remote Server" };
let current = "";      // open subpage key, "" = the menu
let pushed = false;    // a history entry is ours to pop

function render(){
  for (const k of Object.keys(SUBS)) { const el = $("set-" + k); if (el) el.hidden = k !== current; }
  $("setMenu").hidden = !!current;
  $("setBack").hidden = !current;
  $("setTitle").textContent = current ? SUBS[current] : "Settings";
}
export function openSettingsSub(key){
  if (!SUBS[key]) return;
  current = key; render(); window.scrollTo(0, 0);
  if (!pushed) { try { history.pushState({ cfloSettings:key }, ""); pushed = true; } catch (_) {} }
}
export function showSettingsMenu(){
  if (pushed) { pushed = false; try { history.back(); } catch (_) {} }   // popstate lands on render() again: harmless
  current = ""; render(); window.scrollTo(0, 0);
}

export function initSettingsPages(){
  if (!$("setMenu")) return;
  $("setMenu").addEventListener("click", e => { const r = e.target.closest(".setrow"); if (r) openSettingsSub(r.dataset.sub); });
  $("setMenu").addEventListener("keydown", e => {
    const r = e.target.closest(".setrow");
    if (r && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); openSettingsSub(r.dataset.sub); }
  });
  $("setBack").onclick = showSettingsMenu;
  window.addEventListener("popstate", () => { if (pushed || current) { pushed = false; current = ""; render(); } });
  // Bottom nav: Settings -> the menu; any other page closes an open subpage (and its history entry) on the way out.
  document.querySelectorAll(".navbtn").forEach(b => b.addEventListener("click", () => {
    if (current || pushed) showSettingsMenu();
  }));
  render();
}
