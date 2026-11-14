"use strict";

const COPY = {
  en: {
    phases: ["Catch", "Drive", "Finish", "Run", "Approach"],
    together: "Together",
    thisStroke: "This stroke",
    average: (n) => n + "-stroke average",
    rateAverage: (n) => "average of " + n + " strokes",
    againstBest: "against the best strokes at this rate",
    fewAtRate: "too few strokes to set the best apart",
    rateBest: (n, m, when) => "best " + n + " strokes average " + m.toFixed(1) + " m, the furthest at " + when,
    rateFurthest: (m, when) => "furthest stroke " + m.toFixed(1) + " m at " + when,
    rateSlow: (spm, slow, here) => spm + " spm averages " + slow.toFixed(1) + " m, this rate " + here.toFixed(1) + " m",
    rateEmpty: "Not enough strokes share a rate to average them.",
    paceTag: "hold",
    paceHold: (spm, split, held, total) => "Best pace to hold is " + spm + " spm at " + split + ". Longest run " + held + ", " + total + " in the outing.",
    paceBurst: (spm, split, held) => " " + spm + " spm is quicker (" + split + ") but only lasts " + held + ".",
    paceShort: " This outing never held one rate for a full minute.",
    spells: (h, l) => "Rowed in spells, not at one pace: " + h.n + " hard spells of about " + clock(h.median_s) + " (" + h.rate.toFixed(1) + " spm, " + splitText(h.split) + ") and " + l.n + " light ones of about " + clock(l.median_s) + " (" + l.rate.toFixed(1) + " spm, " + splitText(l.split) + "). The rates differ because the effort did, so none is picked as the pace to hold.",
    rateToggle: "Show this rate",
    rateHelp: "Tick the rates you want to compare. The chart draws only those, and the average of the best strokes at the selected rate in green. Double-click a rate to open its furthest stroke.",
    rateLegend: ["this rate", "best strokes", "slower", "faster"],
    bestWord: "best",
    vsBest: "against your best in this session",
    vsUsual: "against your usual in this session",
    learning: "not enough strokes at this rate yet",
    rescued: "Filled in from the rhythm",
    unread: "The phases could not be read on this stroke.",
    help: "Wheel zooms, drag pans, double-click shows the whole stroke. Arrows move between strokes.",
    sessionHelp: "Wheel zooms the outing, drag pans, double-click a stroke to open it. Arrows move between strokes.",
    sessionLegend: ["surge", "gps", "tip", "selected"],
    legend: ["now", "reference", "measured surge", "gps", "catch", "finish", "mark"],
    chips: {
      width: "catch width",
      notches: "notches",
      loss: "speed loss",
      peak: "drive peak",
      gain: "drive gain",
      sag: "sag",
      fall: "finish",
      release: "release",
      run: "run",
      approach: "approach",
      speed: "speed peak",
    },
    basis: {
      width: "Catch width is how long the dip stays below half its depth. The tip fires when that is 25% and 20 ms wider than the reference.",
      notches: "A notch is a dip of its own inside the catch: below 40% of the depth, with the curve coming back up by 15% of the depth (at least 0.02 g) before the next one counts. A clean catch counts as 1. The tip fires at 0.8 more than the reference.",
      loss: "Catch loss is the boat speed given up from the catch until the boat picks up again. What is lost before the catch counts as approach. The tip fires when that is 25% and 0.04 m/s more than the reference.",
      peak: "The drive peak is the highest acceleration during the drive, placed as a fraction of the drive. The tip fires when it sits more than 15% of the drive away from the reference.",
      gain: "Drive gain is the boat speed added between the end of the catch and the finish. The tip fires when that is under 90% of the reference.",
      sag: "Sag is a valley between two humps in the drive, each above 45% of the peak, when the valley is more than 0.03 g deep.",
      fall: "The finish is the time the drive falls from 80% of its peak to 20%. The tip fires when that takes 30% and 25 ms longer than the reference.",
      release: "The release dip is how far acceleration falls below the run in the first 0.35 s after the finish. The tip fires when it is 0.025 g deeper than the reference.",
      run: "Run is how unevenly the boat glides between the finish and the approach: the scatter of the acceleration around a straight line, in milli-g. Lower is a steadier run. No tip is given on it.",
      approach: "Approach lead is how long the boat has already been slowing before the catch. The tip fires when that starts more than 0.08 s earlier than the reference.",
      speed: "Speed peak is when the boat, read from the acceleration, is fastest after the catch.",
    },
    caps: {
      catch: (d, loss) => d.toFixed(2) + " g deep, loses " + loss.toFixed(2) + " m/s",
      drive: (pct, sag) => "peak " + pct + "%, sag " + sag.toFixed(2) + " g",
      finish: (g) => "release dip " + g.toFixed(2) + " g",
      run: (s) => "speed peaks at " + s.toFixed(1) + " s",
      approach: (v) => "loses " + v.toFixed(2) + " m/s before the blade is in",
      notch: "1 notch in the catch",
      notches: (n) => Math.round(n) + " notches in the catch",
    },
  },
  nl: {
    phases: ["Inpik", "Haal", "Uitpik", "Uitloop", "Aanloop"],
    together: "Samen",
    thisStroke: "Deze slag",
    average: (n) => "gemiddelde " + n + " slagen",
    rateAverage: (n) => "gemiddelde van " + n + " slagen",
    againstBest: "tegen de beste slagen op dit tempo",
    fewAtRate: "te weinig slagen om de beste apart te zetten",
    rateBest: (n, m, when) => "beste " + n + " slagen gemiddeld " + m.toFixed(1) + " m, de verste om " + when,
    rateFurthest: (m, when) => "verste slag " + m.toFixed(1) + " m om " + when,
    rateSlow: (spm, slow, here) => spm + " slagen/min gemiddeld " + slow.toFixed(1) + " m, dit tempo " + here.toFixed(1) + " m",
    rateEmpty: "Te weinig slagen delen een tempo om te middelen.",
    paceTag: "vast",
    paceHold: (spm, split, held, total) => "Beste tempo om vast te houden is " + spm + " slagen/min op " + split + ". Langste stuk " + held + ", " + total + " in de sessie.",
    paceBurst: (spm, split, held) => " " + spm + " slagen/min is sneller (" + split + ") maar duurt maar " + held + ".",
    paceShort: " Deze sessie hield geen tempo een volle minuut vast.",
    spells: (h, l) => "Geroeid in blokken, niet op één tempo: " + h.n + " harde blokken van ongeveer " + clock(h.median_s) + " (" + h.rate.toFixed(1) + " slagen/min, " + splitText(h.split) + ") en " + l.n + " lichte van ongeveer " + clock(l.median_s) + " (" + l.rate.toFixed(1) + " slagen/min, " + splitText(l.split) + "). De tempo's verschillen omdat de inzet verschilde, dus er is geen tempo gekozen om vast te houden.",
    rateToggle: "Toon dit tempo",
    rateHelp: "Vink de tempo's aan die je wilt vergelijken. De grafiek tekent alleen die, en het gemiddelde van de beste slagen op het gekozen tempo in groen. Dubbelklik een tempo om de verste slag te openen.",
    rateLegend: ["dit tempo", "beste slagen", "langzamer", "sneller"],
    bestWord: "beste",
    vsBest: "tegen je beste in deze sessie",
    vsUsual: "tegen je normale in deze sessie",
    learning: "nog te weinig slagen op dit tempo",
    rescued: "Ingevuld vanuit het ritme",
    unread: "De fasen zijn op deze slag niet te lezen.",
    help: "Wiel zoomt, slepen verschuift, dubbelklik toont de hele slag. Pijltjes wisselen van slag.",
    sessionHelp: "Wiel zoomt de sessie, slepen verschuift, dubbelklik op een slag opent hem. Pijltjes wisselen van slag.",
    sessionLegend: ["surge", "gps", "tip", "gekozen"],
    legend: ["nu", "referentie", "gemeten surge", "gps", "inpik", "uitpik", "markering"],
    chips: {
      width: "inpikbreedte",
      notches: "knikken",
      loss: "snelheidsverlies",
      peak: "haalpiek",
      gain: "haalwinst",
      sag: "dal",
      fall: "uitpik",
      release: "loslaten",
      run: "uitloop",
      approach: "aanloop",
      speed: "snelheidspiek",
    },
    basis: {
      width: "De inpikbreedte is hoelang de dip onder de helft van zijn diepte blijft. De tip komt als dat 25% en 20 ms breder is dan de referentie.",
      notches: "Een knik is een eigen dip in de inpik: onder 40% van de diepte, waarbij de curve eerst 15% van de diepte (minstens 0,02 g) terugkomt voor de volgende telt. Een schone inpik telt als 1. De tip komt bij 0,8 meer dan de referentie.",
      loss: "Het inpikverlies is de bootsnelheid die verloren gaat vanaf de inpik tot de boot weer versnelt. Wat voor de inpik verloren gaat telt bij de aanloop. De tip komt bij 25% en 0,04 m/s meer dan de referentie.",
      peak: "De haalpiek is de hoogste versnelling in de haal, als deel van de haal. De tip komt als die meer dan 15% van de haal van de referentie af zit.",
      gain: "De haalwinst is de bootsnelheid die er tussen het eind van de inpik en de uitpik bij komt. De tip komt onder 90% van de referentie.",
      sag: "Het dal zit tussen twee bulten in de haal, elk boven 45% van de piek, als het dal dieper is dan 0,03 g.",
      fall: "De uitpik is de tijd waarin de haal van 80% van de piek naar 20% zakt. De tip komt als dat 30% en 25 ms langer duurt dan de referentie.",
      release: "De uitpikdip is hoever de versnelling onder de uitloop zakt in de eerste 0,35 s na de uitpik. De tip komt bij 0,025 g dieper dan de referentie.",
      run: "De uitloop is hoe ongelijkmatig de boot glijdt tussen de uitpik en de aanloop: de spreiding van de versnelling rond een rechte lijn, in milli-g. Lager is een rustiger uitloop. Hier hoort geen tip bij.",
      approach: "De aanloop is hoelang de boot al vertraagt voor de inpik. De tip komt als dat meer dan 0,08 s eerder begint dan de referentie.",
      speed: "De snelheidspiek is wanneer de boot, gelezen uit de versnelling, het hardst gaat na de inpik.",
    },
    caps: {
      catch: (d, loss) => d.toFixed(2) + " g diep, verliest " + loss.toFixed(2) + " m/s",
      drive: (pct, sag) => "piek " + pct + "%, dal " + sag.toFixed(2) + " g",
      finish: (g) => "uitpikdip " + g.toFixed(2) + " g",
      run: (s) => "snelheid piekt na " + s.toFixed(1) + " s",
      approach: (v) => "verliest " + v.toFixed(2) + " m/s voor de inpik",
      notch: "1 knik in de inpik",
      notches: (n) => Math.round(n) + " knikken in de inpik",
    },
  },
};

