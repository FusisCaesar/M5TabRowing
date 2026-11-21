"use strict";

// Reads a session off the Tab5 over USB, with the same exchange as tools/fetch.py,
// then hands the files to the viewer. The browser does this itself: the server
// on Render cannot see a tablet plugged into this computer.
// A Chromium browser is required. Opening the port can restart the tablet.

const TAB5_CHUNK = 4096;
const TAB5_MAX = 80 * 1024 * 1024;
const FILE_LINE = /\[files\]\s+(?:.*\/)?(S(\d{4})_[\w-]+\.csv)\s+(\d+)\s+bytes/;

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(data) {
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function sessionsFromLines(lines) {
  const sessions = new Map();
  for (const line of lines) {
    const m = FILE_LINE.exec(line);
    if (!m) continue;
    const id = "S" + m[2];
    let session = sessions.get(id);
    if (!session) {
      const stem = m[1].replace(/_(imu|info|strokes|seconds|laps|marks)\.csv$/, "").replace(/_/g, " ");
      session = { id, name: stem || id, files: [] };
      sessions.set(id, session);
    }
    session.files.push({ name: m[1], size: Number(m[3]) });
  }
  // Session numbers only go up, so the highest is the newest.
  return [...sessions.values()].sort((a, b) => b.id.localeCompare(a.id));
}

function megabytes(n) {
  return (n / 1e6).toFixed(n >= 1e6 ? 1 : 2) + " MB";
}

class Tab5Port {
  constructor(port) {
    this.port = port;
    this.reader = null;
    this.writer = null;
    this.pending = null;
    this.buf = new Uint8Array(0);
    this.text = new TextEncoder();
    this.queue = Promise.resolve();
  }

  // One exchange at a time on the port: reading the dates in the background must not run into a download.
  exclusive(task) {
    const run = this.queue.then(task, task);
    this.queue = run.catch(() => {});
    return run;
  }

  async open() {
    await this.port.open({ baudRate: 115200, bufferSize: 1 << 20 });
    // Held low so a later close does not pulse the lines. The open itself can
    // still restart the tablet; the browser sets these lines as it connects.
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
  }

  async close() {
    this.closing = true;
    try { if (this.reader) this.reader.releaseLock(); } catch (e) { /* already released */ }
    try { if (this.writer) this.writer.releaseLock(); } catch (e) { /* already released */ }
    this.reader = this.writer = null;
    try { await this.port.close(); } catch (e) { /* port already gone */ }
  }

  async write(bytes) {
    await this.writer.write(bytes);
  }

  // One read stays in flight. A timeout leaves it there, so the next wait
  // collects the same bytes instead of asking the port to read twice.
  async _take(deadline, recoveries = 0) {
    if (this.closing || !this.reader) throw new Error("the tablet closed the connection");
    if (!this.pending) this.pending = this.reader.read();
    const left = deadline - performance.now();
    if (left <= 0) return false;
    let timed;
    try {
      timed = await Promise.race([
        this.pending.then((value) => ({ value })),
        new Promise((resolve) => setTimeout(() => resolve(null), left)),
      ]);
    } catch (err) {
      // A read that failed (a buffer overrun, a break signalled on the line) leaves this reader broken:
      // every later read fails at once. Take a fresh one from the port and carry on; whatever was lost
      // shows up as a bad chunk and is asked for again.
      this.pending = null;
      this.lastError = (err && err.name ? err.name : "error") + (err && err.message ? ": " + err.message : "");
      if (this.closing || recoveries >= 3) throw err;
      await this._recover();
      return this._take(deadline, recoveries + 1);
    }
    if (timed === null) return false;
    this.pending = null;
    if (timed.value.done) throw new Error("the tablet closed the connection");
    const chunk = timed.value.value;
    const next = new Uint8Array(this.buf.length + chunk.length);
    next.set(this.buf);
    next.set(chunk, this.buf.length);
    this.buf = next;
    return true;
  }

  async _recover() {
    this.pending = null;
    try { if (this.reader) this.reader.releaseLock(); } catch (e) { /* already released */ }
    this.reader = null;
    const until = performance.now() + 2000;
    while (!this.port.readable && performance.now() < until) await new Promise((resolve) => setTimeout(resolve, 50));
    if (!this.port.readable) throw new Error("the USB connection to the tablet was lost");
    this.reader = this.port.readable.getReader();
    this.buf = new Uint8Array(0);
  }

  async readExact(n, timeoutMs) {
    const deadline = performance.now() + timeoutMs;
    while (this.buf.length < n) {
      if (!await this._take(deadline)) throw new Error("the tablet stopped sending");
    }
    const out = this.buf.slice(0, n);
    this.buf = this.buf.slice(n);
    return out;
  }

  // Waits until the tablet has been quiet for quietMs and drops everything that arrived.
  async drain(quietMs) {
    for (;;) {
      this.buf = new Uint8Array(0);
      if (!await this._take(performance.now() + quietMs)) break;
    }
    this.buf = new Uint8Array(0);
  }

  async readLine(timeoutMs) {
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      const i = this.buf.indexOf(10);
      if (i >= 0) {
        const line = new TextDecoder().decode(this.buf.slice(0, i)).replace(/\r$/, "");
        this.buf = this.buf.slice(i + 1);
        return line;
      }
      if (!await this._take(deadline)) return null;
    }
  }

  // Asks for the listing. Newer firmware ends it with "[files] end"; older firmware just stops, so a
  // long silence ends it too. The first line can take a while (the card counts its free space), and a
  // tablet that is still starting up after the port opened does not hear the request: ask again.
  list() {
    return this.exclusive(() => this._list());
  }

  async _list() {
    const lines = [];
    for (let attempt = 0; attempt < 3; attempt++) {
      this.buf = new Uint8Array(0);
      await this.write(this.text.encode("D"));
      const deadline = performance.now() + 60000;
      let quiet = performance.now() + (attempt < 2 ? 5000 : 12000);
      let started = false;
      for (;;) {
        const now = performance.now();
        if (now >= deadline) break;
        const line = await this.readLine(Math.max(50, Math.min(quiet, deadline) - now));
        if (line === null) {
          if (performance.now() >= quiet) break;
          continue;
        }
        if (line.startsWith("[files]")) {
          if (!started) lines.length = 0;
          started = true;
          quiet = performance.now() + 3000;
          if (/^\[files\]\s+end\b/.test(line)) break;
        }
        if (started) lines.push(line);
      }
      if (started) break;
    }
    if (lines.some((line) => line.startsWith("[files] no SD"))) throw new Error("the tablet has no SD card");
    return sessionsFromLines(lines);
  }

  fetchFile(name, onProgress) {
    return this.exclusive(() => this._fetchFile(name, onProgress));
  }

  async _fetchFile(name, onProgress) {
    this.buf = new Uint8Array(0);
    await this.write(this.text.encode("B /rowing/" + name + "\n"));
    let size = null;
    const deadline = performance.now() + 8000;
    while (performance.now() < deadline) {
      const line = await this.readLine(8000);
      if (line === null) break;
      if (line.startsWith("[fetch] ERR")) throw new Error(line.replace("[fetch] ERR ", ""));
      if (line.startsWith("[fetch] SIZE")) {
        size = Number(line.split(/\s+/)[2]);
        break;
      }
    }
    if (size == null || !Number.isFinite(size)) throw new Error("the tablet did not start the file");
    const parts = [];
    let got = 0;
    try {
      while (got < size) {
        const want = Math.min(TAB5_CHUNK, size - got);
        let chunk = null;
        for (let attempt = 0; attempt < 30; attempt++) {
          if (this.closing) throw new Error("the tablet closed the connection");
          // After a bad chunk, let whatever the tablet is still sending arrive and throw it
          // away. Left in the buffer, stray bytes (a log line the tablet printed in the middle
          // of the chunk) shift every later read, and the retries can never line up again.
          if (attempt > 0) await this.drain(300);
          await this.write(this.text.encode(attempt === 0 ? "N" : "R"));
          let raw = null;
          try {
            raw = await this.readExact(want + 4, attempt === 0 ? 10000 : 4000);
          } catch (err) {
            if (/closed the connection/.test(err.message)) throw err;
            continue;  // bytes went missing: ask again
          }
          const body = raw.slice(0, want);
          const crc = raw[want] | (raw[want + 1] << 8) | (raw[want + 2] << 16) | (raw[want + 3] << 24);
          if ((crc >>> 0) === crc32(body)) {
            chunk = body;
            break;
          }
        }
        if (!chunk) {
          throw new Error("too many bad chunks at byte " + got + " of " + size + (this.lastError ? " (" + this.lastError + ")" : ""));
        }
        parts.push(chunk);
        got += want;
        if (onProgress) onProgress(got, size);
      }
    } finally {
      try { await this.write(this.text.encode("Q")); } catch (e) { /* the tablet already moved on */ }
    }
    return new Blob(parts, { type: "text/csv" });
  }
}

