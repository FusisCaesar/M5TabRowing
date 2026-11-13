"use strict";

// Session overview: the same page as the device's Session review. The track on a map coloured
// from slow (red) to fast (green), the session figures, and split, rate and heart rate over time.
// Needs the per-second log (_seconds.csv) next to the recording; without it the page stays hidden.

const OVERVIEW_WORDS = {
  en: {
    pageOverview: "Overview",
    pageStrokes: "Strokes",
    downloadGpx: "Download GPX",
    downloadTcx: "Download TCX",
    toStrava: "Upload to Strava",
  },
  nl: {
    pageOverview: "Overzicht",
    pageStrokes: "Slagen",
    downloadGpx: "Download GPX",
    downloadTcx: "Download TCX",
    toStrava: "Upload naar Strava",
  },
};

const OV_COPY = {
  en: {
    tip: "Download the GPX, then pick it on Strava's upload page. It is marked as rowing and carries stroke rate and heart rate. The TCX also holds the laps.",
    noGpsTip: "There is no GPS track in this session, so there is nothing to upload as a map activity.",
    noGps: "No GPS track in this session. The graphs still show rate and heart rate.",
    noMap: "The map could not be loaded.",
    slow: "Slow",
    fast: "Fast",
    start: "Start",
    finish: "Finish",
    race: "Race",
    course: "course",
    stats: {
      distance: "Distance", time: "Time", avgSplit: "Avg split", best500: "Best 500 m",
      avgRate: "Avg rate", strokes: "Strokes", dps: "Dist/stroke", maxSpeed: "Max speed",
      hr: "HR avg / max", laps: "Laps", avgHr: "Avg HR", maxHr: "Max HR",
    },
    charts: { split: "Split /500 m", rate: "Rate (spm)", hr: "Heart rate (bpm)" },
    lapHead: ["Lap", "Time", "Dist", "Split", "Rate", "Strokes", "HR"],
    strokesWord: "strokes",
  },
  nl: {
    tip: "Download de GPX en kies hem op de uploadpagina van Strava. Hij staat als roeien gemarkeerd en bevat slagtempo en hartslag. De TCX bevat ook de rondes.",
    noGpsTip: "Er is geen GPS-spoor in deze sessie, dus er is niets om als kaartactiviteit te uploaden.",
    noGps: "Geen GPS-spoor in deze sessie. De grafieken tonen nog wel tempo en hartslag.",
    noMap: "De kaart kon niet worden geladen.",
    slow: "Langzaam",
    fast: "Snel",
    start: "Start",
    finish: "Finish",
    race: "Wedstrijd",
    course: "parcours",
    stats: {
      distance: "Afstand", time: "Tijd", avgSplit: "Gem. split", best500: "Beste 500 m",
      avgRate: "Gem. tempo", strokes: "Slagen", dps: "Afstand/slag", maxSpeed: "Max snelheid",
      hr: "HF gem / max", laps: "Rondes", avgHr: "Gem. HF", maxHr: "Max HF",
    },
    charts: { split: "Split /500 m", rate: "Tempo (spm)", hr: "Hartslag (bpm)" },
    lapHead: ["Ronde", "Tijd", "Afst", "Split", "Tempo", "Slagen", "HF"],
    strokesWord: "slagen",
  },
};

Object.assign(CHROME.en, OVERVIEW_WORDS.en);
Object.assign(CHROME.nl, OVERVIEW_WORDS.nl);
applyChrome();

const ov = {
  token: 0,
  data: null,
  page: null,
  map: null,
  layer: null,
  now: null,
  cursor: null,
  frame: 0,
  fitted: false,
};

function ovCopy() { return OV_COPY[lang()]; }

function ovDuration(s) {
  s = Math.max(0, Math.round(s));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return h ? h + ":" + String(m).padStart(2, "0") + ":" + String(sec).padStart(2, "0")
           : m + ":" + String(sec).padStart(2, "0");
}

// Red for the slow end, green for the fast end, grey when stopped: the device's colour scale.
function ovColorOf(v, scale) {
  if (v < 1) return { key: -1, rgb: [140, 140, 140] };
  let t = scale.fast_mps > scale.slow_mps ? (v - scale.slow_mps) / (scale.fast_mps - scale.slow_mps) : 0.5;
  t = Math.max(0, Math.min(1, t));
  const r = t < 0.5 ? 230 : 230 * (1 - t) * 2;
  const g = t < 0.5 ? 200 * t * 2 : 200;
  return { key: Math.round(t * 24), rgb: [r, g, 20] };
}

