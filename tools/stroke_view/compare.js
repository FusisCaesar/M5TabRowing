"use strict";

// Compare page: the average stroke at one stroke rate, side by side for several sessions, to see
// what a different crew, boat set-up or rigging did. Each session is analysed once and its
// per-rate averages are kept in the page, so ticking sessions on and off is instant.

const CMP_MAX = 6;
const CMP_COLORS = ["#1d4e89", "#c4552a", "#1f7a4d", "#7b3f98", "#b8860b", "#0e7c86"];

Object.assign(CHROME.en, { pageCompare: "Compare" });
Object.assign(CHROME.nl, { pageCompare: "Vergelijk" });
applyChrome();

const CMP_COPY = {
  en: {
    title: "Compare sessions",
    lead: "Tick the sessions to compare and pick a stroke rate. Each line is the average stroke of that session at that rate.",
    rate: "Stroke rate",
    rateOption: (spm, k, of) => spm + " spm · " + k + (of > 1 ? " of " + of + " sessions" : " session"),
    sessions: "Sessions",
    label: "Note, e.g. rigging 85 cm",
    full: "At most " + CMP_MAX + " at once.",
    working: (s) => "Analysing… " + s + " s",
    waiting: "Waiting…",
    noRate: "No strokes at this rate",
    noneReadable: "No rate with 3 or more strokes",
    failed: "Could not read this session",
    pickOne: "Tick a session to start.",
    pickTwo: "Tick a second session to compare.",
    noCommon: "None of the ticked sessions has three or more strokes at one stroke rate.",
    missing: (names, spm) => "No strokes at " + spm + " spm in " + names + ".",
    baseline: "baseline",
    fine: "A rate counts every stroke within half a stroke per minute of it, and needs three or more. The speed line is lifted to the average speed of that session, so a faster session sits higher. Differences are against the first ticked session. The bars along the top of the chart show when the blade is in the water for each session, an estimate from the boat's acceleration.",
    chartAria: "Average stroke at the chosen rate for each ticked session",
    time: (t) => t.toFixed(2) + " s from the catch",
    accel: "acc",
    speed: "speed",
    rows: {
      n: "Strokes averaged",
      split: "Split /500 m",
      dps: "Distance per stroke",
      blade: "Blade in water",
      bladePct: "Blade share of the stroke",
      drive: "Drive time",
      rec: "Recovery time",
      ratio: "Drive : recovery",
      peak: "Peak of the drive",
      width: "Catch width",
      depth: "Catch depth",
      loss: "Speed lost at the catch",
      gain: "Speed gained in the drive",
      fall: "Finish, fall time",
      run: "Run",
      lead: "Approach",
    },
  },
  nl: {
    title: "Sessies vergelijken",
    lead: "Vink de sessies aan die je wilt vergelijken en kies een slagtempo. Elke lijn is de gemiddelde slag van die sessie op dat tempo.",
    rate: "Slagtempo",
    rateOption: (spm, k, of) => spm + " spm · " + k + (of > 1 ? " van " + of + " sessies" : " sessie"),
    sessions: "Sessies",
    label: "Notitie, bijv. rigging 85 cm",
    full: "Maximaal " + CMP_MAX + " tegelijk.",
    working: (s) => "Analyseren… " + s + " s",
    waiting: "Wachten…",
    noRate: "Geen slagen op dit tempo",
    noneReadable: "Geen tempo met 3 of meer slagen",
    failed: "Deze sessie kon niet worden gelezen",
    pickOne: "Vink een sessie aan om te beginnen.",
    pickTwo: "Vink een tweede sessie aan om te vergelijken.",
    noCommon: "Geen van de aangevinkte sessies heeft drie of meer slagen op één slagtempo.",
    missing: (names, spm) => "Geen slagen op " + spm + " spm in " + names + ".",
    baseline: "basis",
    fine: "Een tempo telt elke slag binnen een halve slag per minuut ervan en heeft er minstens drie nodig. De snelheidslijn is opgetild naar de gemiddelde snelheid van die sessie, dus een snellere sessie ligt hoger. Verschillen zijn ten opzichte van de eerste aangevinkte sessie. De balkjes boven in de grafiek tonen wanneer het blad in het water is voor elke sessie, een schatting uit de versnelling van de boot.",
    chartAria: "Gemiddelde slag op het gekozen tempo voor elke aangevinkte sessie",
    time: (t) => t.toFixed(2) + " s na de haal",
    accel: "versn",
    speed: "snelh",
    rows: {
      n: "Slagen in gemiddelde",
      split: "Split /500 m",
      dps: "Afstand per slag",
      blade: "Blad in het water",
      bladePct: "Blad in water, deel van de slag",
      drive: "Duur van de haal",
      rec: "Duur van het herstel",
      ratio: "Haal : herstel",
      peak: "Piek van de haal",
      width: "Breedte van de inzet",
      depth: "Diepte van de inzet",
      loss: "Snelheid verloren bij de inzet",
      gain: "Snelheid gewonnen in de haal",
      fall: "Uitzet, valtijd",
      run: "Doorglijden",
      lead: "Aanloop",
    },
  },
};

