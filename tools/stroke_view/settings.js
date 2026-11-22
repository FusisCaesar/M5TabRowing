"use strict";

// Settings window, opened with the cogwheel. It has tabs; Disk is the first. A tab is a button in
// #st-tabs with a data-tab and a panel in the window with the same data-tab. A tab that needs to
// draw itself when it opens adds a case to stShow().

Object.assign(CHROME.en, { settings: "Settings", settingsClose: "Close" });
Object.assign(CHROME.nl, { settings: "Instellingen", settingsClose: "Sluiten" });
applyChrome();

const st = { tab: "disk" };

function stShow(tab) {
  st.tab = tab;
  const tabs = [...document.querySelectorAll("#st-tabs button")];
  tabs.forEach((btn, i) => {
    btn.setAttribute("aria-selected", btn.dataset.tab === tab);
    btn.classList.toggle("first", i === 0);
    btn.classList.toggle("last", i === tabs.length - 1);
  });
  for (const panel of document.querySelectorAll("#settings .st-panel")) panel.hidden = panel.dataset.tab !== tab;
  if (tab === "disk") diskShow();
}

function stOpen() {
  $("settings").showModal();
  stShow(st.tab);
}

$("settings-btn").addEventListener("click", stOpen);
$("st-close").addEventListener("click", () => $("settings").close());
$("settings").addEventListener("click", (e) => { if (e.target === $("settings")) $("settings").close(); });
for (const btn of document.querySelectorAll("#st-tabs button")) {
  btn.addEventListener("click", () => stShow(btn.dataset.tab));
}
