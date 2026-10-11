// Bottom-navbar page switching. All four are real page sections (<section class="page">, shown via
// the .visible class — a class, not the [hidden] attribute, so a CSS `display` rule can't override
// it). The Map nav button calls openMap() (which calls showPage('map') AND sizes Leaflet); the
// others call showPage() directly. Wiring lives in main.js (addEventListener convention).
import { $ } from './util/dom.js';

const PAGES = ['home', 'scanner', 'map', 'data', 'settings'];   // real page sections, id="page-<name>"

export function showPage(p){
  if (!PAGES.includes(p)) return;
  for (const name of PAGES){
    const sec = $('page-' + name);
    if (sec) sec.classList.toggle('visible', name === p);
  }
  document.querySelectorAll('.navbtn').forEach(b =>
    b.classList.toggle('active', b.dataset.page === p));
  window.scrollTo(0, 0);
}