const cmp = {
  sel: [],           // ids ticked, in the order they were ticked: the first is the baseline
  data: {},          // id -> the per-rate report of the server
  state: {},         // id -> "waiting" | "working" | "failed"
  secs: {},          // id -> seconds the analysis has taken
  rate: null,        // the chosen rate, or null for the automatic one
  hover: null,       // time in the chart under the pointer
  queue: Promise.resolve(),
  labels: {},
  listed: null,      // the session list the rows were built from
};

try { cmp.labels = JSON.parse(localStorage.getItem("stroke-cmp-labels") || "{}") || {}; } catch (err) { cmp.labels = {}; }

function cmpCopy() { return CMP_COPY[lang()]; }
function cmpSaveLabels() {
  try { localStorage.setItem("stroke-cmp-labels", JSON.stringify(cmp.labels)); } catch (err) { /* storage blocked */ }
}

function cmpColor(id) {
  const i = cmp.sel.indexOf(id);
  return i < 0 ? "#b7ad9e" : CMP_COLORS[i % CMP_COLORS.length];
}

function cmpName(id) {
  if (cmp.labels[id]) return cmp.labels[id];
  const listed = state.sessions.find((s) => s.id === id);
  return listed ? listed.name : id;
}

function cmpWhen(id) {
  const d = cmp.data[id];
  if (!d || !d.started) return "";
  const when = new Date(d.started);
  if (isNaN(when.getTime())) return "";
  return when.toLocaleString(lang() === "nl" ? "nl-NL" : "en-GB", {
    day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit",
  });
}

function cmpRowAt(id, spm) {
  const d = cmp.data[id];
  return d && d.rates ? d.rates.find((r) => r.spm === spm) || null : null;
}

// The session's own readout of the analysis: one at a time, as the server analyses one at a time.
function cmpEnsure(id) {
  if (cmp.data[id] || cmp.state[id] === "working" || cmp.state[id] === "waiting") return;
  cmp.state[id] = "waiting";
  cmp.queue = cmp.queue.then(async () => {
    if (!cmp.sel.includes(id)) { delete cmp.state[id]; return; }
    cmp.state[id] = "working";
    cmp.secs[id] = 0;
    cmpRenderList();
    const keep = analysingNotice;
    analysingNotice = (s) => { cmp.secs[id] = s; cmpRenderList(); };
    try {
      cmp.data[id] = await getJSON("/api/rates?id=" + encodeURIComponent(id) + "&crew=1&lang=" + lang());
      delete cmp.state[id];
    } catch (err) {
      cmp.state[id] = "failed";
    } finally {
      analysingNotice = keep;
    }
    cmpRenderAll();
  });
  cmpRenderList();
}

// A session that was read again (uploaded once more under the same name) is no longer what we kept.
function cmpForget(id) {
  delete cmp.data[id];
  delete cmp.state[id];
}