function ovCss(rgb) { return "rgb(" + rgb.map((x) => Math.round(x)).join(",") + ")"; }

function ovShowPage(page) {
  ov.page = page;
  document.body.classList.toggle("page-overview", page === "overview");
  document.body.classList.toggle("page-strokes", page === "strokes");
  $("page-overview").setAttribute("aria-selected", page === "overview");
  $("page-strokes").setAttribute("aria-selected", page === "strokes");
  if (page === "overview") {
    requestAnimationFrame(() => {
      if (ov.map) {
        ov.map.invalidateSize();
        ovFit();
      }
      ovDrawCharts();
    });
  } else if (state.stroke) {
    requestAnimationFrame(() => resize());
  }
}

function ovFit() {
  if (!ov.map || !ov.bounds) return;
  ov.map.fitBounds(ov.bounds, { paddingTopLeft: [30, 30], paddingBottomRight: [30, 70], maxZoom: 18, animate: false });
}

function ovEnsureMap() {
  if (ov.map || typeof L === "undefined") return;
  ov.map = L.map("map", { preferCanvas: true, zoomSnap: 0.25, attributionControl: true });
  L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 19,
    attribution: '&copy; <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener">OpenStreetMap</a> contributors',
  }).addTo(ov.map);
  ov.map.setView([52.2, 5.1], 12);
}

function ovPoints(d) {
  const pts = [];
  for (let i = 0; i < d.t.length; i++) {
    if (d.lat[i] != null) pts.push(i);
  }
  return pts;
}

function ovDrawTrack(d) {
  const c = ovCopy();
  const box = document.querySelector(".ov-map");
  const idx = ovPoints(d);
  const ok = d.has_gps && idx.length >= 2;
  box.classList.toggle("nogps", !ok || typeof L === "undefined");
  $("map-note").textContent = ok ? c.noMap : c.noGps;
  if (ov.layer && ov.map) {
    ov.map.removeLayer(ov.layer);
    ov.layer = null;
  }
  ov.bounds = null;
  if (ov.now && ov.map) ov.map.removeLayer(ov.now);
  ov.now = null;
  if (!ok || typeof L === "undefined") {
    $("map-legend").innerHTML = "";
    return;
  }
  ovEnsureMap();
  const group = L.layerGroup();
  const renderer = L.canvas({ padding: 0.3 });
  const latlng = (i) => [d.lat[i], d.lon[i]];

  // A white casing keeps the colours readable on any map; then the coloured runs on top.
  const whole = [];
  for (const i of idx) whole.push(latlng(i));
  group.addLayer(L.polyline(whole, { color: "#fff", weight: 8, opacity: 0.9, lineJoin: "round", renderer, interactive: false }));

  let run = null;
  const flush = () => {
    if (run && run.pts.length > 1) {
      group.addLayer(L.polyline(run.pts, { color: ovCss(run.rgb), weight: 4.5, opacity: 1, lineJoin: "round", lineCap: "round", renderer, interactive: false }));
    }
    run = null;
  };
  for (let k = 1; k < idx.length; k++) {
    const a = idx[k - 1];
    const b = idx[k];
    if (b - a > 4) { flush(); continue; }
    const col = ovColorOf(d.speed[b], d.scale);
    if (!run || run.key !== col.key) {
      flush();
      run = { key: col.key, rgb: col.rgb, pts: [latlng(a)] };
    }
    run.pts.push(latlng(b));
  }
  flush();

  const icon = (cls) => L.divIcon({ className: cls, iconSize: [14, 14], iconAnchor: [7, 7] });
  group.addLayer(L.marker(latlng(idx[0]), { icon: icon("mark-start"), interactive: false, keyboard: false }));
  group.addLayer(L.marker(latlng(idx[idx.length - 1]), { icon: icon("mark-end"), interactive: false, keyboard: false }));
  group.addTo(ov.map);
  ov.layer = group;
  ov.bounds = L.latLngBounds(whole);
  ov.nowIcon = icon("mark-now");

  const slow = 500 / d.scale.slow_mps;
  const fast = 500 / d.scale.fast_mps;
  $("map-legend").innerHTML =
    '<div class="ends"><span>' + c.slow + " <b>" + splitText(slow) + "</b></span><span>" + c.fast + " <b>" + splitText(fast) + "</b></span></div>" +
    '<div class="bar2"></div>' +
    '<div class="keys"><span><i class="start"></i>' + c.start + '</span><span><i class="end"></i>' + c.finish + "</span></div>";
}