const PHASE = ["#f6d5cc", "#d7e4f5", "#d4efef", "#ece7dc", "#f8e7c4"];
// Chart surfaces: the card, the plot panels inside it, their grid and edge.
const CARD = "#ffffff";
const PANEL = "#fcfbf8";
const EDGE = "#e7e1d5";
const GRID = "#eee9de";
const ZERO = "#c9c1b4";
const MUTED = "#766f64";
const FONT = "Segoe UI, system-ui, sans-serif";
const DRIVE_BINS = 32;
const REC_BINS = 64;

const $ = (id) => document.getElementById(id);
const state = {
  token: 0,
  sessions: [],
  id: null,
  rows: [],
  i: 0,
  stroke: null,
  focus: null,
  zoomed: false,
  preroll: 0.4,
  t0: -0.4,
  t1: 2,
  s0: null,
  s1: null,
  span0: null,
  span1: null,
  trace: null,
  traceToken: 0,
  rates: null,
  ri: 0,
  rateOff: new Set(),
  filter: { kind: "all", rate: null },
  blurb: "",
};

function lang() { return $("lang").value === "nl" ? "nl" : "en"; }
function copy() { return COPY[lang()]; }
function crew() { return parseInt($("crew").value, 10) || 1; }
function viewMode() { return $("view").value; }
function sessionMode() { return $("chartmode").value === "session"; }
function rateMode() { return $("chartmode").value === "rate"; }

function clock(s) {
  if (s == null || !isFinite(s)) return "—";
  const sign = s < 0 ? "-" : "";
  s = Math.abs(s);
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return sign + m + ":" + String(sec).padStart(2, "0");
}

function splitText(s) {
  if (!s || s <= 0) return "—";
  let sec = Math.round(s);
  const m = Math.floor(sec / 60);
  sec %= 60;
  return m + ":" + String(sec).padStart(2, "0");
}

function setStatus(text) { $("status").textContent = text || ""; }

// The page around the chart: labels, buttons and the welcome text.
const CHROME = {
  en: {
    tagline: "Stroke analysis",
    build: "Build your own",
    language: "Language",
    session: "Session",
    crew: "Crew",
    chart: "Chart",
    view: "View",
    oneStroke: "One stroke",
    wholeSession: "Whole session",
    byRate: "By rate",
    thisStroke: "This stroke",
    lastFive: "Last 5 at this rate",
    openFiles: "Open files",
    openFilesTip: "Choose the _imu.csv of a session, with its _seconds.csv, _laps.csv and _info.csv for the map and heart rate",
    fromTab5: "Read from Tab5",
    fromTab5Tip: "Read a recording from the tablet over USB. Chrome or Edge.",
    welcomeTitle: "Read your rowing, stroke by stroke",
    welcomeLead: "Load a recording from an M5Tab Rowing monitor. Every stroke is found again and shown as drive, finish, run and approach, with the same hint the device gives on the water.",
    stepStop: "Stop the session on the tablet.",
    stepRead: "Plug it into this computer with USB-C and press <b>Read from Tab5</b>, or open the recording files you copied off the card (select the <code>_imu.csv</code> together with the <code>_seconds.csv</code>, <code>_laps.csv</code> and <code>_info.csv</code> of that session).",
    stepRead2: "Pick the session, step through the strokes, and switch the chart to the whole session or by rate.",
    privacy: "Recordings are analysed on this server and are not kept: they are gone when it restarts. Reading from the tablet needs Chrome or Edge.",
    buildTitle: "Do not have the device yet?",
    buildLead: "It is an open project. You need an M5Stack Tab5, a GNSS unit and a microSD card. The firmware installs from your browser, no tools needed.",
    buildStep1: "Get the parts: <b>M5Stack Tab5</b>, <b>M5Stack Unit GNSS</b> on Port A, and a FAT32 <b>microSD card</b>.",
    buildStep2: "Open the install page in Chrome or Edge, plug in the Tab5 and press install.",
    buildStep3: "Mount it rigidly in the boat, row, and come back here with the recording.",
    installPage: "Open the install page",
    sourceDocs: "Source and full guide on GitHub",
    filterAll: "All",
    filterTips: "Tips",
    filterBest: "Best",
    filterFilled: "Filled in",
    filterRate: "Rate",
    filterNone: "No strokes match this filter.",
  },
  nl: {
    tagline: "Slaganalyse",
    build: "Zelf bouwen",
    language: "Taal",
    session: "Sessie",
    crew: "Bemanning",
    chart: "Grafiek",
    view: "Weergave",
    oneStroke: "Eén slag",
    wholeSession: "Hele sessie",
    byRate: "Per slagfrequentie",
    thisStroke: "Deze slag",
    lastFive: "Laatste 5 op dit tempo",
    openFiles: "Bestanden openen",
    openFilesTip: "Kies de _imu.csv van een sessie, met de _seconds.csv, _laps.csv en _info.csv voor de kaart en hartslag",
    fromTab5: "Lees van Tab5",
    fromTab5Tip: "Lees een opname via USB van de tablet. Chrome of Edge.",
    welcomeTitle: "Bekijk je roeien, slag voor slag",
    welcomeLead: "Laad een opname van een M5Tab Rowing-monitor. Elke slag wordt opnieuw gevonden en getoond als haal, finish, doorglijden en aanloop, met dezelfde tip als het apparaat op het water geeft.",
    stepStop: "Stop de sessie op de tablet.",
    stepRead: "Sluit hem met USB-C aan op deze computer en kies <b>Lees van Tab5</b>, of open de opnamebestanden die je van de kaart hebt gekopieerd (selecteer de <code>_imu.csv</code> samen met de <code>_seconds.csv</code>, <code>_laps.csv</code> en <code>_info.csv</code> van die sessie).",
    stepRead2: "Kies de sessie, loop door de slagen, en zet de grafiek op de hele sessie of per slagfrequentie.",
    privacy: "Opnames worden op deze server geanalyseerd en niet bewaard: ze zijn weg als de server herstart. Lezen van de tablet kan alleen in Chrome of Edge.",
    buildTitle: "Nog geen apparaat?",
    buildLead: "Het is een open project. Je hebt een M5Stack Tab5, een GNSS-unit en een microSD-kaart nodig. De firmware installeer je vanuit de browser, zonder extra gereedschap.",
    buildStep1: "Haal de onderdelen: <b>M5Stack Tab5</b>, <b>M5Stack Unit GNSS</b> op Port A en een FAT32 <b>microSD-kaart</b>.",
    buildStep2: "Open de installatiepagina in Chrome of Edge, sluit de Tab5 aan en kies installeren.",
    buildStep3: "Monteer hem stevig in de boot, roei, en kom terug met de opname.",
    installPage: "Open de installatiepagina",
    sourceDocs: "Broncode en volledige gids op GitHub",
    filterAll: "Alle",
    filterTips: "Tips",
    filterBest: "Beste",
    filterFilled: "Ingevuld",
    filterRate: "Tempo",
    filterNone: "Geen slagen voor dit filter.",
  },
};

function applyChrome() {
  const words = CHROME[lang()];
  document.documentElement.lang = lang();
  for (const el of document.querySelectorAll("[data-i18n]")) {
    const text = words[el.dataset.i18n];
    if (text != null) el.innerHTML = text;
  }
  for (const el of document.querySelectorAll("[data-i18n-title]")) {
    const text = words[el.dataset.i18nTitle];
    if (text != null) el.title = text;
  }
}

function startLanguage() {
  let saved = null;
  try { saved = localStorage.getItem("stroke-lang"); } catch (err) { /* storage blocked */ }
  const guess = saved || ((navigator.language || "en").toLowerCase().startsWith("nl") ? "nl" : "en");
  $("lang").value = guess === "nl" ? "nl" : "en";
  applyChrome();
}

function sessionBlurb(data, n) {
  let text = data.name + " · " + n + " strokes";
  if (data.rescued) text += ", " + data.rescued + " filled in";
  if (data.logged && data.detected && data.logged !== data.detected) {
    text += " (file has " + data.logged + ")";
  }
  const listed = state.sessions.find((s) => s.id === data.id);
  if (listed && listed.demo) {
    text += lang() === "nl"
      ? " · Voorbeeldopname. Lees van Tab5 of open bestanden voor je eigen sessie."
      : " · Demo recording. Read from Tab5 or open files for your own session.";
  }
  return text;
}

async function getJSON(url, options) {
  const res = await fetch(url, options);
  const text = await res.text();
  let data = null;
  try {
    data = JSON.parse(text);
  } catch (err) {
    // Not our JSON: the server was restarting or ran out of memory and the host sent a web page.
    throw new Error(res.ok
      ? "The server sent an unexpected answer. Try again."
      : "The server is not answering (" + res.status + "). A very long recording can be too much for it, try again in a minute.");
  }
  if (!res.ok) throw new Error(data.error || res.statusText);
  return data;
}

function fullSpan(d) {
  if (rateMode() && state.rates && state.rates.length) {
    let hi = 0.8;
    for (const row of visibleRates()) {
      if (row.bounds && row.bounds.interval) hi = Math.max(hi, row.bounds.interval);
    }
    const best = d && rateShown(d.spm) && d.best && d.best.curve;
    if (best && best.t && best.t.length) hi = Math.max(hi, best.t[best.t.length - 1]);
    return [-state.preroll, hi];
  }
  const interval = d && d.bounds ? d.bounds.interval : (d && d.drive + d.rec) || 2;
  return [-state.preroll, Math.max(0.8, interval)];
}

function resetZoom() {
  if (!state.stroke) return;
  const span = fullSpan(state.stroke);
  state.t0 = span[0];
  state.t1 = span[1];
  state.zoomed = false;
}

function clampView() {
  const span = fullSpan(state.stroke);
  let t0 = state.t0;
  let t1 = state.t1;
  const minSpan = Math.min(0.08, span[1] - span[0]);
  if (t1 - t0 < minSpan) {
    const mid = (t0 + t1) / 2;
    t0 = mid - minSpan / 2;
    t1 = mid + minSpan / 2;
  }
  if (t1 - t0 > span[1] - span[0]) {
    t0 = span[0];
    t1 = span[1];
  }
  if (t0 < span[0]) { t1 += span[0] - t0; t0 = span[0]; }
  if (t1 > span[1]) { t0 -= t1 - span[1]; t1 = span[1]; }
  state.t0 = Math.max(span[0], t0);
  state.t1 = Math.min(span[1], t1);
}

async function loadSessions(selectId) {
  state.sessions = await getJSON("/api/sessions");
  const sel = $("session");
  sel.innerHTML = "";
  for (const s of state.sessions) {
    const opt = document.createElement("option");
    opt.value = s.id;
    opt.textContent = s.name + "  (" + s.strokes + ")";
    sel.appendChild(opt);
  }
  $("welcome").hidden = state.sessions.length > 0;
  document.body.classList.toggle("empty", state.sessions.length === 0);
  if (!state.sessions.length) {
    $("app").hidden = true;
    setStatus("");
    return;
  }
  const want = selectId && state.sessions.some((s) => s.id === selectId) ? selectId : state.sessions[0].id;
  sel.value = want;
  await loadSession(want);
}