function cmpToggle(id, on) {
  const at = cmp.sel.indexOf(id);
  if (on && at < 0) {
    if (cmp.sel.length >= CMP_MAX) { cmpRenderList(); return; }
    cmp.sel.push(id);
    cmpEnsure(id);
  } else if (!on && at >= 0) {
    cmp.sel.splice(at, 1);
  }
  cmpRenderAll();
}

// Rates that at least one ticked session has, with how many of them do.
function cmpRates() {
  const counts = new Map();
  let have = 0;
  for (const id of cmp.sel) {
    const d = cmp.data[id];
    if (!d || !d.rates || !d.rates.length) continue;
    have++;
    for (const r of d.rates) {
      const e = counts.get(r.spm) || { spm: r.spm, k: 0, n: 0 };
      e.k++;
      e.n += r.n;
      counts.set(r.spm, e);
    }
  }
  return { list: [...counts.values()].sort((a, b) => a.spm - b.spm), have };
}

function cmpChosenRate(list) {
  if (!list.length) return null;
  if (cmp.rate != null && list.some((e) => e.spm === cmp.rate)) return cmp.rate;
  // Automatic: the rate most of the ticked sessions share, then the one with most strokes.
  let best = list[0];
  for (const e of list) if (e.k > best.k || (e.k === best.k && e.n > best.n)) best = e;
  return best.spm;
}

function cmpSeries(spm) {
  const out = [];
  for (const id of cmp.sel) {
    const row = cmpRowAt(id, spm);
    if (row && row.curve) out.push({ id, row, color: cmpColor(id) });
  }
  return out;
}

function split1(s) {
  if (!s || s <= 0) return "—";
  const m = Math.floor(s / 60);
  const sec = s - m * 60;
  return m + ":" + (sec < 10 ? "0" : "") + sec.toFixed(1);
}

// One line per figure in the table. get() takes the rate row of the server; best says which way is better.
const CMP_ROWS = [
  { key: "n", get: (r) => r.n, dec: 0, delta: false },
  { key: "split", get: (r) => r.split, show: split1, step: (d) => (d > 0 ? "+" : "−") + Math.abs(d).toFixed(1) + " s", best: "low" },
  { key: "dps", get: (r) => r.dps, dec: 2, unit: " m", best: "high" },
  { key: "blade", get: (r) => r.now && r.now.blade, dec: 2, unit: " s" },
  { key: "bladePct", get: (r) => (r.now && r.now.blade_pct != null ? r.now.blade_pct * 100 : null), dec: 0, unit: " %" },
  { key: "drive", get: (r) => r.drive, dec: 2, unit: " s" },
  { key: "rec", get: (r) => r.rec, dec: 2, unit: " s" },
  { key: "ratio", get: (r) => (r.drive > 0 ? r.rec / r.drive : null), dec: 1, show: (v) => "1 : " + v.toFixed(1) },
  { key: "peak", get: (r) => r.now && r.now.peak, dec: 2, unit: " g" },
  { key: "width", get: (r) => (r.now ? r.now.width * 1000 : null), dec: 0, unit: " ms" },
  { key: "depth", get: (r) => r.now && r.now.depth, dec: 2, unit: " g" },
  { key: "loss", get: (r) => r.now && r.now.loss, dec: 2, unit: " m/s" },
  { key: "gain", get: (r) => r.now && r.now.gain, dec: 2, unit: " m/s" },
  { key: "fall", get: (r) => (r.now ? r.now.fall * 1000 : null), dec: 0, unit: " ms" },
  { key: "run", get: (r) => r.now && r.now.run_mg, dec: 0, unit: " mg" },
  { key: "lead", get: (r) => r.now && r.now.lead, dec: 2, unit: " s" },
];

function cmpShowValue(spec, v) {
  if (v == null || !isFinite(v)) return "—";
  if (spec.show) return spec.show(v);
  return v.toFixed(spec.dec) + (spec.unit || "");
}