const DEVICE_COPY = {
  en: {
    unsupported: "This browser cannot read a USB device. Use Chrome or Edge.",
    cancelled: "",
    title: "Recording on the Tab5",
    note: "Stop the session on the tablet first. Connecting can restart it. A recording over 80 MB is too big for this server.",
    empty: "No recordings on the card.",
    get: "Get recording",
    close: "Close",
    reading: "Reading the tablet…",
    fetching: (name, pct) => name + "  " + pct + "%",
    tooBig: (name, size) => name + " is " + size + ", over the 80 MB this server can take.",
    recording: "the tablet is recording; stop it first",
    stageRead: (i, n, name, pct) => "Reading from the tablet (" + i + "/" + n + "): " + name + "  " + pct + "%",
    stageSend: (pct) => "Sending to the server  " + pct + "%",
    stageAnalyse: (s) => "Analysing the recording… " + s + " s. A long one takes a minute or two.",
    uploadFailed: "The upload failed: the server did not answer. Press Get recording to try again; the tablet is not read twice.",
  },
  nl: {
    unsupported: "Deze browser kan geen USB-apparaat lezen. Gebruik Chrome of Edge.",
    cancelled: "",
    title: "Opname op de Tab5",
    note: "Stop eerst de sessie op de tablet. Verbinden kan hem herstarten. Een opname boven 80 MB is te groot voor deze server.",
    empty: "Geen opnames op de kaart.",
    get: "Haal opname op",
    close: "Sluiten",
    reading: "Tablet wordt gelezen…",
    fetching: (name, pct) => name + "  " + pct + "%",
    tooBig: (name, size) => name + " is " + size + ", meer dan de 80 MB die deze server aan kan.",
    recording: "de tablet is aan het opnemen; stop die eerst",
    stageRead: (i, n, name, pct) => "Lezen van de tablet (" + i + "/" + n + "): " + name + "  " + pct + "%",
    stageSend: (pct) => "Naar de server sturen  " + pct + "%",
    stageAnalyse: (s) => "Opname wordt geanalyseerd… " + s + " s. Een lange duurt een of twee minuten.",
    uploadFailed: "Het uploaden mislukte: de server antwoordde niet. Kies Haal opname op om opnieuw te proberen; de tablet wordt niet nog eens gelezen.",
  },
};