async function loadSession(id) {
  const mine = ++state.token;
  setStatus("Reading " + id + "…");
  $("app").hidden = true;
  $("overview").hidden = true;
  const data = await getJSON("/api/session?id=" + encodeURIComponent(id) + "&crew=" + crew() + "&lang=" + lang() + "&view=" + viewMode());
  if (mine !== state.token) return;
  state.id = data.id;
  if (data.preroll_s) state.preroll = data.preroll_s;
  state.rateOff = new Set();
  state.filter = { kind: "all", rate: null };
  state.rates = null;
  state.rows = data.strokes;
  state.s0 = state.s1 = state.span0 = state.span1 = null;
  state.trace = null;
  const first = state.rows.findIndex((r) => r.valid);
  state.i = first >= 0 ? first : 0;
  renderList();
  state.blurb = sessionBlurb(data, state.rows.length);
  setStatus(state.blurb);
  if (window.overviewLoad) await overviewLoad(data.id);
  if (mine !== state.token) return;
  $("app").hidden = false;
  if (sessionMode()) await enterSession();
  if (rateMode()) await loadRates();
  else if (state.rows.length) await loadStroke(state.i);
  else setStatus(data.name + " has no strokes.");
}

async function reloadHints() {
  if (!state.id) return;
  const mine = ++state.token;
  const keep = state.i;
  const data = await getJSON("/api/session?id=" + encodeURIComponent(state.id) + "&crew=" + crew() + "&lang=" + lang() + "&view=" + viewMode());
  if (mine !== state.token) return;
  state.rows = data.strokes;
  state.i = Math.min(keep, state.rows.length - 1);
  if (rateMode()) {
    await loadRates();
    return;
  }
  renderList();
  if (state.rows.length) await loadStroke(state.i);
  if (sessionMode()) drawSession();
}

async function loadStroke(i) {
  const mine = ++state.token;
  state.i = i;
  renderList();
  const data = await getJSON("/api/stroke?id=" + encodeURIComponent(state.id) +
    "&i=" + i + "&crew=" + crew() + "&view=" + viewMode() + "&lang=" + lang());
  if (mine !== state.token) return;
  state.stroke = data;
  state.focus = data.focus;
  resetZoom();
  renderHint();
  renderChips();
  renderGauges();
  if (sessionMode()) drawSession();
  else {
    resetZoom();
    draw();
  }
  const btn = $("list").querySelector(".stroke.on");
  if (btn) btn.scrollIntoView({ block: "nearest" });
}

function rateShown(spm) {
  return !(state.rateOff && state.rateOff.has(spm));
}

function visibleRates() {
  const rows = (state.rates || []).filter((row) => rateShown(row.spm));
  return rows.length ? rows : (state.rates || []);
}

function toggleRate(spm, on) {
  if (!state.rateOff) state.rateOff = new Set();
  if (on) {
    state.rateOff.delete(spm);
  } else {
    const left = state.rates.filter((row) => row.spm !== spm && rateShown(row.spm));
    if (!left.length) {
      renderList();
      return;
    }
    state.rateOff.add(spm);
    if (state.rates[state.ri] && state.rates[state.ri].spm === spm) {
      selectRate(state.rates.indexOf(left[0]));
      return;
    }
  }
  if (!state.zoomed) resetZoom();
  else clampView();
  renderList();
  renderHint();
  draw();
}

function rateHue(spm) {
  const rates = state.rates || [];
  const lo = rates.length ? rates[0].spm : spm;
  const hi = rates.length ? rates[rates.length - 1].spm : spm;
  const u = hi === lo ? 0.5 : (spm - lo) / (hi - lo);
  return [
    Math.round(14 + u * (154 - 14)),
    Math.round(124 + u * (52 - 124)),
    Math.round(134 + u * (18 - 134)),
  ];
}

function rateInk(spm, alpha) {
  const [r, g, b] = rateHue(spm);
  return alpha == null ? "rgb(" + r + "," + g + "," + b + ")" : "rgba(" + r + "," + g + "," + b + "," + alpha + ")";
}

async function loadRates() {
  if (!state.id) return;
  const mine = ++state.token;
  const keep = state.rates && state.rates[state.ri] ? state.rates[state.ri].spm : null;
  const data = await getJSON("/api/rates?id=" + encodeURIComponent(state.id) +
    "&crew=" + crew() + "&lang=" + lang());
  if (mine !== state.token) return;
  state.rates = data.rates || [];
  state.pace = data.pace || null;
  state.spells = data.spells || null;
  if (!state.rates.length) {
    state.stroke = null;
    renderList();
    renderHint();
    renderChips();
    renderGauges();
    draw();
    setStatus(copy().rateEmpty);
    return;
  }
  let idx = keep == null ? -1 : state.rates.findIndex((row) => row.spm === keep);
  if (idx < 0 && keep == null && state.pace) idx = state.rates.findIndex((row) => row.spm === state.pace.spm);
  if (idx < 0) {
    const want = state.rows[state.i] && state.rows[state.i].rate ? Math.round(state.rows[state.i].rate) : state.rates[0].spm;
    let nearest = 0;
    let dist = Infinity;
    state.rates.forEach((row, i) => {
      const d = Math.abs(row.spm - want);
      if (d < dist) { dist = d; nearest = i; }
    });
    idx = nearest;
  }
  selectRate(idx);
  const left = (data.readable || 0) - (data.shown || 0);
  let status = state.rates.length + " rates, " + (data.shown || 0) + " strokes";
  if (left > 0) status += " (" + left + " at a rate with fewer than 3)";
  setStatus(status);
}

function selectRate(i) {
  state.ri = Math.max(0, Math.min((state.rates || []).length - 1, i));
  const row = state.rates[state.ri];
  if (row && state.rateOff) state.rateOff.delete(row.spm);
  state.stroke = row || null;
  state.focus = row ? row.focus : null;
  resetZoom();
  renderList();
  renderHint();
  renderChips();
  renderGauges();
  draw();
  const btn = $("list").querySelector(".stroke.on");
  if (btn) btn.scrollIntoView({ block: "nearest" });
}

function paceSentence(p) {
  if (!p) return "";
  const c = copy();
  let text = c.paceHold(p.spm, splitText(p.split), clock(p.hold_s), clock(p.total_s));
  if (p.burst) text += c.paceBurst(p.burst.spm, splitText(p.burst.split), clock(p.burst.hold_s));
  if (!p.settled) text += c.paceShort;
  return text;
}

function renderRateList() {
  const list = $("list");
  list.innerHTML = "";
  const slow = state.rates[0];
  const word = copy().bestWord;
  state.rates.forEach((row, i) => {
    const shorter = slow && row.spm > slow.spm && slow.dps && row.dps && row.dps < slow.dps * 0.97;
    const shown = rateShown(row.spm);
    const wrap = document.createElement("div");
    wrap.className = "rate" + (shown ? "" : " off");
    const tick = document.createElement("label");
    tick.className = "tick";
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = shown;
    box.title = copy().rateToggle;
    box.style.accentColor = rateInk(row.spm);
    box.addEventListener("click", (e) => e.stopPropagation());
    box.addEventListener("change", () => toggleRate(row.spm, box.checked));
    tick.append(box);
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "stroke" + (i === state.ri ? " on" : "") + (shorter ? " warn" : "");
    const top = document.createElement("div");
    top.className = "top";
    const left = document.createElement("span");
    left.className = "who";
    const spm = document.createElement("b");
    spm.textContent = row.spm + " spm";
    const count = document.createElement("span");
    count.textContent = row.n + (lang() === "nl" ? " slagen" : " strokes");
    left.append(spm, count);
    const right = document.createElement("b");
    right.className = "split";
    right.textContent = splitText(row.split);
    top.append(left, right);
    const hint = document.createElement("div");
    hint.className = "hint";
    const best = row.best && row.best.dps ? "   ·   " + word + " " + row.best.dps.toFixed(1) + " m" : "";
    hint.textContent = (row.dps ? row.dps.toFixed(1) + " m" : "—") + best;
    btn.append(top, hint);
    if (row.pace) {
      const tags = document.createElement("div");
      tags.className = "tags";
      const tag = document.createElement("span");
      tag.className = "tag pace";
      tag.textContent = copy().paceTag;
      tags.append(tag);
      btn.append(tags);
    }
    btn.addEventListener("click", () => selectRate(i));
    btn.addEventListener("dblclick", () => { if (row.best) openStroke(row.best.i); });
    wrap.append(tick, btn);
    list.append(wrap);
  });
}

function rowMatches(row) {
  const f = state.filter;
  if (f.rate != null && Math.round(row.rate || 0) !== f.rate) return false;
  switch (f.kind) {
    case "tip": return !!row.warn;
    case "best": return !!row.best;
    case "filled": return !!row.rescued;
    default: return true;
  }
}

// Badges above the stroke list: what kind of stroke, and at which rate.
function renderFilters(list) {
  const words = CHROME[lang()];
  const rows = state.rows;
  const kinds = [
    ["all", words.filterAll, rows.length],
    ["tip", words.filterTips, rows.filter((r) => r.warn).length],
    ["best", words.filterBest, rows.filter((r) => r.best).length],
    ["filled", words.filterFilled, rows.filter((r) => r.rescued).length],
  ];
  const perRate = new Map();
  for (const row of rows) {
    if (!row.rate) continue;
    const spm = Math.round(row.rate);
    perRate.set(spm, (perRate.get(spm) || 0) + 1);
  }
  const rates = [...perRate.entries()].filter(([, n]) => n >= 3).sort((a, b) => a[0] - b[0]);

  const bar = document.createElement("div");
  bar.className = "filters";
  const badge = (label, count, on, onClick, extra) => {
    const el = document.createElement("button");
    el.type = "button";
    el.className = "badge" + (on ? " on" : "") + (extra ? " " + extra : "");
    el.setAttribute("aria-pressed", on ? "true" : "false");
    el.append(document.createTextNode(label));
    if (count != null) {
      const n = document.createElement("span");
      n.textContent = count;
      el.append(n);
    }
    el.addEventListener("click", onClick);
    return el;
  };
  const kindRow = document.createElement("div");
  kindRow.className = "badges";
  for (const [kind, label, count] of kinds) {
    if (kind !== "all" && !count) continue;
    kindRow.append(badge(label, count, state.filter.kind === kind, () => {
      state.filter.kind = kind;
      renderList();
    }));
  }
  bar.append(kindRow);
  if (rates.length > 1) {
    const rateRow = document.createElement("div");
    rateRow.className = "badges";
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = words.filterRate;
    rateRow.append(name);
    for (const [spm, count] of rates) {
      const el = badge(String(spm), null, state.filter.rate === spm, () => {
        state.filter.rate = state.filter.rate === spm ? null : spm;
        renderList();
      }, "rate-badge");
      el.title = count + " " + (lang() === "nl" ? "slagen" : "strokes");
      rateRow.append(el);
    }
    bar.append(rateRow);
  }
  list.append(bar);
}