function cmpShowDelta(spec, d) {
  if (spec.step) return spec.step(d);
  const text = Math.abs(d).toFixed(spec.dec);
  if (Number(text) === 0) return "±0";
  return (d > 0 ? "+" : "−") + text;
}

function cmpRenderList() {
  const box = $("cmp-list");
  const c = cmpCopy();
  const sessions = state.sessions;
  const full = cmp.sel.length >= CMP_MAX;
  // Rebuild only when the sessions or the language changed, so a note being typed keeps its focus.
  const key = lang() + "|" + sessions.map((s) => s.id).join(",");
  if (cmp.listed !== key) {
    cmp.listed = key;
    box.innerHTML = "";
    for (const s of sessions) {
      const row = document.createElement("div");
      row.className = "cmp-row";
      row.dataset.id = s.id;
      const tick = document.createElement("label");
      tick.className = "cmp-tick";
      const input = document.createElement("input");
      input.type = "checkbox";
      input.addEventListener("change", () => cmpToggle(s.id, input.checked));
      const swatch = document.createElement("i");
      const text = document.createElement("span");
      text.className = "cmp-name";
      const name = document.createElement("b");
      const meta = document.createElement("small");
      text.append(name, meta);
      tick.append(input, swatch, text);
      const note = document.createElement("input");
      note.type = "text";
      note.className = "cmp-note";
      note.placeholder = c.label;
      note.maxLength = 60;
      note.value = cmp.labels[s.id] || "";
      note.addEventListener("input", () => {
        const v = note.value.trim();
        if (v) cmp.labels[s.id] = v;
        else delete cmp.labels[s.id];
        cmpSaveLabels();
        cmpRenderResults();
        const nm = row.querySelector(".cmp-name b");
        nm.textContent = cmp.labels[s.id] || s.name;
      });
      row.append(tick, note);
      box.append(row);
    }
  }
  for (const row of box.children) {
    const id = row.dataset.id;
    const on = cmp.sel.includes(id);
    const listed = sessions.find((s) => s.id === id);
    row.classList.toggle("on", on);
    row.querySelector("input[type=checkbox]").checked = on;
    row.querySelector("input[type=checkbox]").disabled = !on && full;
    row.querySelector("i").style.background = on ? cmpColor(id) : "transparent";
    row.querySelector("b").textContent = cmp.labels[id] || (listed ? listed.name : id);
    let meta = cmpWhen(id);
    const st = cmp.state[id];
    if (on && st === "working") meta = c.working(cmp.secs[id] || 0);
    else if (on && st === "waiting") meta = c.waiting;
    else if (on && st === "failed") meta = c.failed;
    else if (on && cmp.data[id] && !(cmp.data[id].rates || []).length) meta = c.noneReadable;
    else if (!meta && listed) meta = listed.strokes + " strokes";
    const small = row.querySelector("small");
    small.textContent = meta;
    small.classList.toggle("warn", on && st === "failed");
  }
  $("cmp-full").hidden = !full;
}

function cmpRenderTable(spm, series) {
  const c = cmpCopy();
  const table = $("cmp-table");
  table.innerHTML = "";
  if (spm == null || !series.length) return;
  const head = document.createElement("tr");
  head.append(document.createElement("th"));
  series.forEach((s, i) => {
    const th = document.createElement("th");
    th.style.borderTopColor = s.color;
    const name = document.createElement("div");
    name.className = "cmp-th";
    name.textContent = cmpName(s.id);
    th.append(name);
    const when = cmpWhen(s.id);
    if (when || i === 0) {
      const small = document.createElement("small");
      small.textContent = [when, i === 0 && series.length > 1 ? c.baseline : ""].filter(Boolean).join(" · ");
      th.append(small);
    }
    head.append(th);
  });
  table.append(head);
  for (const spec of CMP_ROWS) {
    const values = series.map((s) => {
      const v = spec.get(s.row);
      return v == null || !isFinite(v) || (spec.best && v <= 0) ? null : v;
    });
    const have = values.filter((v) => v != null);
    let bestV = null;
    if (spec.best && have.length > 1) {
      bestV = spec.best === "low" ? Math.min(...have) : Math.max(...have);
      if (have.every((v) => Math.abs(v - bestV) < 1e-9)) bestV = null;
    }
    const tr = document.createElement("tr");
    const th = document.createElement("th");
    th.scope = "row";
    th.textContent = c.rows[spec.key];
    tr.append(th);
    const baseAt = values.findIndex((v) => v != null);
    const base = baseAt < 0 ? null : values[baseAt];
    values.forEach((v, i) => {
      const td = document.createElement("td");
      const main = document.createElement("span");
      main.textContent = cmpShowValue(spec, v);
      td.append(main);
      if (bestV != null && v === bestV) td.classList.add("best");
      if (v != null && base != null && spec.delta !== false && i !== baseAt) {
        const d = document.createElement("small");
        d.textContent = cmpShowDelta(spec, v - base);
        td.append(d);
      }
      tr.append(td);
    });
    table.append(tr);
  }
}