function ovStat(label, value, unit, none) {
  return '<div class="stat"><div class="k">' + label + '</div><div class="v' + (none ? " dim" : "") + '">' +
    value + (unit && !none ? "<small>" + unit + "</small>" : "") + "</div></div>";
}

function ovDrawStats(d) {
  const c = ovCopy();
  const s = d.stats;
  const nf = (x, digits) => x.toLocaleString(lang(), { minimumFractionDigits: digits, maximumFractionDigits: digits, useGrouping: false });
  const cells = [];
  const dash = "—";
  const add = (label, value, unit, none) => cells.push(ovStat(c.stats[label], none ? dash : value, unit, none));
  if (d.has_gps) {
    add("distance", nf(s.distance_m, 0), "m", s.distance_m <= 0);
    add("time", ovDuration(s.duration_s), "", false);
    add("avgSplit", splitText(s.avg_split_s), "/500m", s.avg_split_s <= 0);
    add("best500", splitText(s.best_500_s), "/500m", s.best_500_s <= 0);
    add("avgRate", nf(s.avg_rate, 1), "spm", s.avg_rate <= 0);
    add("strokes", nf(s.strokes, 0), "", s.strokes <= 0);
    add("dps", nf(s.dps_m, 1), "m", s.dps_m <= 0);
    add("maxSpeed", nf(s.max_speed_mps, 1), "m/s", s.max_speed_mps <= 0);
    if (s.hr_avg > 0) add("hr", Math.round(s.hr_avg) + " / " + Math.round(s.hr_max), "bpm", false);
  } else {
    add("time", ovDuration(s.duration_s), "", false);
    add("avgRate", nf(s.avg_rate, 1), "spm", s.avg_rate <= 0);
    add("strokes", nf(s.strokes, 0), "", s.strokes <= 0);
    add("laps", String(d.laps.length), "", false);
    if (s.hr_avg > 0) {
      add("avgHr", String(Math.round(s.hr_avg)), "bpm", false);
      add("maxHr", String(Math.round(s.hr_max)), "bpm", false);
    }
  }
  while (cells.length % 3) cells.push('<div class="stat"></div>');
  $("ov-stats").innerHTML = cells.join("");
}

function ovDrawLaps(d) {
  const box = $("ov-laps");
  const c = ovCopy();
  if (d.laps.length < 2) {
    box.hidden = true;
    return;
  }
  const rows = d.laps.map((l) =>
    "<tr><td>" + l.lap + "</td><td>" + ovDuration(l.time_s) + "</td><td>" + Math.round(l.distance_m) + " m</td><td>" +
    splitText(l.split_s) + "</td><td>" + (l.rate ? l.rate.toFixed(1) : "—") + "</td><td>" + l.strokes + "</td><td>" +
    (l.hr ? Math.round(l.hr) : "—") + "</td></tr>").join("");
  box.innerHTML = "<table><thead><tr>" + c.lapHead.map((h) => "<th>" + h + "</th>").join("") + "</tr></thead><tbody>" + rows + "</tbody></table>";
  box.hidden = false;
}

function ovDrawHead(d) {
  const c = ovCopy();
  const info = state.sessions.find((s) => s.id === d.id);
  $("ov-title").textContent = info ? info.name : d.id;
  const when = new Date(d.start_utc);
  let sub = isNaN(when) ? "" : when.toLocaleString(lang(), { dateStyle: "medium", timeStyle: "short" });
  if (d.race) {
    $("ov-badge").textContent = c.race + (d.race.segment ? " · " + d.race.segment : "");
    sub += (sub ? " · " : "") + (d.race.course_m ? Math.round(d.race.course_m) + " m " + c.course : "");
  } else {
    $("ov-badge").textContent = "";
  }
  $("ov-sub").textContent = sub;
  const id = encodeURIComponent(d.id);
  $("dl-gpx").href = "/api/export?id=" + id + "&format=gpx";
  $("dl-tcx").href = "/api/export?id=" + id + "&format=tcx";
  const gps = d.has_gps;
  $("dl-gpx").hidden = !gps;
  $("dl-tcx").hidden = !d.has_seconds;
  $("strava").hidden = !gps;
  $("ov-tip").textContent = gps ? c.tip : c.noGpsTip;
}

