"use strict";

// Session picker: the session list opens in a window with a calendar. Days that have a session are
// marked; a day lists its sessions, and with no day chosen every session is listed, newest first.
// The <select id="session"> stays in the page, hidden, as the one place the chosen session lives.

const PK_COPY = {
  en: {
    title: "Choose a session",
    close: "Close",
    prev: "Previous month",
    next: "Next month",
    all: "All sessions",
    today: "Today",
    none: "No sessions yet.",
    emptyDay: "No sessions on this day.",
    onDay: (day) => "Sessions on " + day,
    everything: "All sessions, newest first",
    strokes: (n) => n + (n === 1 ? " stroke" : " strokes"),
    count: (n) => n + (n === 1 ? " session" : " sessions"),
    undated: "No date",
    demo: "demo",
  },
  nl: {
    title: "Kies een sessie",
    close: "Sluiten",
    prev: "Vorige maand",
    next: "Volgende maand",
    all: "Alle sessies",
    today: "Vandaag",
    none: "Nog geen sessies.",
    emptyDay: "Geen sessies op deze dag.",
    onDay: (day) => "Sessies op " + day,
    everything: "Alle sessies, nieuwste eerst",
    strokes: (n) => n + (n === 1 ? " slag" : " slagen"),
    count: (n) => n + (n === 1 ? " sessie" : " sessies"),
    undated: "Geen datum",
    demo: "demo",
  },
};

const pk = { month: null, day: null };

function pkCopy() { return PK_COPY[lang()]; }
function pkLocale() { return lang() === "nl" ? "nl-NL" : "en-GB"; }

function pkKey(date) {
  return date.getFullYear() + "-" + String(date.getMonth() + 1).padStart(2, "0") + "-" + String(date.getDate()).padStart(2, "0");
}

// A session's moment as the browser's date, or null when the server could not say.
function pkWhen(session) {
  if (!session.started) return null;
  const when = new Date(session.started);
  return Number.isNaN(when.getTime()) ? null : when;
}

function pkEntries() {
  return state.sessions.map((s) => ({ s, when: pkWhen(s) }));
}

function pkDays() {
  const days = new Map();
  for (const e of pkEntries()) {
    if (!e.when) continue;
    const key = pkKey(e.when);
    days.set(key, (days.get(key) || 0) + 1);
  }
  return days;
}

function pkCurrent() {
  return state.sessions.find((s) => s.id === $("session").value) || null;
}

// The button in the toolbar shows the chosen session.
function pickerSync() {
  const sel = $("session");
  const btn = $("session-btn");
  const current = sel.options[sel.selectedIndex];
  $("session-name").textContent = state.sessions.length && current ? current.textContent.replace(/\s+/g, " ").trim() : "—";
  btn.disabled = !state.sessions.length;
}

function pkDayLabel(key) {
  const [y, m, d] = key.split("-").map(Number);
  return new Date(y, m - 1, d).toLocaleDateString(pkLocale(), { weekday: "long", day: "numeric", month: "long", year: "numeric" });
}

function pkRenderCalendar() {
  const copy = pkCopy();
  const first = pk.month;
  $("pk-month").textContent = first.toLocaleDateString(pkLocale(), { month: "long", year: "numeric" });
  $("pk-prev").title = copy.prev;
  $("pk-next").title = copy.next;
  const days = pkDays();
  const current = pkCurrent();
  const currentWhen = current ? pkWhen(current) : null;
  const currentKey = currentWhen ? pkKey(currentWhen) : null;
  const todayKey = pkKey(new Date());
  const cells = [];
  for (let i = 0; i < 7; i++) {
    const name = new Date(2024, 0, 1 + i).toLocaleDateString(pkLocale(), { weekday: "short" });
    cells.push("<span class=\"pk-wd\">" + name + "</span>");
  }
  const lead = (first.getDay() + 6) % 7; // Monday first
  for (let i = 0; i < lead; i++) cells.push("<span></span>");
  const last = new Date(first.getFullYear(), first.getMonth() + 1, 0).getDate();
  for (let day = 1; day <= last; day++) {
    const key = pkKey(new Date(first.getFullYear(), first.getMonth(), day));
    const n = days.get(key) || 0;
    const classes = ["pk-day"];
    if (n) classes.push("has");
    if (key === pk.day) classes.push("sel");
    if (key === currentKey) classes.push("cur");
    if (key === todayKey) classes.push("today");
    cells.push("<button type=\"button\" class=\"" + classes.join(" ") + "\" data-day=\"" + key + "\""
      + (n ? " title=\"" + copy.count(n) + "\"" : "") + (key === pk.day ? " aria-pressed=\"true\"" : "") + ">" + day
      + (n ? "<i></i>" : "") + "</button>");
  }
  $("pk-grid").innerHTML = cells.join("");
  for (const button of $("pk-grid").querySelectorAll("button[data-day]")) {
    button.addEventListener("click", () => {
      pk.day = pk.day === button.dataset.day ? null : button.dataset.day;
      pkRender();
    });
  }
  $("pk-all").textContent = copy.all;
  $("pk-all").disabled = pk.day === null;
  $("pk-today").textContent = copy.today;
}