function cmpRenderLegend(series) {
  const box = $("cmp-legend");
  box.innerHTML = "";
  for (const s of series) {
    const item = document.createElement("span");
    const sw = document.createElement("i");
    sw.style.background = s.color;
    item.append(sw, document.createTextNode(cmpName(s.id)));
    box.append(item);
  }
}

function cmpRenderResults() {
  const c = cmpCopy();
  const { list, have } = cmpRates();
  const spm = cmpChosenRate(list);
  const sel = $("cmp-rate");
  sel.innerHTML = "";
  for (const e of list) {
    const opt = document.createElement("option");
    opt.value = e.spm;
    opt.textContent = c.rateOption(e.spm, e.k, have);
    sel.append(opt);
  }
  if (spm != null) sel.value = String(spm);
  sel.disabled = !list.length;
  const series = spm == null ? [] : cmpSeries(spm);
  const note = $("cmp-msg");
  let msg = "";
  if (!cmp.sel.length) msg = c.pickOne;
  else if (have && !list.length) msg = c.noCommon;
  else if (cmp.sel.length === 1 && series.length === 1) msg = c.pickTwo;
  else if (spm != null) {
    const lacking = cmp.sel.filter((id) => cmp.data[id] && (cmp.data[id].rates || []).length && !cmpRowAt(id, spm));
    if (lacking.length) msg = c.missing(lacking.map(cmpName).join(", "), spm);
  }
  note.textContent = msg;
  note.hidden = !msg;
  cmpRenderLegend(series);
  cmpRenderTable(spm, series);
  $("cmp-tablebox").hidden = !series.length;
  $("cmp-fine").textContent = c.fine;
  cmpDraw(series, spm);
}

function cmpRenderAll() {
  const c = cmpCopy();
  $("cmp-title").textContent = c.title;
  $("cmp-lead").textContent = c.lead;
  $("cmp-rate-label").textContent = c.rate;
  $("cmp-sessions").textContent = c.sessions;
  $("cmp-full").textContent = c.full;
  $("cmp-chart").setAttribute("aria-label", c.chartAria);
  cmpRenderList();
  cmpRenderResults();
}

function cmpSetup(canvas) {
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
  return { ctx, w, h };
}