function renderList() {
  if (rateMode() && state.rates) { renderRateList(); return; }
  const list = $("list");
  list.innerHTML = "";
  if (state.rows.length) renderFilters(list);
  let shown = 0;
  state.rows.forEach((row, i) => {
    if (!rowMatches(row)) return;
    shown++;
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "stroke" + (i === state.i ? " on" : "") + (row.warn ? " warn" : "");
    const top = document.createElement("div");
    top.className = "top";
    const left = document.createElement("span");
    left.className = "who";
    const time = document.createElement("b");
    time.textContent = clock(row.elapsed);
    const spm = document.createElement("span");
    spm.textContent = (row.rate ? row.rate.toFixed(1) : "—") + " spm";
    left.append(time, spm);
    const right = document.createElement("b");
    right.className = "split";
    right.textContent = splitText(row.split);
    top.append(left, right);
    const hint = document.createElement("div");
    hint.className = "hint";
    hint.textContent = row.hint;
    btn.append(top, hint);
    if (row.rescued || row.best) {
      const tags = document.createElement("div");
      tags.className = "tags";
      if (row.rescued) {
        const tag = document.createElement("span");
        tag.className = "tag rescued";
        tag.textContent = copy().rescued;
        tags.append(tag);
      }
      if (row.best) {
        const tag = document.createElement("span");
        tag.className = "tag";
        tag.textContent = lang() === "nl" ? "beste" : "best";
        tags.append(tag);
      }
      btn.append(tags);
    }
    btn.addEventListener("click", () => {
      if (sessionMode()) ensureVisible(i);
      loadStroke(i);
    });
    list.append(btn);
  });
  if (state.rows.length && !shown) {
    const none = document.createElement("div");
    none.className = "none";
    none.textContent = CHROME[lang()].filterNone;
    list.append(none);
  }
  const on = list.querySelector(".stroke.on");
  if (on) on.scrollIntoView({ block: "nearest" });
}

function renderHint() {
  const d = state.stroke;
  const box = $("hint");
  const c = copy();
  box.className = d && d.warn ? "warn" : "";
  if (!d) { box.innerHTML = ""; return; }
  const ref = d.view === "rate" ? (d.have_ref ? c.againstBest : c.fewAtRate) : !d.have_ref ? c.learning : d.vs_best ? c.vsBest : c.vsUsual;
  const view = d.view === "rate" ? c.rateAverage(d.samples) : d.view === "average" ? c.average(d.samples) : c.thisStroke;
  const basis = d.valid && state.focus ? c.basis[state.focus] : (d.valid ? "" : c.unread);
  const rescued = d.view === "rate" ? "" : (d.rescued ? " · " + c.rescued : "");
  box.innerHTML = "";
  if (d.view === "rate" && (state.spells || state.pace)) {
    const found = document.createElement("div");
    found.className = "text pace-line";
    found.textContent = state.spells ? c.spells(state.spells.hard, state.spells.light) : paceSentence(state.pace);
    box.append(found);
  }
  const text = document.createElement("div");
  text.className = "text";
  text.textContent = d.hint;
  const meta = document.createElement("div");
  meta.className = "meta";
  meta.textContent = view + " · " + ref + rescued;
  box.append(text, meta);
  if (d.view === "rate" && state.rates && state.rates.length) {
    const slow = visibleRates()[0];
    const line = document.createElement("div");
    line.className = "basis";
    let extra = !d.best || !d.best.dps ? "" : d.best.n ? c.rateBest(d.best.n, d.best.dps, clock(d.best.elapsed)) : c.rateFurthest(d.best.dps, clock(d.best.elapsed));
    if (slow && slow.spm !== d.spm && slow.dps && d.dps) {
      extra += (extra ? " · " : "") + c.rateSlow(slow.spm, slow.dps, d.dps);
    }
    line.textContent = extra;
    if (extra) box.append(line);
  }
  if (basis) {
    const line = document.createElement("div");
    line.className = "basis";
    line.textContent = basis;
    box.append(line);
  }
}

function chipValue(id, d) {
  const n = d.now;
  if (!n) return "";
  switch (id) {
    case "width": return Math.round(n.width * 1000) + " ms";
    case "notches": return String(Math.round(n.notches));
    case "loss": return n.loss.toFixed(2) + " m/s";
    case "peak": return Math.round(n.peak_pct * 100) + "%";
    case "gain": return "+" + n.gain.toFixed(2) + " m/s";
    case "sag": return n.sag.toFixed(2) + " g";
    case "fall": return Math.round(n.fall * 1000) + " ms";
    case "release": return n.release.toFixed(2) + " g";
    case "run": return Math.round(n.run_mg) + " mg";
    case "approach": return n.lead.toFixed(2) + " s";
    case "speed": return n.speed_peak.toFixed(1) + " s";
    default: return "";
  }
}

function renderChips() {
  const box = $("chips");
  box.innerHTML = "";
  const d = state.stroke;
  const names = copy().chips;
  if (!d || !d.valid) return;
  for (const id of Object.keys(names)) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "chip" + (state.focus === id ? " on" : "") + (d.focus === id && d.warn ? " hot" : "");
    const k = document.createElement("span");
    k.className = "k";
    k.textContent = names[id];
    const v = document.createElement("span");
    v.className = "v";
    v.textContent = chipValue(id, d);
    btn.append(k, v);
    btn.addEventListener("click", () => onChip(id));
    box.append(btn);
  }
  fadeChips();
}

// The chip row scrolls sideways when it is wider than the page. A fade on the
// right edge says there is more.
function fadeChips() {
  const box = $("chips");
  const more = box.scrollWidth > box.clientWidth + 2;
  box.classList.toggle("more", more);
  box.classList.toggle("end", more && box.scrollLeft + box.clientWidth >= box.scrollWidth - 2);
}

function zoomSpan(id, d) {
  const m = d.markers;
  const b = d.bounds;
  if (!m || !b) return null;
  const pad = (a, c) => {
    if (a == null || c == null || !isFinite(a) || !isFinite(c)) return null;
    const extra = Math.max(0.05, Math.abs(c - a) * 0.6);
    return [Math.min(a, c) - extra, Math.max(a, c) + extra];
  };
  switch (id) {
    case "width": return pad(m.width_l, m.width_r);
    case "notches": return pad(b.catch0, b.catch1);
    case "loss": return pad(0, b.catch1);
    case "peak": return m.peak_t == null ? null : pad(m.peak_t - 0.04, m.peak_t + 0.04);
    case "gain": return pad(b.catch1, b.finish);
    case "sag": return m.sag_t == null ? pad(b.catch1, b.finish) : pad(m.sag_t - 0.08, m.sag_t + 0.08);
    case "fall": return m.t80 != null && m.t20 != null ? pad(m.t80, m.t20) : pad(b.finish, b.finish_end);
    case "release": return pad(b.finish, b.finish_end);
    case "run": return pad(b.finish_end, b.approach);
    case "approach": return pad(b.approach, b.interval);
    case "speed": return pad(0, m.peak_t != null ? Math.max(m.peak_t, d.now.speed_peak) : b.finish);
    default: return null;
  }
}

function onChip(id) {
  state.focus = id;
  const span = zoomSpan(id, state.stroke);
  if (span) {
    state.t0 = span[0];
    state.t1 = span[1];
    state.zoomed = true;
    clampView();
  }
  renderHint();
  renderChips();
  draw();
}

function tone(now, ref, lower, have) {
  if (!have || ref == null || ref <= 0 || now == null || now <= 0) return "";
  const ratio = lower ? now / ref : ref / now;
  if (ratio <= 1.1) return "ok";
  if (ratio <= 1.25) return "caution";
  return "warn";
}

function worse(a, b) {
  const rank = (c) => c === "warn" ? 3 : c === "caution" ? 2 : c === "ok" ? 1 : 0;
  return rank(a) >= rank(b) ? a : b;
}

function renderGauges() {
  const box = $("gauges");
  box.innerHTML = "";
  const d = state.stroke;
  const c = copy();
  if (!d) return;
  const head = document.createElement("div");
  head.className = "panel-head";
  const rate = document.createElement("div");
  rate.className = "rate";
  const rateL = document.createElement("span");
  rateL.textContent = d.rate ? Math.round(d.rate) + " spm" : "—";
  const rateR = document.createElement("span");
  rateR.textContent = splitText(d.split);
  rate.append(rateL, rateR);
  const sub = document.createElement("div");
  sub.className = "sub";
  const subL = document.createElement("span");
  subL.textContent = d.view === "rate" ? c.rateAverage(d.samples) : d.view === "average" ? c.average(d.samples) : c.thisStroke;
  const subR = document.createElement("span");
  subR.textContent = d.dps ? d.dps.toFixed(1) + " m" : "";
  sub.append(subL, subR);
  head.append(rate, sub);
  box.append(head);

  const now = d.now || {};
  const ref = d.ref || {};
  const have = d.have_ref && d.valid;
  const scaleOf = (refV, fallback) => have && refV > 0 ? refV * 1.8 : fallback;

  const peakTone = () => {
    if (!have || now.peak_pct == null || ref.peak_pct == null) return "";
    const diff = Math.abs(now.peak_pct - ref.peak_pct);
    if (diff <= 0.1) return "ok";
    if (diff <= 0.2) return "caution";
    return "warn";
  };

  const rows = [
    {
      name: c.phases[0], color: PHASE[0],
      value: d.valid ? Math.round(now.width * 1000) + " ms" : "—",
      tone: tone(now.width, ref.width, true, have),
      frac: now.width / scaleOf(ref.width, 0.3),
      refFrac: have ? ref.width / scaleOf(ref.width, 0.3) : 0,
      cap: d.valid ? c.caps.catch(now.depth, now.loss) : "",
    },
    {
      name: c.phases[1], color: PHASE[1],
      value: d.valid ? "+" + now.gain.toFixed(2) + " m/s" : "—",
      tone: worse(tone(now.gain, ref.gain, false, have), peakTone()),
      frac: now.gain / scaleOf(ref.gain, 1.2),
      refFrac: have ? ref.gain / scaleOf(ref.gain, 1.2) : 0,
      cap: d.valid ? c.caps.drive(Math.round(now.peak_pct * 100), now.sag) : "",
    },
    {
      name: c.phases[2], color: PHASE[2],
      value: d.valid ? Math.round(now.fall * 1000) + " ms" : "—",
      tone: worse(tone(now.fall, ref.fall, true, have), tone(now.release, ref.release, true, have)),
      frac: now.fall / scaleOf(ref.fall, 0.3),
      refFrac: have ? ref.fall / scaleOf(ref.fall, 0.3) : 0,
      cap: d.valid ? c.caps.finish(now.release) : "",
    },
    {
      name: c.phases[3], color: PHASE[3],
      value: d.valid ? Math.round(now.run_mg) + " mg" : "—",
      tone: tone(now.run_mg, ref.run_mg, true, have),
      frac: now.run_mg / scaleOf(ref.run_mg, 40),
      refFrac: have ? ref.run_mg / scaleOf(ref.run_mg, 40) : 0,
      cap: d.valid ? c.caps.run(now.speed_peak) : "",
    },
    {
      name: c.phases[4], color: PHASE[4],
      value: d.valid ? now.lead.toFixed(2) + " s" : "—",
      tone: tone(now.lead, ref.lead, true, have),
      frac: now.lead / scaleOf(ref.lead, 0.5),
      refFrac: have ? ref.lead / scaleOf(ref.lead, 0.5) : 0,
      cap: d.valid ? c.caps.approach(now.approach_loss) : "",
    },
  ];
  if (crew() > 1) {
    let col = "";
    if (d.valid && have && now.notches >= ref.notches + 0.8) col = "warn";
    else if (d.valid && have) col = "ok";
    rows.push({
      name: c.together, color: null,
      value: d.valid ? Math.round(now.width * 1000) + " ms" : "—",
      tone: worse(tone(now.width, ref.width, true, have), col),
      frac: now.width / scaleOf(ref.width, 0.3),
      refFrac: have ? ref.width / scaleOf(ref.width, 0.3) : 0,
      cap: !d.valid ? "" : now.notches < 1.5 ? c.caps.notch : c.caps.notches(now.notches),
    });
  }
  for (const row of rows) box.append(gauge(row));
}

