// Bottom-navbar page switching. Home + Scanner are real page sections (<section class="page">,
// shown via the .visible class — a class, not the [hidden] attribute, so a CSS `display` rule can't
// override it). Map + Settings are action items that open their existing dialogs for now; they
// become full inline pages in a later phase. Wiring lives in main.js (addEventListener convention).
import { $ } from './util/dom.js';

const PAGES = ['home', 'scanner'];   // real page sections, id="page-<name>"

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