function cmpDraw(series, spm) {
  if (!document.body.classList.contains("page-compare")) return;
  if (!series) {
    spm = cmpChosenRate(cmpRates().list);
    series = spm == null ? [] : cmpSeries(spm);
  }
  const canvas = $("cmp-chart");
  const { ctx, w, h } = cmpSetup(canvas);
  if (!series.length) return;
  const box = layoutOf(w, h);
  let t0 = Infinity, t1 = -Infinity;
  const aVals = [], vVals = [];
  for (const s of series) {
    const cv = s.row.curve;
    for (let i = 0; i < cv.t.length; i++) {
      if (cv.t[i] == null) continue;
      t0 = Math.min(t0, cv.t[i]);
      t1 = Math.max(t1, cv.t[i]);
      if (cv.a[i] != null) aVals.push(cv.a[i]);
      if (cv.v[i] != null) vVals.push(cv.v[i]);
    }
  }
  if (!(t1 > t0)) return;
  const X = (t) => box.L + (t - t0) / (t1 - t0) * (box.R - box.L);
  const aRange = rangeOf(aVals) || [-0.6, 0.6];
  if (aRange[0] > 0) aRange[0] = -0.05;
  if (aRange[1] < 0) aRange[1] = 0.05;
  headroom(aRange, box, 14 + series.length * 8);
  const vRange = rangeOf(vVals) || [0, 3];
  const Ya = (g) => box.aB - (g - aRange[0]) / (aRange[1] - aRange[0]) * (box.aB - box.aT);
  const Yv = (v) => box.vB - (v - vRange[0]) / (vRange[1] - vRange[0]) * (box.vB - box.vT);
  const ticks = strokeTicks(t0, t1);

  paintPanels(ctx, box);
  drawGrid(ctx, box, aRange, vRange, ticks.times.map(X), "rate");

  const vline = (t, color, dash, width) => {
    if (t == null || t < t0 || t > t1) return;
    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = width || 1.5;
    if (dash) ctx.setLineDash(dash);
    ctx.beginPath();
    for (const [top, bottom] of [[box.aT, box.aB], [box.vT, box.vB]]) {
      ctx.moveTo(Math.round(X(t)) + 0.5, top);
      ctx.lineTo(Math.round(X(t)) + 0.5, bottom);
    }
    ctx.stroke();
    ctx.restore();
  };
  vline(0, "#1f7a4d");
  for (const s of series) vline(s.row.drive, s.color, [3, 4], 1);
  // When the blade is in the water, one bar per session.
  series.forEach((s, i) => {
    const m = s.row.markers;
    if (!m || m.blade_t0 == null || m.blade_t1 == null) return;
    const y = box.aT + 9 + i * 8;
    ctx.strokeStyle = s.color;
    ctx.lineWidth = 4;
    ctx.lineCap = "butt";
    ctx.beginPath();
    ctx.moveTo(X(m.blade_t0), y);
    ctx.lineTo(X(m.blade_t1), y);
    ctx.stroke();
  });

  const line = (cv, vals, Y, color) => {
    ctx.beginPath();
    let started = false;
    for (let i = 0; i < cv.t.length; i++) {
      if (cv.t[i] == null || vals[i] == null) { started = false; continue; }
      const x = X(cv.t[i]);
      const y = Y(vals[i]);
      if (!started) { ctx.moveTo(x, y); started = true; }
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = color;
    ctx.lineWidth = 2.2;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke();
  };
  for (const s of series) {
    line(s.row.curve, s.row.curve.a, Ya, s.color);
    line(s.row.curve, s.row.curve.v, Yv, s.color);
  }

  const bottom = box.vB;
  ctx.fillStyle = MUTED;
  ctx.font = "11px " + FONT;
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (const t of ticks.times) {
    const x = X(t);
    ctx.strokeStyle = "#d9d2c5";
    ctx.beginPath();
    ctx.moveTo(x, bottom);
    ctx.lineTo(x, bottom + 4);
    ctx.stroke();
    ctx.fillText(t.toFixed(ticks.step < 0.1 ? 2 : 1), x, bottom + 5);
  }

  if (cmp.hover != null && cmp.hover >= t0 && cmp.hover <= t1) {
    const t = cmp.hover;
    vline(t, "rgba(28,25,21,0.45)", null, 1);
    const c = cmpCopy();
    const lines = [{ text: c.time(t), color: "#4a443a" }];
    for (const s of series) {
      const cv = s.row.curve;
      const a = sampleAt(cv.t, cv.a, t);
      const v = sampleAt(cv.t, cv.v, t);
      if (a == null || v == null || !isFinite(a) || !isFinite(v)) continue;
      ctx.fillStyle = s.color;
      for (const [val, Y] of [[a, Ya], [v, Yv]]) {
        ctx.beginPath();
        ctx.arc(X(t), Y(val), 3.5, 0, Math.PI * 2);
        ctx.fill();
      }
      lines.push({ text: cmpName(s.id) + "  " + (a >= 0 ? "+" : "") + a.toFixed(2) + " g · " + v.toFixed(2) + " m/s", color: s.color });
    }
    ctx.font = "12px " + FONT;
    const widest = Math.max(...lines.map((l) => ctx.measureText(l.text).width));
    const bw = widest + 28, bh = lines.length * 17 + 10;
    let bx = X(t) + 14;
    if (bx + bw > box.R) bx = X(t) - 14 - bw;
    const by = box.aT + 6;
    ctx.fillStyle = "rgba(255,255,255,0.94)";
    ctx.strokeStyle = EDGE;
    roundedRect(ctx, bx + 0.5, by + 0.5, bw, bh, 6);
    ctx.fill();
    ctx.stroke();
    ctx.textAlign = "left";
    ctx.textBaseline = "middle";
    lines.forEach((l, i) => {
      const y = by + 5 + i * 17 + 8;
      if (i > 0) {
        ctx.fillStyle = l.color;
        ctx.fillRect(bx + 8, y - 4, 8, 8);
      }
      ctx.fillStyle = i === 0 ? l.color : "#1c1915";
      ctx.fillText(l.text, bx + (i === 0 ? 8 : 22), y);
    });
  }
}

function cmpPointer(e) {
  const canvas = $("cmp-chart");
  const spm = cmpChosenRate(cmpRates().list);
  const series = spm == null ? [] : cmpSeries(spm);
  if (!series.length) return;
  let t0 = Infinity, t1 = -Infinity;
  for (const s of series) {
    for (const t of s.row.curve.t) {
      if (t == null) continue;
      t0 = Math.min(t0, t);
      t1 = Math.max(t1, t);
    }
  }
  const rect = canvas.getBoundingClientRect();
  const box = layoutOf(rect.width, rect.height);
  const u = (e.clientX - rect.left - box.L) / (box.R - box.L);
  cmp.hover = u < 0 || u > 1 ? null : t0 + u * (t1 - t0);
  cmpDraw(series, spm);
}

// Called when the sessions on the server change (after a recording was read in).
function cmpSessions(selectId) {
  if (selectId) {
    cmpForget(selectId);
    if (document.body.classList.contains("page-compare") && !cmp.sel.includes(selectId) && cmp.sel.length < CMP_MAX) {
      cmp.sel.push(selectId);
    }
  }
  cmp.sel = cmp.sel.filter((id) => state.sessions.some((s) => s.id === id));
  for (const id of cmp.sel) cmpEnsure(id);
  cmp.listed = null;
  if (document.body.classList.contains("page-compare")) cmpRenderAll();
}

function cmpShow() {
  if (!cmp.sel.length && state.id && state.sessions.some((s) => s.id === state.id)) cmp.sel.push(state.id);
  for (const id of cmp.sel) cmpEnsure(id);
  cmp.listed = null;
  cmpRenderAll();
  requestAnimationFrame(() => cmpDraw());
}

$("cmp-rate").addEventListener("change", () => {
  cmp.rate = parseInt($("cmp-rate").value, 10);
  cmpRenderResults();
});
$("cmp-chart").addEventListener("pointermove", cmpPointer);
$("cmp-chart").addEventListener("pointerleave", () => { cmp.hover = null; cmpDraw(); });
$("page-compare").addEventListener("click", () => ovShowPage("compare"));
$("lang").addEventListener("change", () => {
  if (!document.body.classList.contains("page-compare")) return;
  cmp.listed = null;
  cmpRenderAll();
});
if (typeof ResizeObserver !== "undefined") {
  let frame = 0;
  new ResizeObserver(() => {
    if (frame || !document.body.classList.contains("page-compare")) return;
    frame = requestAnimationFrame(() => { frame = 0; cmpDraw(); });
  }).observe($("cmp-chart"));
}