const $dev = (id) => document.getElementById(id);

// The bar in the window: pct is a number, or null when there is nothing to measure (the analysis).
function showProgress(text, pct) {
  $dev("device-progress").hidden = false;
  $dev("device-note").hidden = true;
  $dev("device-progress-text").textContent = text;
  const bar = $dev("device-bar");
  if (pct == null) bar.removeAttribute("value");
  else bar.value = pct;
}

function hideProgress() {
  $dev("device-progress").hidden = true;
  $dev("device-note").hidden = false;
}

function postForm(url, body, onProgress, onSent) {
  const copy = deviceCopy();
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", url);
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) onProgress(e.loaded, e.total); };
    xhr.upload.onload = onSent;
    xhr.onerror = () => reject(new Error(copy.uploadFailed));
    xhr.onabort = () => reject(new Error(copy.uploadFailed));
    xhr.onload = () => {
      let data = null;
      try {
        data = JSON.parse(xhr.responseText);
      } catch (err) {
        reject(new Error(xhr.status >= 400
          ? "The server is not answering (" + xhr.status + "). Try again in a minute."
          : "The server sent an unexpected answer. Try again."));
        return;
      }
      if (xhr.status >= 400) reject(new Error(data.error || xhr.statusText));
      else resolve(data);
    };
    xhr.send(body);
  });
}

// Files already read off the tablet, kept while the window is open so a failed upload is not read again.
const readFiles = new Map();

function deviceCopy() {
  return DEVICE_COPY[typeof lang === "function" && lang() === "nl" ? "nl" : "en"];
}