// ---- curves ----

function ovNiceStep(span) {
  for (const step of [30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 14400]) {
    if (span / step <= 7) return step;
  }
  return 21600;
}

function ovSeries(d, key) {
  if (key === "split") return d.split.map((x) => (x != null && x <= 360 ? x : null));
  if (key === "rate") return d.rate.map((x) => (x > 0 ? x : null));
  return d.hr;
}

function ovRange(values, key) {
  const v = values.filter((x) => x != null).sort((a, b) => a - b);
  if (!v.length) return null;
  const at = (p) => v[Math.min(v.length - 1, Math.floor(v.length * p))];
  if (key === "split") {
    const lo = Math.max(60, Math.floor(at(0.01) / 5) * 5);
    const hi = Math.min(420, Math.max(lo + 20, Math.ceil(at(0.99) / 5) * 5));
    return [lo, hi];
  }
  const step = key === "hr" ? 10 : 5;
  let lo = Math.floor((v[0] - 1) / step) * step;
  let hi = Math.ceil((v[v.length - 1] + 1) / step) * step;
  if (key === "rate") lo = Math.max(0, lo);
  if (hi - lo < step * 2) hi = lo + step * 2;
  return [lo, hi];
}

function ovFormat(key, x) {
  if (x == null) return "—";
  if (key === "split") return splitText(x);
  return String(Math.round(x));
}

