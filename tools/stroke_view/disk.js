"use strict";

// Disk page: what the server has stored, and a way to remove it. It sits behind a code that the server
// checks; the page only keeps the token the server hands back, for this browser tab.

Object.assign(CHROME.en, { pageDisk: "Disk" });
Object.assign(CHROME.nl, { pageDisk: "Schijf" });
applyChrome();

const DISK_COPY = {
  en: {
    title: "Disk management",
    lead: "See what this server has stored and remove what you no longer need. Removed recordings can be read from the tablet again.",
    code: "Code",
    unlock: "Unlock",
    checking: "Checking…",
    expired: "The code has expired. Enter it again.",
    used: "Recordings",
    free: "Free",
    of: "of",
    path: "Stored in",
    transient: "This storage is not persistent here: everything on it is lost when the server restarts.",
    sessions: "Sessions",
    none: "Nothing is stored.",
    name: "Session",
    size: "Size",
    saved: "Saved",
    files: "Files",
    cached: "open in memory",
    other: "Other files",
    selectAll: "Select all",
    selectNone: "Select none",
    remove: (n) => n ? "Delete " + n + " selected" : "Delete selected",
    refresh: "Refresh",
    lock: "Lock",
    rename: "Rename",
    renameAsk: "Name for this session (leave empty to go back to the file name):",
    ask: (n, size) => "Delete " + n + (n === 1 ? " item" : " items") + " (" + size + ") from the server? This cannot be undone.",
    done: (n) => n + (n === 1 ? " item deleted." : " items deleted."),
    failed: (n) => n + " could not be deleted.",
    loading: "Loading…",
  },
  nl: {
    title: "Schijfbeheer",
    lead: "Zie wat deze server heeft opgeslagen en verwijder wat je niet meer nodig hebt. Verwijderde opnames kun je opnieuw van de tablet lezen.",
    code: "Code",
    unlock: "Ontgrendel",
    checking: "Controleren…",
    expired: "De code is verlopen. Voer hem opnieuw in.",
    used: "Opnames",
    free: "Vrij",
    of: "van",
    path: "Opgeslagen in",
    transient: "Deze opslag is hier niet blijvend: alles erop is weg als de server herstart.",
    sessions: "Sessies",
    none: "Er is niets opgeslagen.",
    name: "Sessie",
    size: "Grootte",
    saved: "Bewaard",
    files: "Bestanden",
    cached: "open in het geheugen",
    other: "Andere bestanden",
    selectAll: "Selecteer alles",
    selectNone: "Selecteer niets",
    remove: (n) => n ? "Verwijder " + n + " geselecteerd" : "Verwijder selectie",
    refresh: "Ververs",
    lock: "Vergrendel",
    rename: "Hernoem",
    renameAsk: "Naam voor deze sessie (leeg laten voor de bestandsnaam):",
    ask: (n, size) => "Verwijder " + n + (n === 1 ? " item" : " items") + " (" + size + ") van de server? Dit kan niet ongedaan worden gemaakt.",
    done: (n) => n + (n === 1 ? " item verwijderd." : " items verwijderd."),
    failed: (n) => n + " konden niet worden verwijderd.",
    loading: "Laden…",
  },
};

const disk = { token: null, data: null, picked: new Set(), message: "", busy: false };
try { disk.token = sessionStorage.getItem("stroke-admin"); } catch (err) { /* storage blocked */ }

function diskCopy() { return DISK_COPY[lang()]; }