function gauge(row) {
  const el = document.createElement("div");
  el.className = "gauge";
  const top = document.createElement("div");
  top.className = "row";
  const name = document.createElement("span");
  name.className = "name";
  if (row.color) {
    const sw = document.createElement("i");
    sw.style.cssText = "display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:6px;vertical-align:-1px;background:" + row.color;
    name.append(sw);
  }
  name.append(document.createTextNode(row.name));
  const val = document.createElement("span");
  val.className = "val " + (row.tone || "");
  val.textContent = row.value;
  top.append(name, val);
  const bar = document.createElement("div");
  bar.className = "bar";
  const fill = document.createElement("i");
  fill.className = row.tone || "";
  const frac = Math.max(0, Math.min(1, row.frac || 0));
  fill.style.width = (frac * 100) + "%";
  bar.append(fill);
  if (row.refFrac > 0.02 && row.refFrac < 1.15) {
    const tick = document.createElement("em");
    tick.style.left = (Math.min(1, row.refFrac) * 100) + "%";
    bar.append(tick);
  }
  const cap = document.createElement("div");
  cap.className = "cap";
  cap.textContent = row.cap || "";
  el.append(top, bar, cap);
  return el;
}

function renderLegend() {
  const box = $("legend");
  box.innerHTML = "";
  const session = sessionMode();
  const rate = rateMode();
  const names = session ? ["now", "gps", "warn", "sel"] : rate ? ["now", "best", "slow", "fast"] : ["now", "ref", "raw", "gps", "catch", "finish", "mark"];
  const labels = session ? copy().sessionLegend : rate ? copy().rateLegend : copy().legend;
  labels.forEach((label, i) => {
    const span = document.createElement("span");
    span.className = "swatch " + names[i];
    span.textContent = label;
    box.append(span);
  });
  $("help").textContent = session ? copy().sessionHelp : rate ? copy().rateHelp : copy().help;
  $("chips").hidden = session;
  fadeChips();
}

function sampleAt(t, v, time) {
  if (!t || !t.length) return null;
  if (time <= t[0]) return v[0];
  if (time >= t[t.length - 1]) return v[v.length - 1];
  let lo = 0, hi = t.length - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (t[mid] <= time) lo = mid;
    else hi = mid;
  }
  const u = (time - t[lo]) / (t[hi] - t[lo] || 1);
  return v[lo] + (v[hi] - v[lo]) * u;
}

function refPoints(d) {
  if (!d.ref_preroll || !d.ref_accel || !d.drive) return [];
  const pts = [];
  const step = state.preroll / d.ref_preroll.length;
  d.ref_preroll.forEach((a, i) => pts.push([-state.preroll + (i + 0.5) * step, a]));
  for (let i = 0; i < DRIVE_BINS; i++) pts.push([(i + 0.5) / DRIVE_BINS * d.drive, d.ref_accel[i]]);
  for (let i = 0; i < REC_BINS; i++) pts.push([d.drive + (i + 0.5) / REC_BINS * d.rec, d.ref_accel[DRIVE_BINS + i]]);
  return pts;
}

// Round tick values that fit a range, about `want` of them.
function niceTicks(lo, hi, want) {
  const span = hi - lo;
  if (!(span > 0)) return [lo];
  const rough = span / Math.max(1, want);
  const pow = Math.pow(10, Math.floor(Math.log10(rough)));
  const unit = [1, 2, 2.5, 5, 10].map((m) => m * pow).find((s) => s >= rough) || 10 * pow;
  const out = [];
  for (let v = Math.ceil(lo / unit - 1e-9) * unit; v <= hi + 1e-9; v += unit) out.push(Math.abs(v) < unit * 1e-6 ? 0 : v);
  out.unit = unit;
  return out;
}

function roundedRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

// The two plot panels, drawn under the phase bands.
function paintPanels(ctx, box) {
  ctx.fillStyle = PANEL;
  ctx.strokeStyle = EDGE;
  ctx.lineWidth = 1;
  for (const [top, bottom] of [[box.aT, box.aB], [box.vT, box.vB]]) {
    roundedRect(ctx, box.L + 0.5, top + 0.5, box.R - box.L - 1, bottom - top - 1, 4);
    ctx.fill();
    ctx.stroke();
  }
}

function panelTitles(kind) {
  const nl = lang() === "nl";
  return {
    accel: nl ? "Versnelling (g)" : "Acceleration (g)",
    speed: nl ? "Snelheid (m/s)" : "Speed (m/s)",
    time: kind === "session" ? "" : nl ? "seconden vanaf de haal" : "seconds from the catch",
  };
}

// Grid lines and value labels for both panels, with a title above each.
function drawGrid(ctx, box, aRange, vRange, xs, kind) {
  const Ya = (g) => box.aB - (g - aRange[0]) / (aRange[1] - aRange[0]) * (box.aB - box.aT);
  const Yv = (v) => box.vB - (v - vRange[0]) / (vRange[1] - vRange[0]) * (box.vB - box.vT);
  ctx.save();
  ctx.lineWidth = 1;
  for (const [top, bottom] of [[box.aT, box.aB], [box.vT, box.vB]]) {
    ctx.strokeStyle = GRID;
    ctx.beginPath();
    for (const x of xs) {
      ctx.moveTo(Math.round(x) + 0.5, top + 1);
      ctx.lineTo(Math.round(x) + 0.5, bottom - 1);
    }
    ctx.stroke();
  }
  ctx.font = "11px " + FONT;
  ctx.fillStyle = MUTED;
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  const rows = [
    [niceTicks(aRange[0], aRange[1], 4), Ya, box.aT, box.aB, true],
    [niceTicks(vRange[0], vRange[1], 4), Yv, box.vT, box.vB, false],
  ];
  for (const [ticks, Y, top, bottom, signed] of rows) {
    const tenths = ticks.unit * 10;
    const dec = ticks.unit >= 1 ? 0 : ticks.unit >= 0.1 && Math.abs(tenths - Math.round(tenths)) < 1e-6 ? 1 : 2;
    for (const v of ticks) {
      const y = Math.round(Y(v)) + 0.5;
      if (y < top + 5 || y > bottom - 5) continue;
      ctx.strokeStyle = v === 0 && signed ? ZERO : GRID;
      ctx.beginPath();
      ctx.moveTo(box.L + 1, y);
      ctx.lineTo(box.R - 1, y);
      ctx.stroke();
      ctx.fillText((signed && v > 0 ? "+" : "") + v.toFixed(dec), box.L - 8, y);
    }
  }
  const names = panelTitles(kind);
  ctx.font = "600 11px " + FONT;
  ctx.textBaseline = "bottom";
  ctx.textAlign = "left";
  ctx.fillStyle = "#4a443a";
  ctx.fillText(names.accel, box.L, box.aT - 7);
  ctx.fillText(names.speed, box.L, box.vT - 7);
  if (names.time) {
    ctx.font = "11px " + FONT;
    ctx.fillStyle = MUTED;
    ctx.textAlign = "right";
    ctx.fillText(names.time, box.R, box.vT - 7);
  }
  ctx.restore();
}

function strokeTicks(t0, t1) {
  const span = t1 - t0;
  const step = span > 2 ? 0.5 : span > 0.8 ? 0.2 : span > 0.3 ? 0.1 : 0.02;
  const out = [];
  for (let t = Math.ceil(t0 / step) * step; t <= t1 + 1e-6; t += step) out.push(t);
  return { step, times: out };
}

function layoutOf(w, h) {
  const L = 54, R = w - 14, top = 26, gap = 34, axis = 22;
  const inner = Math.max(80, h - top - gap - axis);
  const aH = Math.round(inner * 0.62);
  return { L, R, aT: top, aB: top + aH, vT: top + aH + gap, vB: h - axis, axis: h - 4 };
}

function rangeOf(pts) {
  let lo = Infinity, hi = -Infinity;
  for (const p of pts) {
    if (p == null || !isFinite(p)) continue;
    lo = Math.min(lo, p);
    hi = Math.max(hi, p);
  }
  if (!isFinite(lo)) return null;
  if (hi - lo < 1e-4) { lo -= 0.2; hi += 0.2; }
  const pad = (hi - lo) * 0.12;
  return [lo - pad, hi + pad];
}

function clampSession() {
  if (state.span0 == null) return;
  let t0 = state.s0, t1 = state.s1;
  const span = state.span1 - state.span0;
  const minSpan = Math.min(0.4, span);
  if (t1 - t0 < minSpan) {
    const mid = (t0 + t1) / 2;
    t0 = mid - minSpan / 2;
    t1 = mid + minSpan / 2;
  }
  if (t1 - t0 > span) { t0 = state.span0; t1 = state.span1; }
  if (t0 < state.span0) { t1 += state.span0 - t0; t0 = state.span0; }
  if (t1 > state.span1) { t0 -= t1 - state.span1; t1 = state.span1; }
  state.s0 = Math.max(state.span0, t0);
  state.s1 = Math.min(state.span1, t1);
}

function ensureVisible(i) {
  const row = state.rows[i];
  if (!row || row.t0 == null || state.s0 == null) return;
  if (row.t0 >= state.s0 && row.t1 <= state.s1) return;
  const mid = (row.t0 + row.t1) / 2;
  const span = state.s1 - state.s0;
  state.s0 = mid - span / 2;
  state.s1 = state.s0 + span;
  clampSession();
  scheduleTrace();
}

let traceTimer = 0;
function scheduleTrace() {
  clearTimeout(traceTimer);
  traceTimer = setTimeout(() => { fetchTrace().catch((err) => setStatus(err.message)); }, 80);
}

async function fetchTrace() {
  if (!state.id || state.s0 == null) return;
  const mine = ++state.traceToken;
  const data = await getJSON("/api/trace?id=" + encodeURIComponent(state.id) +
    "&t0=" + state.s0 + "&t1=" + state.s1 + "&n=1000");
  if (mine !== state.traceToken) return;
  state.trace = data;
  if (sessionMode()) drawSession();
}

async function enterSession() {
  if (!state.id) return;
  $("chips").hidden = true;
  const data = await getJSON("/api/trace?id=" + encodeURIComponent(state.id) + "&n=1000");
  state.trace = data;
  state.span0 = data.span[0];
  state.span1 = data.span[1];
  state.s0 = data.span[0];
  state.s1 = data.span[1];
  if (sessionMode()) drawSession();
}