// When a session was recorded, as the tablet's own clock wrote it into the session's info file.
function startedText(utc) {
  const when = new Date(utc);
  if (isNaN(when.getTime())) return "";
  const locale = typeof lang === "function" && lang() === "nl" ? "nl-NL" : "en-GB";
  return when.toLocaleString(locale, { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" });
}

function optionText(session, imu) {
  const parts = [];
  if (session.started) parts.push(session.started);
  parts.push(session.name, megabytes(imu.size));
  return parts.join("  ·  ");
}

let datesStop = false;

// Fills in the date of each session, newest first, while the list is already usable. Each is one
// small file. It stops as soon as a download starts, and quietly gives up when the tablet is busy.
async function readDates(port, sessions) {
  datesStop = false;
  const box = document.getElementById("device-session");
  for (const session of sessions) {
    if (datesStop || port !== tab5) return;
    const info = session.files.find((file) => file.name.endsWith("_info.csv"));
    const imu = session.files.find((file) => file.name.endsWith("_imu.csv"));
    if (!info || !imu || session.started) continue;
    try {
      const text = await (await port.fetchFile(info.name)).text();
      const m = /^start_utc,(\S+)/m.exec(text);
      if (!m) continue;
      session.started = startedText(m[1]);
      const opt = [...box.options].find((item) => item.value === session.id);
      if (opt) opt.textContent = optionText(session, imu);
    } catch (err) {
      return;
    }
  }
}

function fillDeviceList(sessions) {
  const copy = deviceCopy();
  const box = document.getElementById("device-session");
  box.innerHTML = "";
  for (const session of sessions) {
    const imu = session.files.find((file) => file.name.endsWith("_imu.csv"));
    if (!imu) continue;
    const opt = document.createElement("option");
    opt.value = session.id;
    opt.textContent = optionText(session, imu);
    opt.disabled = imu.size > TAB5_MAX;
    box.append(opt);
  }
  // A list box starts with nothing selected, unlike a dropdown.
  const choice = [...box.options].find((opt) => !opt.disabled) || box.options[0] || null;
  if (choice) choice.selected = true;
  document.getElementById("device-note").textContent = box.options.length ? copy.note : copy.empty;
  const get = document.getElementById("device-get");
  get.disabled = !choice || choice.disabled;
  box.onchange = () => {
    const opt = box.options[box.selectedIndex];
    get.disabled = !opt || opt.disabled;
  };
}

let tab5 = null;

async function connectTab5() {
  const copy = deviceCopy();
  if (!navigator.serial) {
    setStatus(copy.unsupported);
    return;
  }
  let port;
  try {
    port = await navigator.serial.requestPort();
  } catch (err) {
    if (err && err.name === "NotFoundError") return;
    setStatus(err.message);
    return;
  }
  if (tab5) await tab5.close();
  tab5 = new Tab5Port(port);
  setStatus(copy.reading);
  try {
    await tab5.open();
    const sessions = await tab5.list();
    fillDeviceList(sessions);
    document.getElementById("device-title").textContent = copy.title;
    document.getElementById("device-get").textContent = copy.get;
    document.getElementById("device-close").textContent = copy.close;
    document.getElementById("device").showModal();
    setStatus("");
    readDates(tab5, sessions);
  } catch (err) {
    setStatus(err.message);
    await tab5.close();
    tab5 = null;
  }
}

async function getFromTab5() {
  const copy = deviceCopy();
  const box = document.getElementById("device-session");
  const id = box.value;
  const option = box.options[box.selectedIndex];
  if (!id || !tab5 || !option || option.disabled) return;
  document.getElementById("device-get").disabled = true;
  box.disabled = true;
  setStatus("");
  datesStop = true;
  try {
    const sessions = await tab5.list();
    const session = sessions.find((item) => item.id === id);
    if (!session) throw new Error(copy.empty);
    const wanted = session.files.filter((file) => /_(imu|info|seconds|laps|strokes)\.csv$/.test(file.name));
    const imu = wanted.find((file) => file.name.endsWith("_imu.csv"));
    if (!imu) throw new Error(copy.empty);
    if (imu.size > TAB5_MAX) throw new Error(copy.tooBig(imu.name, megabytes(imu.size)));
    let blobs = readFiles.get(id);
    if (!blobs) {
      blobs = [];
      for (let i = 0; i < wanted.length; i++) {
        const file = wanted[i];
        showProgress(copy.stageRead(i + 1, wanted.length, file.name, 0), 0);
        const blob = await tab5.fetchFile(file.name, (got, size) => {
          const pct = Math.round(100 * got / size);
          showProgress(copy.stageRead(i + 1, wanted.length, file.name, pct), pct);
        });
        blobs.push({ name: file.name, blob });
      }
      readFiles.set(id, blobs);
    }
    const body = new FormData();
    for (const item of blobs) body.append("file", item.blob, item.name);
    showProgress(copy.stageSend(0), 0);
    const opened = await postForm("/api/upload", body,
      (sent, total) => {
        const pct = Math.round(100 * sent / total);
        showProgress(copy.stageSend(pct), pct);
      },
      () => showProgress(copy.stageAnalyse(0), null));
    analysingNotice = (s) => showProgress(copy.stageAnalyse(s), null);
    try {
      await loadSessions(opened.id);
    } finally {
      analysingNotice = null;
    }
    readFiles.delete(id);
    document.getElementById("device").close();
  } catch (err) {
    const message = String(err.message || err);
    if (!tab5) return;  // the window was closed on purpose
    hideProgress();
    setStatus(message === "session open" ? copy.recording : message);
    document.getElementById("device-get").disabled = false;
    box.disabled = false;
  } finally {
    analysingNotice = null;
  }
}

document.getElementById("tab5").addEventListener("click", () => {
  connectTab5().catch((err) => setStatus(err.message));
});
document.getElementById("device-close").addEventListener("click", () => document.getElementById("device").close());
// However the window closes, give the port back: another tool (fetch.py) cannot open it while this page holds it.
document.getElementById("device").addEventListener("close", () => {
  datesStop = true;
  analysingNotice = null;
  readFiles.clear();
  hideProgress();
  $dev("device-session").disabled = false;
  const port = tab5;
  tab5 = null;
  if (port) port.close();
});
document.getElementById("device-get").addEventListener("click", () => {
  getFromTab5().catch((err) => setStatus(err.message));
});