function pkRenderList() {
  const copy = pkCopy();
  let rows = pkEntries();
  if (pk.day) rows = rows.filter((e) => e.when && pkKey(e.when) === pk.day);
  rows.sort((a, b) => (b.when ? b.when.getTime() : -Infinity) - (a.when ? a.when.getTime() : -Infinity));
  $("pk-listhead").textContent = pk.day ? copy.onDay(pkDayLabel(pk.day)) : copy.everything;
  const box = $("pk-items");
  box.innerHTML = "";
  if (!rows.length) {
    const p = document.createElement("p");
    p.className = "pk-empty";
    p.textContent = state.sessions.length ? copy.emptyDay : copy.none;
    box.appendChild(p);
    return;
  }
  const chosen = $("session").value;
  for (const { s, when } of rows) {
    const item = document.createElement("button");
    item.type = "button";
    item.className = "pk-item" + (s.id === chosen ? " on" : "");
    if (s.id === chosen) item.setAttribute("aria-current", "true");
    const name = document.createElement("b");
    name.textContent = s.name;
    const meta = document.createElement("small");
    const time = when
      ? (pk.day
        ? when.toLocaleTimeString(pkLocale(), { hour: "2-digit", minute: "2-digit" })
        : when.toLocaleString(pkLocale(), { dateStyle: "medium", timeStyle: "short" }))
      : copy.undated;
    meta.textContent = time + " · " + copy.strokes(s.strokes);
    item.append(name, meta);
    item.addEventListener("click", () => pkChoose(s.id));
    box.appendChild(item);
  }
}

function pkRender() {
  pkRenderCalendar();
  pkRenderList();
}

function pkChoose(id) {
  const sel = $("session");
  $("picker").close();
  if (sel.value === id) return;
  sel.value = id;
  sel.dispatchEvent(new Event("change"));
  pickerSync();
}

function pkOpen() {
  if (!state.sessions.length) return;
  const copy = pkCopy();
  $("pk-title").textContent = copy.title;
  $("pk-close").textContent = copy.close;
  const current = pkCurrent();
  const when = current ? pkWhen(current) : null;
  if (when) {
    pk.day = pkKey(when);
    pk.month = new Date(when.getFullYear(), when.getMonth(), 1);
  } else {
    pk.day = null;
    const dated = pkEntries().filter((e) => e.when).sort((a, b) => b.when - a.when);
    const base = dated.length ? dated[0].when : new Date();
    pk.month = new Date(base.getFullYear(), base.getMonth(), 1);
  }
  pkRender();
  $("picker").showModal();
}

$("session-btn").addEventListener("click", pkOpen);
$("pk-close").addEventListener("click", () => $("picker").close());
$("picker").addEventListener("click", (e) => { if (e.target === $("picker")) $("picker").close(); });
$("pk-prev").addEventListener("click", () => {
  pk.month = new Date(pk.month.getFullYear(), pk.month.getMonth() - 1, 1);
  pkRenderCalendar();
});
$("pk-next").addEventListener("click", () => {
  pk.month = new Date(pk.month.getFullYear(), pk.month.getMonth() + 1, 1);
  pkRenderCalendar();
});
$("pk-today").addEventListener("click", () => {
  const now = new Date();
  pk.month = new Date(now.getFullYear(), now.getMonth(), 1);
  pkRenderCalendar();
});
$("pk-all").addEventListener("click", () => {
  pk.day = null;
  pkRender();
});
$("lang").addEventListener("change", () => {
  pickerSync();
  if ($("picker").open) {
    $("pk-title").textContent = pkCopy().title;
    $("pk-close").textContent = pkCopy().close;
    pkRender();
  }
});
pickerSync();