function diskBytes(n) {
  if (n == null) return "?";
  if (n >= 1024 * 1024 * 1024) return (n / (1024 * 1024 * 1024)).toFixed(2) + " GB";
  if (n >= 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + " MB";
  if (n >= 1024) return Math.round(n / 1024) + " KB";
  return n + " B";
}

function diskDate(seconds) {
  return new Date(seconds * 1000).toLocaleString(lang() === "nl" ? "nl-NL" : "en-GB", { dateStyle: "medium", timeStyle: "short" });
}

function diskEsc(text) {
  return String(text).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

// One call to the server. Resolves to { status, data } so a wrong code and a lock can be told apart.
async function diskCall(url, body) {
  const headers = { "X-Admin-Token": disk.token || "" };
  const options = { headers };
  if (body !== undefined) {
    options.method = "POST";
    headers["Content-Type"] = "application/json";
    options.body = JSON.stringify(body);
  }
  const res = await fetch(url, options);
  let data = {};
  try { data = await res.json(); } catch (err) { /* not JSON */ }
  return { status: res.status, data };
}

function diskSetToken(token) {
  disk.token = token;
  try {
    if (token) sessionStorage.setItem("stroke-admin", token);
    else sessionStorage.removeItem("stroke-admin");
  } catch (err) { /* storage blocked */ }
}

function diskLocked(message) {
  diskSetToken(null);
  disk.data = null;
  disk.picked.clear();
  disk.message = message || "";
  diskRender();
}

async function diskLoad() {
  if (!disk.token) return;
  disk.busy = true;
  diskRender();
  try {
    const { status, data } = await diskCall("/api/admin/storage");
    if (status === 401) return diskLocked(diskCopy().expired);
    if (status !== 200) throw new Error(data.error || "error " + status);
    disk.data = data;
    const present = new Set(data.sessions.map((s) => s.id).concat(data.other.map((f) => f.name)));
    for (const id of [...disk.picked]) if (!present.has(id)) disk.picked.delete(id);
  } catch (err) {
    disk.message = err.message;
  } finally {
    disk.busy = false;
    diskRender();
  }
}

async function diskUnlock(pin) {
  const copy = diskCopy();
  disk.message = copy.checking;
  diskRender();
  try {
    const { status, data } = await diskCall("/api/admin/login", { pin });
    if (status === 200) {
      diskSetToken(data.token);
      disk.message = "";
      await diskLoad();
      return;
    }
    disk.message = data.error || "error " + status;
  } catch (err) {
    disk.message = err.message;
  }
  diskRender();
}

// After the disk changed, the session list and anything the page kept about a session follow.
async function diskSync(ids) {
  if (typeof cmpForget === "function") for (const id of ids) cmpForget(id);
  if (typeof loadSessions === "function") {
    try { await loadSessions($("session").value); } catch (err) { /* the list shows on the next visit */ }
  }
}

async function diskRename(id) {
  const row = disk.data && disk.data.sessions.find((s) => s.id === id);
  if (!row) return;
  const name = window.prompt(diskCopy().renameAsk, row.name || row.id);
  if (name === null || name.trim() === (row.name || row.id)) return;
  disk.busy = true;
  diskRender();
  try {
    const { status, data } = await diskCall("/api/admin/rename", { id, name });
    if (status === 401) return diskLocked(diskCopy().expired);
    if (status !== 200) throw new Error(data.error || "error " + status);
    disk.message = "";
  } catch (err) {
    disk.message = err.message;
  }
  await diskLoad();
  await diskSync([id]);
}

async function diskDelete() {
  const d = disk.data;
  if (!d || !disk.picked.size) return;
  const copy = diskCopy();
  const ids = d.sessions.filter((s) => disk.picked.has(s.id));
  const files = d.other.filter((f) => disk.picked.has(f.name));
  const bytes = ids.reduce((sum, s) => sum + s.bytes, 0) + files.reduce((sum, f) => sum + f.bytes, 0);
  if (!window.confirm(copy.ask(ids.length + files.length, diskBytes(bytes)))) return;
  disk.busy = true;
  diskRender();
  try {
    const { status, data } = await diskCall("/api/admin/delete", { ids: ids.map((s) => s.id), files: files.map((f) => f.name) });
    if (status === 401) return diskLocked(copy.expired);
    if (status !== 200) throw new Error(data.error || "error " + status);
    disk.picked.clear();
    disk.message = copy.done(data.deleted.length) + (data.failed.length ? " " + copy.failed(data.failed.length) : "");
  } catch (err) {
    disk.message = err.message;
  }
  await diskLoad();
  await diskSync(ids.map((s) => s.id));
}

function diskRender() {
  const root = $("disk");
  if (!root) return;
  const copy = diskCopy();
  const head = "<h2>" + copy.title + "</h2><p class=\"disk-lead\">" + copy.lead + "</p>";
  if (!disk.token || !disk.data) {
    if (disk.token && disk.busy) {
      root.innerHTML = "<div class=\"card disk-card\">" + head + "<p class=\"disk-msg\">" + copy.loading + "</p></div>";
      return;
    }
    root.innerHTML = "<div class=\"card disk-card disk-lock\">" + head
      + "<form id=\"disk-form\"><label><span>" + copy.code + "</span>"
      + "<input id=\"disk-pin\" type=\"password\" inputmode=\"numeric\" autocomplete=\"off\" maxlength=\"32\"></label>"
      + "<button type=\"submit\">" + copy.unlock + "</button></form>"
      + "<p class=\"disk-msg\" role=\"status\">" + diskEsc(disk.message) + "</p></div>";
    $("disk-form").addEventListener("submit", (e) => {
      e.preventDefault();
      const pin = $("disk-pin").value.trim();
      if (pin) diskUnlock(pin);
    });
    return;
  }
  const d = disk.data;
  const used = d.total ? Math.min(100, ((d.total - d.free) / d.total) * 100) : 0;
  const mine = d.total ? Math.min(100, (d.kept / d.total) * 100) : 0;
  const rows = d.sessions.map((s) => "<tr><td><label class=\"disk-tick\"><input type=\"checkbox\" data-pick=\"" + diskEsc(s.id) + "\""
    + (disk.picked.has(s.id) ? " checked" : "") + "><b>" + diskEsc(s.name || s.id) + "</b></label>"
    + (s.name || s.cached ? "<small>" + diskEsc((s.name ? s.id : "") + (s.name && s.cached ? " · " : "") + (s.cached ? copy.cached : "")) + "</small>" : "") + "</td>"
    + "<td class=\"num\">" + diskBytes(s.bytes) + "</td><td>" + diskDate(s.saved) + "</td>"
    + "<td>" + diskEsc(s.files.join(", ")) + "</td>"
    + "<td class=\"num\"><button type=\"button\" class=\"quiet disk-rename\" data-rename=\"" + diskEsc(s.id) + "\">" + copy.rename + "</button></td></tr>").join("");
  const loose = d.other.map((f) => "<tr><td><label class=\"disk-tick\"><input type=\"checkbox\" data-pick=\"" + diskEsc(f.name) + "\""
    + (disk.picked.has(f.name) ? " checked" : "") + "><b>" + diskEsc(f.name) + "</b></label></td>"
    + "<td class=\"num\">" + diskBytes(f.bytes) + "</td><td>" + diskDate(f.saved) + "</td><td></td><td></td></tr>").join("");
  const all = d.sessions.length + d.other.length;
  root.innerHTML = "<div class=\"card disk-card\">" + head
    + "<div class=\"disk-bar\" role=\"img\" aria-label=\"" + diskBytes(d.total - d.free) + " " + copy.of + " " + diskBytes(d.total) + "\">"
    + "<i class=\"all\" style=\"width:" + used.toFixed(1) + "%\"></i><i class=\"mine\" style=\"width:" + mine.toFixed(1) + "%\"></i></div>"
    + "<p class=\"disk-stats\"><span>" + copy.used + " <b>" + diskBytes(d.kept) + "</b></span>"
    + "<span>" + copy.free + " <b>" + diskBytes(d.free) + "</b> " + copy.of + " " + diskBytes(d.total) + "</span></p>"
    + "<p class=\"disk-path\">" + copy.path + " <code>" + diskEsc(d.path) + "</code></p>"
    + (d.persistent ? "" : "<p class=\"disk-warn\">" + copy.transient + "</p>")
    + "<div class=\"disk-actions\">"
    + "<button type=\"button\" id=\"disk-all\" class=\"quiet\">" + (disk.picked.size === all && all ? copy.selectNone : copy.selectAll) + "</button>"
    + "<button type=\"button\" id=\"disk-del\" class=\"danger\"" + (disk.picked.size && !disk.busy ? "" : " disabled") + ">" + copy.remove(disk.picked.size) + "</button>"
    + "<button type=\"button\" id=\"disk-refresh\" class=\"quiet\"" + (disk.busy ? " disabled" : "") + ">" + copy.refresh + "</button>"
    + "<button type=\"button\" id=\"disk-lock\" class=\"quiet\">" + copy.lock + "</button></div>"
    + "<p class=\"disk-msg\" role=\"status\">" + diskEsc(disk.message) + "</p>"
    + (all ? "<div class=\"disk-scroll\"><table id=\"disk-table\"><thead><tr><th>" + copy.name + "</th><th class=\"num\">" + copy.size
      + "</th><th>" + copy.saved + "</th><th>" + copy.files + "</th><th></th></tr></thead><tbody>" + rows
      + (loose ? "<tr class=\"disk-sep\"><td colspan=\"5\">" + copy.other + "</td></tr>" + loose : "") + "</tbody></table></div>"
      : "<p class=\"disk-msg\">" + copy.none + "</p>")
    + "</div>";
  for (const box of root.querySelectorAll("input[data-pick]")) {
    box.addEventListener("change", () => {
      if (box.checked) disk.picked.add(box.dataset.pick);
      else disk.picked.delete(box.dataset.pick);
      diskRender();
    });
  }
  for (const button of root.querySelectorAll("button[data-rename]")) {
    button.addEventListener("click", () => diskRename(button.dataset.rename));
  }
  $("disk-all").addEventListener("click", () => {
    if (disk.picked.size === all) disk.picked.clear();
    else for (const id of d.sessions.map((s) => s.id).concat(d.other.map((f) => f.name))) disk.picked.add(id);
    diskRender();
  });
  $("disk-del").addEventListener("click", diskDelete);
  $("disk-refresh").addEventListener("click", () => { disk.message = ""; diskLoad(); });
  $("disk-lock").addEventListener("click", async () => {
    try { await diskCall("/api/admin/lock", {}); } catch (err) { /* the token expires anyway */ }
    diskLocked("");
  });
}

function diskShow() {
  disk.message = disk.data ? disk.message : "";
  diskRender();
  if (disk.token) diskLoad();
  else requestAnimationFrame(() => { const pin = $("disk-pin"); if (pin) pin.focus(); });
}

$("lang").addEventListener("change", () => {
  if (document.body.classList.contains("page-disk")) diskRender();
});