function strokeAt(time) {
  for (let i = state.rows.length - 1; i >= 0; i--) {
    const row = state.rows[i];
    if (row.t0 == null) continue;
    if (time >= row.t0 && time <= row.t1) return i;
  }
  return -1;
}

function openStroke(i) {
  $("chartmode").value = "stroke";
  renderLegend();
  loadStroke(i);
}

function draw() {
  if (sessionMode()) { drawSession(); return; }
  if (rateMode()) { drawRate(); return; }
  const canvas = $("chart");
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  const w = Math.max(10, rect.width);
  const h = Math.max(10, rect.height);
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = CARD;
  ctx.fillRect(0, 0, w, h);
  const d = state.stroke;
  if (!d || !d.valid || !d.curve) {
    ctx.fillStyle = MUTED;
    ctx.font = "16px Segoe UI, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText(copy().unread, w / 2, h / 2);
    return;
  }
  clampView();
  const box = layoutOf(w, h);
  const X = (t) => box.L + (t - state.t0) / (state.t1 - state.t0) * (box.R - box.L);
  const curve = d.curve;
  const rawOn = d.view !== "average";
  const ref = refPoints(d);
  const aVals = [];
  const vVals = [];
  const take = (ts, vs, into) => {
    for (let i = 0; i < ts.length; i++) {
      if (ts[i] >= state.t0 && ts[i] <= state.t1 && vs[i] != null) into.push(vs[i]);
    }
  };
  take(curve.t, curve.a, aVals);
  if (rawOn && d.raw) take(d.raw.t, d.raw.a, aVals);
  for (const p of ref) if (p[0] >= state.t0 && p[0] <= state.t1) aVals.push(p[1]);
  take(curve.t, curve.v, vVals);
  if (d.gps) take(d.gps.t, d.gps.v, vVals);
  const m = d.markers;
  if (m) {
    for (const y of [m.half_level, m.depth_g, m.peak_g, m.level80, m.level20, m.release_g, m.run_base, m.sag_g]) {
      if (y != null) aVals.push(y);
    }
  }
  let aRange = rangeOf(aVals) || [-0.6, 0.6];
  if (aRange[0] > 0) aRange[0] = -0.05;
  if (aRange[1] < 0) aRange[1] = 0.05;
  const vRange = rangeOf(vVals) || [0, 3];
  const Ya = (g) => box.aB - (g - aRange[0]) / (aRange[1] - aRange[0]) * (box.aB - box.aT);
  const Yv = (v) => box.vB - (v - vRange[0]) / (vRange[1] - vRange[0]) * (box.vB - box.vT);

  paintPanels(ctx, box);
  drawBands(ctx, d.bounds, X, box);
  drawGrid(ctx, box, aRange, vRange, strokeTicks(state.t0, state.t1).times.map(X), "stroke");

  const vline = (t, color) => {
    if (t < state.t0 || t > state.t1) return;
    ctx.strokeStyle = color;
    ctx.beginPath();
    for (const [top, bottom] of [[box.aT, box.aB], [box.vT, box.vB]]) {
      ctx.moveTo(X(t), top);
      ctx.lineTo(X(t), bottom);
    }
    ctx.stroke();
  };
  ctx.lineWidth = 1.5;
  vline(0, "#1f7a4d");
  vline(d.drive, "#0e7c86");

  const strokeLine = (pts, color, width) => {
    ctx.beginPath();
    let started = false;
    for (const [t, y] of pts) {
      if (t < state.t0 - 0.02 || t > state.t1 + 0.02 || y == null) { started = false; continue; }
      const x = X(t);
      if (!started) { ctx.moveTo(x, y); started = true; }
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke();
  };
  const pairs = (ts, vs, Y) => {
    const out = [];
    for (let i = 0; i < ts.length; i++) if (vs[i] != null) out.push([ts[i], Y(vs[i])]);
    return out;
  };
  // Each curve stays inside its own panel.
  const inPanel = (top, bottom, paint) => {
    ctx.save();
    ctx.beginPath();
    ctx.rect(box.L, top, box.R - box.L, bottom - top);
    ctx.clip();
    paint();
    ctx.restore();
  };
  inPanel(box.aT, box.aB, () => {
    if (rawOn && d.raw && d.raw.t) strokeLine(pairs(d.raw.t, d.raw.a, Ya), "rgba(29, 78, 137, 0.28)", 1);
    if (ref.length) strokeLine(ref.map((p) => [p[0], Ya(p[1])]), "#8a8478", 1.5);
    strokeLine(pairs(curve.t, curve.a, Ya), "#1d4e89", 2.2);
  });
  inPanel(box.vT, box.vB, () => {
    strokeLine(pairs(curve.t, curve.v, Yv), "#1d4e89", 2.2);
    if (d.gps && d.gps.t) {
      ctx.fillStyle = "#1c1915";
      for (let i = 0; i < d.gps.t.length; i++) {
        const t = d.gps.t[i];
        if (t < state.t0 || t > state.t1 || d.gps.v[i] == null || d.gps.v[i] < 0) continue;
        ctx.beginPath();
        ctx.arc(X(t), Yv(d.gps.v[i]), 3, 0, Math.PI * 2);
        ctx.fill();
      }
    }
  });

  drawMarks(ctx, d, X, Ya, Yv, box);
  drawAxis(ctx, X, box);
}

// The catch phase starts at the catch. The slowing before it is the approach of the
// stroke before, so it takes that colour and no second label.
function drawBands(ctx, b, X, box) {
  const spans = [
    [Math.min(b.catch0, 0), 0, 4, false],
    [0, b.catch1, 0, true],
    [b.catch1, b.finish, 1, true],
    [b.finish, b.finish_end, 2, true],
    [b.finish_end, b.approach, 3, true],
    [b.approach, b.interval, 4, true],
  ];
  ctx.save();
  ctx.beginPath();
  ctx.rect(box.L, box.aT, box.R - box.L, box.aB - box.aT);
  ctx.rect(box.L, box.vT, box.R - box.L, box.vB - box.vT);
  ctx.clip();
  ctx.font = "600 11px " + FONT;
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (const [from, to, p, named] of spans) {
    const u0 = Math.max(state.t0, from);
    const u1 = Math.min(state.t1, to);
    if (u1 - u0 < 0.01) continue;
    ctx.globalAlpha = 0.6;
    ctx.fillStyle = PHASE[p];
    ctx.fillRect(X(u0), box.aT, X(u1) - X(u0), box.aB - box.aT);
    ctx.fillRect(X(u0), box.vT, X(u1) - X(u0), box.vB - box.vT);
    ctx.globalAlpha = 1;
    const name = copy().phases[p];
    if (named && X(u1) - X(u0) > ctx.measureText(name).width + 8) {
      ctx.fillStyle = "#4a443a";
      ctx.fillText(name, (X(u0) + X(u1)) / 2, box.aT + 6);
    }
  }
  ctx.restore();
}

function drawRate() {
  const canvas = $("chart");
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  const w = Math.max(10, rect.width);
  const h = Math.max(10, rect.height);
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = CARD;
  ctx.fillRect(0, 0, w, h);
  const d = state.stroke;
  if (!d || !d.valid || !d.curve) {
    ctx.fillStyle = MUTED;
    ctx.font = "16px Segoe UI, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText(copy().rateEmpty, w / 2, h / 2);
    return;
  }
  clampView();
  const box = layoutOf(w, h);
  const X = (t) => box.L + (t - state.t0) / (state.t1 - state.t0) * (box.R - box.L);
  const shown = visibleRates();
  const series = [];
  for (const row of shown) if (row.curve) series.push(row.curve);
  if (d.best && d.best.curve) series.push(d.best.curve);
  const aVals = [];
  const vVals = [];
  const take = (ts, vs, into) => {
    if (!ts || !vs) return;
    for (let i = 0; i < ts.length; i++) {
      if (ts[i] >= state.t0 && ts[i] <= state.t1 && vs[i] != null) into.push(vs[i]);
    }
  };
  for (const curve of series) {
    take(curve.t, curve.a, aVals);
    take(curve.t, curve.v, vVals);
  }
  const m = d.markers;
  if (m) {
    for (const y of [m.half_level, m.depth_g, m.peak_g, m.level80, m.level20, m.release_g, m.run_base, m.sag_g]) {
      if (y != null) aVals.push(y);
    }
  }
  let aRange = rangeOf(aVals) || [-0.6, 0.6];
  if (aRange[0] > 0) aRange[0] = -0.05;
  if (aRange[1] < 0) aRange[1] = 0.05;
  const vRange = rangeOf(vVals) || [0, 3];
  const Ya = (g) => box.aB - (g - aRange[0]) / (aRange[1] - aRange[0]) * (box.aB - box.aT);
  const Yv = (v) => box.vB - (v - vRange[0]) / (vRange[1] - vRange[0]) * (box.vB - box.vT);

  paintPanels(ctx, box);
  drawBands(ctx, d.bounds, X, box);
  drawGrid(ctx, box, aRange, vRange, strokeTicks(state.t0, state.t1).times.map(X), "rate");

  const vline = (t, color, dash) => {
    if (t == null || t < state.t0 || t > state.t1) return;
    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    if (dash) ctx.setLineDash(dash);
    ctx.beginPath();
    for (const [top, bottom] of [[box.aT, box.aB], [box.vT, box.vB]]) {
      ctx.moveTo(X(t), top);
      ctx.lineTo(X(t), bottom);
    }
    ctx.stroke();
    ctx.restore();
  };
  vline(0, "#1f7a4d");
  vline(d.drive, "#0e7c86");
  if (d.best && d.best.drive) vline(d.best.drive, "#1f7a4d", [4, 3]);

  const strokeLine = (pts, color, width) => {
    ctx.beginPath();
    let started = false;
    for (const [t, y] of pts) {
      if (t < state.t0 - 0.02 || t > state.t1 + 0.02 || y == null) { started = false; continue; }
      const x = X(t);
      if (!started) { ctx.moveTo(x, y); started = true; }
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke();
  };
  const pairs = (ts, vs, Y) => {
    const out = [];
    if (!ts || !vs) return out;
    for (let i = 0; i < ts.length; i++) if (vs[i] != null) out.push([ts[i], Y(vs[i])]);
    return out;
  };
  const quiet = shown.length > 4;
  for (const row of shown) {
    if (!row.curve || row.spm === d.spm) continue;
    strokeLine(pairs(row.curve.t, row.curve.a, Ya), rateInk(row.spm, quiet ? 0.28 : 0.9), quiet ? 1 : 1.7);
    strokeLine(pairs(row.curve.t, row.curve.v, Yv), rateInk(row.spm, quiet ? 0.75 : 1), quiet ? 1.25 : 1.8);
  }
  if (d.best && d.best.curve) {
    strokeLine(pairs(d.best.curve.t, d.best.curve.a, Ya), "#1f7a4d", 2);
    strokeLine(pairs(d.best.curve.t, d.best.curve.v, Yv), "#1f7a4d", 2);
  }
  strokeLine(pairs(d.curve.t, d.curve.a, Ya), "#1d4e89", 2.4);
  strokeLine(pairs(d.curve.t, d.curve.v, Yv), "#1d4e89", 2.4);

  if (!state.zoomed) {
    ctx.font = "11px Segoe UI, sans-serif";
    ctx.textAlign = "left";
    ctx.textBaseline = "middle";
    const placed = [];
    for (const row of shown) {
      const curve = row.curve;
      if (!curve || !curve.t) continue;
      let last = curve.t.length - 1;
      while (last > 0 && (curve.v[last] == null || curve.t[last] == null)) last--;
      const t = curve.t[last];
      const v = curve.v[last];
      if (t == null || v == null || t < state.t0 || t > state.t1) continue;
      let y = Yv(v);
      for (const py of placed) if (Math.abs(py - y) < 11) y = py - 11;
      placed.push(y);
      ctx.fillStyle = row.spm === d.spm ? "#1d4e89" : rateInk(row.spm);
      ctx.fillText(String(row.spm), Math.min(box.R - 16, X(t) + 4), y);
    }
  }

  drawMarks(ctx, d, X, Ya, Yv, box);
  drawAxis(ctx, X, box);
}

function drawSession() {
  const canvas = $("chart");
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  const w = Math.max(10, rect.width);
  const h = Math.max(10, rect.height);
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = CARD;
  ctx.fillRect(0, 0, w, h);
  const tr = state.trace;
  if (!tr || state.s0 == null) {
    ctx.fillStyle = MUTED;
    ctx.font = "16px Segoe UI, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText(lang() === "nl" ? "Sessie wordt gelezen…" : "Reading the session…", w / 2, h / 2);
    return;
  }
  clampSession();
  const box = layoutOf(w, h);
  const X = (t) => box.L + (t - state.s0) / (state.s1 - state.s0) * (box.R - box.L);
  const aVals = [];
  const vVals = [];
  for (let i = 0; i < tr.t.length; i++) {
    if (tr.t[i] < state.s0 - 0.01 || tr.t[i] > state.s1 + 0.01) continue;
    if (tr.lo[i] != null) aVals.push(tr.lo[i], tr.hi[i]);
    if (tr.gps[i] != null && tr.gps[i] >= 0) vVals.push(tr.gps[i]);
  }
  let aRange = rangeOf(aVals) || [-0.5, 0.5];
  if (aRange[0] > 0) aRange[0] = -0.05;
  if (aRange[1] < 0) aRange[1] = 0.05;
  const vRange = rangeOf(vVals) || [0, 3];
  const Ya = (g) => box.aB - (g - aRange[0]) / (aRange[1] - aRange[0]) * (box.aB - box.aT);
  const Yv = (v) => box.vB - (v - vRange[0]) / (vRange[1] - vRange[0]) * (box.vB - box.vT);
  const close = state.s1 - state.s0 < 30;

  paintPanels(ctx, box);
  const panels = [[box.aT, box.aB], [box.vT, box.vB]];
  const clipPanels = () => {
    ctx.beginPath();
    for (const [top, bottom] of panels) ctx.rect(box.L, top, box.R - box.L, bottom - top);
    ctx.clip();
  };
  ctx.save();
  clipPanels();
  for (let i = 0; i < state.rows.length; i++) {
    const row = state.rows[i];
    if (row.t0 == null || row.t1 < state.s0 || row.t0 > state.s1) continue;
    const x0 = X(Math.max(state.s0, row.t0));
    const x1 = X(Math.min(state.s1, row.t1));
    if (x1 - x0 < 0.5) continue;
    const on = i === state.i;
    ctx.fillStyle = on ? (row.warn ? "rgba(196,85,42,0.26)" : "rgba(31,122,77,0.20)")
      : (row.warn ? "rgba(196,85,42,0.11)" : "rgba(31,122,77,0.08)");
    for (const [top, bottom] of panels) ctx.fillRect(x0, top, x1 - x0, bottom - top);
    ctx.fillStyle = row.warn ? "#c4552a" : "#1f7a4d";
    ctx.fillRect(x0, box.aT, Math.max(1, x1 - x0), on ? 4 : 2);
    if (close && row.t1 >= state.s0 && row.t1 <= state.s1) {
      ctx.strokeStyle = "#1f7a4d";
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      for (const [top, bottom] of panels) {
        ctx.moveTo(X(row.t1), top);
        ctx.lineTo(X(row.t1), bottom);
      }
      ctx.stroke();
    }
    if (close && row.drive && row.t0 + row.drive >= state.s0 && row.t0 + row.drive <= state.s1) {
      ctx.strokeStyle = "#0e7c86";
      ctx.beginPath();
      for (const [top, bottom] of panels) {
        ctx.moveTo(X(row.t0 + row.drive), top);
        ctx.lineTo(X(row.t0 + row.drive), bottom);
      }
      ctx.stroke();
    }
  }
  ctx.restore();

  const gridStep = tickStep(state.s1 - state.s0);
  const gridX = [];
  for (let t = Math.ceil(state.s0 / gridStep) * gridStep; t <= state.s1 + 1e-6; t += gridStep) gridX.push(X(t));
  drawGrid(ctx, box, aRange, vRange, gridX, "session");

  ctx.save();
  clipPanels();
  ctx.lineJoin = "round";
  ctx.beginPath();
  let pen = false;
  for (let i = 0; i < tr.t.length; i++) {
    if (tr.hi[i] == null) continue;
    const x = X(tr.t[i]), y = Ya(tr.hi[i]);
    if (!pen) { ctx.moveTo(x, y); pen = true; }
    else ctx.lineTo(x, y);
  }
  for (let i = tr.t.length - 1; i >= 0; i--) {
    if (tr.lo[i] == null) continue;
    ctx.lineTo(X(tr.t[i]), Ya(tr.lo[i]));
  }
  ctx.closePath();
  ctx.fillStyle = "rgba(29, 78, 137, 0.18)";
  ctx.fill();
  ctx.beginPath();
  pen = false;
  for (let i = 0; i < tr.t.length; i++) {
    if (tr.lo[i] == null || tr.hi[i] == null) { pen = false; continue; }
    const x = X(tr.t[i]);
    const y = Ya((tr.lo[i] + tr.hi[i]) / 2);
    if (!pen) { ctx.moveTo(x, y); pen = true; }
    else ctx.lineTo(x, y);
  }
  ctx.strokeStyle = "#1d4e89";
  ctx.lineWidth = 1.6;
  ctx.stroke();
  ctx.beginPath();
  pen = false;
  for (let i = 0; i < tr.t.length; i++) {
    if (tr.gps[i] == null || tr.gps[i] < 0) { pen = false; continue; }
    const x = X(tr.t[i]), y = Yv(tr.gps[i]);
    if (!pen) { ctx.moveTo(x, y); pen = true; }
    else ctx.lineTo(x, y);
  }
  ctx.strokeStyle = "#1c1915";
  ctx.lineWidth = 1.5;
  ctx.stroke();
  ctx.restore();

  const step = tickStep(state.s1 - state.s0);
  ctx.fillStyle = MUTED;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  const start = Math.ceil(state.s0 / step) * step;
  for (let t = start; t <= state.s1 + 1e-6; t += step) {
    const x = X(t);
    ctx.strokeStyle = "#d9d2c5";
    ctx.beginPath();
    ctx.moveTo(x, box.vB);
    ctx.lineTo(x, box.vB + 4);
    ctx.stroke();
    ctx.fillText(step >= 1 ? clock(t) : t.toFixed(1), x, box.vB + 5);
  }
}

function tickStep(span) {
  const steps = [0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
  for (const s of steps) if (span / s <= 8) return s;
  return 600;
}

function drawAxis(ctx, X, box) {
  const span = state.t1 - state.t0;
  const step = span > 2 ? 0.5 : span > 0.8 ? 0.2 : span > 0.3 ? 0.1 : 0.02;
  ctx.fillStyle = MUTED;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  const start = Math.ceil(state.t0 / step) * step;
  for (let t = start; t <= state.t1 + 1e-6; t += step) {
    const x = X(t);
    ctx.strokeStyle = "#d9d2c5";
    ctx.beginPath();
    ctx.moveTo(x, box.vB);
    ctx.lineTo(x, box.vB + 4);
    ctx.stroke();
    ctx.fillText(t.toFixed(step < 0.1 ? 2 : 1), x, box.vB + 5);
  }
}

function drawMarks(ctx, d, X, Ya, Yv, box) {
  const m = d.markers;
  const b = d.bounds;
  const focus = state.focus;
  const hot = d.warn && d.focus === focus;
  const ink = hot ? "#9a3412" : "#9a3412";
  const quiet = "rgba(28, 25, 21, 0.55)";
  ctx.lineWidth = 1.5;
  ctx.font = "12px Segoe UI, sans-serif";
  ctx.textBaseline = "bottom";

  const inView = (t) => t != null && t >= state.t0 && t <= state.t1;
  const label = (text, x, y) => {
    ctx.textAlign = "center";
    ctx.lineWidth = 3;
    ctx.strokeStyle = CARD;
    ctx.fillStyle = ink;
    const xx = Math.max(box.L + 24, Math.min(box.R - 24, x));
    const yy = Math.max(box.aT + 16, y) - 6;
    ctx.strokeText(text, xx, yy);
    ctx.fillText(text, xx, yy);
  };

  if (m.width_l != null && m.width_r != null && m.half_level != null) {
    const on = focus === "width";
    ctx.strokeStyle = on ? ink : quiet;
    ctx.lineWidth = on ? 2.5 : 1;
    ctx.beginPath();
    ctx.moveTo(X(m.width_l), Ya(m.half_level));
    ctx.lineTo(X(m.width_r), Ya(m.half_level));
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(X(m.width_l), Ya(m.half_level) - 5);
    ctx.lineTo(X(m.width_l), Ya(m.half_level) + 5);
    ctx.moveTo(X(m.width_r), Ya(m.half_level) - 5);
    ctx.lineTo(X(m.width_r), Ya(m.half_level) + 5);
    ctx.stroke();
    if (on) label(Math.round(d.now.width * 1000) + " ms", X((m.width_l + m.width_r) / 2), Ya(m.half_level));
  }

  const dot = (t, y, on, text) => {
    if (!inView(t) || y == null) return;
    ctx.beginPath();
    ctx.fillStyle = on ? ink : quiet;
    ctx.arc(X(t), y, on ? 5 : 3, 0, Math.PI * 2);
    ctx.fill();
    if (on && text) label(text, X(t), y);
  };

  dot(m.depth_t, Ya(m.depth_g), focus === "width" || focus === "notches", "");
  if (m.notch_t) {
    m.notch_t.forEach((t, i) => {
      const y = sampleAt(d.curve.t, d.curve.a, t);
      if (y == null || !inView(t)) return;
      const on = focus === "notches";
      ctx.fillStyle = on ? ink : quiet;
      ctx.beginPath();
      ctx.moveTo(X(t), Ya(y) + 7);
      ctx.lineTo(X(t) - 5, Ya(y) - 3);
      ctx.lineTo(X(t) + 5, Ya(y) - 3);
      ctx.fill();
      if (on && i === 0) label(copy().chips.notches, X(t), Ya(y));
    });
  }
  dot(m.peak_t, Ya(m.peak_g), focus === "peak", focus === "peak" ? Math.round(d.now.peak_pct * 100) + "%" : "");
  if (m.sag_t != null) dot(m.sag_t, Ya(m.sag_g), focus === "sag", focus === "sag" ? d.now.sag.toFixed(2) + " g" : "");

  if (m.t80 != null && m.t20 != null) {
    const on = focus === "fall";
    ctx.strokeStyle = on ? ink : quiet;
    ctx.lineWidth = on ? 2 : 1;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(X(m.t80), Ya(m.level80));
    ctx.lineTo(X(m.t20), Ya(m.level80));
    ctx.moveTo(X(m.t80), Ya(m.level20));
    ctx.lineTo(X(m.t20), Ya(m.level20));
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.beginPath();
    ctx.moveTo(X(m.t20), Ya(m.level80));
    ctx.lineTo(X(m.t20), Ya(m.level20));
    ctx.stroke();
    if (on) label(Math.round(d.now.fall * 1000) + " ms", X((m.t80 + m.t20) / 2), Ya(Math.max(m.level80, m.level20)));
  }

  if (m.release_t != null) {
    const on = focus === "release";
    if (m.run_base != null && b.finish != null && b.finish_end != null) {
      ctx.strokeStyle = on ? ink : quiet;
      ctx.setLineDash([4, 3]);
      ctx.beginPath();
      ctx.moveTo(X(b.finish), Ya(m.run_base));
      ctx.lineTo(X(b.finish_end), Ya(m.run_base));
      ctx.stroke();
      ctx.setLineDash([]);
    }
    dot(m.release_t, Ya(m.release_g), on, on ? d.now.release.toFixed(2) + " g" : "");
  }

  if (focus === "run" && m.run_base != null && b.finish_end != null && b.approach != null) {
    ctx.strokeStyle = ink;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(X(b.finish_end), Ya(m.run_base));
    ctx.lineTo(X(b.approach), Ya(m.run_base));
    ctx.stroke();
    label(Math.round(d.now.run_mg) + " mg", X((b.finish_end + b.approach) / 2), Ya(m.run_base));
  }

  if (b.approach != null) {
    const on = focus === "approach";
    ctx.strokeStyle = on ? ink : quiet;
    ctx.lineWidth = on ? 2 : 1;
    ctx.setLineDash([2, 3]);
    ctx.beginPath();
    ctx.moveTo(X(b.approach), box.aT);
    ctx.lineTo(X(b.approach), box.aB);
    ctx.moveTo(X(b.approach), box.vT);
    ctx.lineTo(X(b.approach), box.vB);
    ctx.stroke();
    ctx.setLineDash([]);
    if (on) label(d.now.lead.toFixed(2) + " s", X((b.approach + b.interval) / 2), box.aT + 28);
  }

  const speedSpan = (t0, t1, on, text) => {
    const y0 = sampleAt(d.curve.t, d.curve.v, t0);
    const y1 = sampleAt(d.curve.t, d.curve.v, t1);
    if (y0 == null || y1 == null) return;
    ctx.strokeStyle = on ? ink : quiet;
    ctx.fillStyle = on ? ink : quiet;
    ctx.lineWidth = on ? 2.5 : 1;
    ctx.beginPath();
    ctx.moveTo(X(t0), Yv(y0));
    ctx.lineTo(X(t1), Yv(y1));
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(X(t0), Yv(y0), on ? 4 : 2.5, 0, Math.PI * 2);
    ctx.arc(X(t1), Yv(y1), on ? 4 : 2.5, 0, Math.PI * 2);
    ctx.fill();
    if (on) {
      ctx.textAlign = "center";
      ctx.lineWidth = 3;
      ctx.strokeStyle = CARD;
      const lx = X((t0 + t1) / 2);
      const ly = Math.min(Yv(y0), Yv(y1)) - 6;
      ctx.strokeText(text, lx, ly);
      ctx.fillText(text, lx, ly);
    }
  };
  if (b.catch1 != null) speedSpan(0, b.catch1, focus === "loss", "-" + d.now.loss.toFixed(2) + " m/s");
  if (b.catch1 != null && b.finish != null) speedSpan(b.catch1, b.finish, focus === "gain", "+" + d.now.gain.toFixed(2) + " m/s");
  if (d.now.speed_peak != null) {
    const t = d.now.speed_peak;
    const y = sampleAt(d.curve.t, d.curve.v, t);
    dot(t, y == null ? null : Yv(y), focus === "speed", focus === "speed" ? t.toFixed(2) + " s" : "");
  }
}

function resize() { draw(); }

document.addEventListener("keydown", (e) => {
  if (e.target.matches("input, select, textarea")) return;
  if (e.key !== "ArrowLeft" && e.key !== "ArrowRight" && e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
  e.preventDefault();
  const dir = (e.key === "ArrowLeft" || e.key === "ArrowUp") ? -1 : 1;
  if (rateMode() && state.rates && state.rates.length) {
    const next = Math.max(0, Math.min(state.rates.length - 1, state.ri + dir));
    if (next !== state.ri) selectRate(next);
    return;
  }
  const next = Math.max(0, Math.min(state.rows.length - 1, state.i + dir));
  if (next === state.i) return;
  if (sessionMode()) ensureVisible(next);
  loadStroke(next);
});

const canvas = $("chart");
let drag = null;
function timeAt(offsetX) {
  const rect = canvas.getBoundingClientRect();
  const box = layoutOf(rect.width, rect.height);
  const u = Math.min(1, Math.max(0, (offsetX - box.L) / (box.R - box.L)));
  return state.s0 + u * (state.s1 - state.s0);
}
canvas.addEventListener("pointerdown", (e) => {
  if (e.button !== 0) return;
  if (sessionMode()) {
    if (state.s0 == null) return;
    drag = { x: e.offsetX, y: e.offsetY, t0: state.s0, t1: state.s1, moved: false, session: true };
  } else {
    if (!state.stroke) return;
    drag = { x: e.offsetX, y: e.offsetY, t0: state.t0, t1: state.t1, moved: false, session: false };
  }
  canvas.setPointerCapture(e.pointerId);
});
canvas.addEventListener("pointermove", (e) => {
  if (!drag) return;
  if (Math.abs(e.offsetX - drag.x) + Math.abs(e.offsetY - drag.y) > 4) drag.moved = true;
  if (!drag.moved) return;
  const rect = canvas.getBoundingClientRect();
  const box = layoutOf(rect.width, rect.height);
  const span = drag.t1 - drag.t0;
  const dt = (e.offsetX - drag.x) / (box.R - box.L) * span;
  if (drag.session) {
    state.s0 = drag.t0 - dt;
    state.s1 = drag.t1 - dt;
    clampSession();
    drawSession();
  } else {
    state.t0 = drag.t0 - dt;
    state.t1 = drag.t1 - dt;
    state.zoomed = true;
    clampView();
    draw();
  }
});
canvas.addEventListener("pointerup", (e) => {
  if (drag && drag.session && !drag.moved) {
    const hit = strokeAt(timeAt(e.offsetX));
    if (hit >= 0) loadStroke(hit);
  }
  if (drag && drag.session && drag.moved) scheduleTrace();
  drag = null;
});
canvas.addEventListener("pointercancel", () => { drag = null; });
canvas.addEventListener("dblclick", (e) => {
  if (sessionMode()) {
    const hit = strokeAt(timeAt(e.offsetX));
    if (hit >= 0) openStroke(hit);
    else {
      state.s0 = state.span0;
      state.s1 = state.span1;
      scheduleTrace();
      drawSession();
    }
    return;
  }
  resetZoom();
  draw();
});
canvas.addEventListener("wheel", (e) => {
  if (sessionMode()) {
    if (state.s0 == null) return;
    e.preventDefault();
    const rect = canvas.getBoundingClientRect();
    const box = layoutOf(rect.width, rect.height);
    const u = Math.min(1, Math.max(0, (e.offsetX - box.L) / (box.R - box.L)));
    const t = state.s0 + u * (state.s1 - state.s0);
    const factor = e.deltaY > 0 ? 1.18 : 1 / 1.18;
    const full = state.span1 - state.span0;
    let span = Math.max(0.4, Math.min(full, (state.s1 - state.s0) * factor));
    state.s0 = t - u * span;
    state.s1 = state.s0 + span;
    clampSession();
    drawSession();
    scheduleTrace();
    return;
  }
  if (!state.stroke) return;
  e.preventDefault();
  const rect = canvas.getBoundingClientRect();
  const box = layoutOf(rect.width, rect.height);
  const x = e.offsetX;
  const u = Math.min(1, Math.max(0, (x - box.L) / (box.R - box.L)));
  const t = state.t0 + u * (state.t1 - state.t0);
  const factor = e.deltaY > 0 ? 1.18 : 1 / 1.18;
  let span = (state.t1 - state.t0) * factor;
  const full = fullSpan(state.stroke);
  span = Math.max(0.08, Math.min(full[1] - full[0], span));
  state.t0 = t - u * span;
  state.t1 = state.t0 + span;
  state.zoomed = span < full[1] - full[0] - 0.01;
  clampView();
  draw();
}, { passive: false });

$("chartmode").addEventListener("change", () => {
  renderLegend();
  if (!state.id) return;
  if (!rateMode() && state.blurb) setStatus(state.blurb);
  if (sessionMode()) enterSession().catch((err) => setStatus(err.message));
  else if (rateMode()) loadRates().catch((err) => setStatus(err.message));
  else loadStroke(state.i).catch((err) => setStatus(err.message));
});

window.addEventListener("resize", () => { resize(); fadeChips(); });
$("chips").addEventListener("scroll", fadeChips);
$("chips").addEventListener("wheel", (e) => {
  const box = $("chips");
  if (box.scrollWidth <= box.clientWidth || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
  box.scrollLeft += e.deltaY;
  e.preventDefault();
}, { passive: false });
$("session").addEventListener("change", () => loadSession($("session").value).catch((err) => setStatus(err.message)));
$("crew").addEventListener("change", () => reloadHints().catch((err) => setStatus(err.message)));
$("lang").addEventListener("change", () => {
  try { localStorage.setItem("stroke-lang", lang()); } catch (err) { /* storage blocked */ }
  applyChrome();
  renderLegend();
  reloadHints().catch((err) => setStatus(err.message));
});
$("view").addEventListener("change", () => {
  if (state.id && !rateMode()) reloadHints().catch((err) => setStatus(err.message));
});
function chooseFiles() { $("file").click(); }
$("open").addEventListener("click", chooseFiles);
$("welcome-open").addEventListener("click", chooseFiles);
$("welcome-tab5").addEventListener("click", () => $("tab5").click());
$("file").addEventListener("change", async () => {
  const input = $("file");
  const files = input.files ? [...input.files] : [];
  if (!files.length) return;
  setStatus("Reading " + files.map((file) => file.name).join(", ") + "…");
  const body = new FormData();
  for (const file of files) body.append("file", file, file.name);
  try {
    const opened = await getJSON("/api/upload", { method: "POST", body });
    input.value = "";
    await loadSessions(opened.id);
  } catch (err) {
    setStatus(err.message);
  }
});

startLanguage();
renderLegend();
loadSessions().catch((err) => setStatus(err.message));