function ovDrawChart(box, key, last) {
  const canvas = box.querySelector("canvas");
  const d = ov.data;
  if (!d) return;
  const dpr = window.devicePixelRatio || 1;
  const w = box.clientWidth - 20;
  const h = box.clientHeight - 4;
  if (w < 40 || h < 30) return;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const values = ovSeries(d, key);
  const range = ovRange(values, key);
  const c = ovCopy();
  const left = 42;
  const right = 6;
  const top = 20;
  const bottom = last ? 18 : 6;
  const pw = w - left - right;
  const ph = h - top - bottom;
  const t1 = Math.max(1, d.t[d.t.length - 1]);
  const xOf = (t) => left + (t / t1) * pw;
  const color = key === "split" ? "#1d4e89" : key === "rate" ? "#1f7a4d" : "#c4552a";

  ctx.font = "600 11px " + FONT;
  ctx.fillStyle = MUTED;
  ctx.textBaseline = "top";
  ctx.textAlign = "left";
  ctx.fillText(c.charts[key].toUpperCase(), 0, 2);

  // grid: time ticks and three value lines
  const step = ovNiceStep(t1);
  ctx.strokeStyle = GRID;
  ctx.lineWidth = 1;
  ctx.textAlign = "center";
  ctx.font = "11px " + FONT;
  for (let t = 0; t <= t1; t += step) {
    const x = Math.round(xOf(t)) + 0.5;
    ctx.beginPath();
    ctx.moveTo(x, top);
    ctx.lineTo(x, top + ph);
    ctx.stroke();
    if (last) {
      ctx.fillStyle = MUTED;
      ctx.textBaseline = "top";
      ctx.fillText(ovDuration(t), Math.min(w - 12, Math.max(left, x)), top + ph + 4);
    }
  }
  if (!range) return;
  const [lo, hi] = range;
  const inverted = key === "split";
  const yOf = (x) => {
    const f = (Math.max(lo, Math.min(hi, x)) - lo) / (hi - lo);
    return top + ph * (inverted ? f : 1 - f);
  };
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (const f of [0, 0.5, 1]) {
    const y = Math.round(top + ph * f) + 0.5;
    ctx.strokeStyle = f === 1 ? ZERO : GRID;
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(left + pw, y);
    ctx.stroke();
    const val = inverted ? lo + (hi - lo) * f : hi - (hi - lo) * f;
    ctx.fillStyle = MUTED;
    ctx.fillText(ovFormat(key, val), left - 6, y);
  }

  // the curve, broken where there is no reading
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.75;
  ctx.lineJoin = "round";
  ctx.beginPath();
  let pen = false;
  for (let i = 0; i < values.length; i++) {
    if (values[i] == null) { pen = false; continue; }
    const x = xOf(d.t[i]);
    const y = yOf(values[i]);
    if (pen) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    pen = true;
  }
  ctx.stroke();

  if (ov.cursor != null) {
    const i = ov.cursor;
    const x = Math.round(xOf(d.t[i])) + 0.5;
    ctx.strokeStyle = "#1c1915";
    ctx.globalAlpha = 0.45;
    ctx.beginPath();
    ctx.moveTo(x, top);
    ctx.lineTo(x, top + ph);
    ctx.stroke();
    ctx.globalAlpha = 1;
    const v = values[i];
    if (v != null) {
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(xOf(d.t[i]), yOf(v), 3.5, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.textAlign = "right";
    ctx.textBaseline = "top";
    ctx.font = "650 12px " + FONT;
    ctx.fillStyle = "#1c1915";
    ctx.fillText(ovDuration(d.t[i]) + "   " + ovFormat(key, v), w - right, 2);
  }
}

function ovDrawCharts() {
  const d = ov.data;
  if (!d || ov.page !== "overview") return;
  const boxes = [...document.querySelectorAll(".ov-chart")];
  for (const box of boxes) {
    const key = box.dataset.key;
    box.hidden = !ovRange(ovSeries(d, key), key);
  }
  const shown = boxes.filter((b) => !b.hidden);
  $("ov-charts").hidden = shown.length === 0;
  shown.forEach((box, n) => ovDrawChart(box, box.dataset.key, n === shown.length - 1));
}

function ovSchedule() {
  if (ov.frame) return;
  ov.frame = requestAnimationFrame(() => {
    ov.frame = 0;
    ovDrawCharts();
  });
}

function ovMoveMarker(i) {
  const d = ov.data;
  if (!ov.map || !d || !ov.layer) return;
  let j = i;
  while (j > 0 && d.lat[j] == null) j--;
  if (d.lat[j] == null) return;
  const at = [d.lat[j], d.lon[j]];
  if (!ov.now) ov.now = L.marker(at, { icon: ov.nowIcon, interactive: false, keyboard: false, zIndexOffset: 1000 }).addTo(ov.map);
  else ov.now.setLatLng(at);
}

function ovHover(box, event) {
  const d = ov.data;
  if (!d) return;
  const rect = box.querySelector("canvas").getBoundingClientRect();
  const left = 42;
  const pw = rect.width - left - 6;
  const f = Math.max(0, Math.min(1, (event.clientX - rect.left - left) / pw));
  const t = f * d.t[d.t.length - 1];
  let lo = 0;
  let hi = d.t.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (d.t[mid] < t) lo = mid + 1; else hi = mid;
  }
  ov.cursor = lo;
  ovMoveMarker(lo);
  ovSchedule();
}

function ovLeave() {
  ov.cursor = null;
  if (ov.now && ov.map) {
    ov.map.removeLayer(ov.now);
    ov.now = null;
  }
  ovSchedule();
}

function ovRender(d) {
  ov.data = d;
  ov.cursor = null;
  ovDrawHead(d);
  ovDrawStats(d);
  ovDrawLaps(d);
  ovDrawTrack(d);
  ovDrawCharts();
}

async function overviewLoad(id) {
  const mine = ++ov.token;
  let d = null;
  try {
    d = await getJSON("/api/overview?id=" + encodeURIComponent(id));
  } catch (err) {
    d = null;
  }
  if (mine !== ov.token) return;
  if (!d || !d.has_seconds) {
    ov.data = null;
    $("pages").hidden = true;
    $("overview").hidden = true;
    ovShowPage("strokes");
    return;
  }
  $("pages").hidden = false;
  $("overview").hidden = false;
  ovRender(d);
  ovShowPage(ov.page || "overview");
}

for (const box of document.querySelectorAll(".ov-chart")) {
  const canvas = box.querySelector("canvas");
  canvas.addEventListener("pointermove", (e) => ovHover(box, e));
  canvas.addEventListener("pointerleave", ovLeave);
}
$("page-overview").addEventListener("click", () => ovShowPage("overview"));
$("page-strokes").addEventListener("click", () => ovShowPage("strokes"));
$("lang").addEventListener("change", () => { if (ov.data) ovRender(ov.data); });
if (typeof ResizeObserver !== "undefined") {
  new ResizeObserver(() => ovSchedule()).observe($("ov-charts"));
}
