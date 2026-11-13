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
  return [...sessions.values()].sort((a, b) => a.id.localeCompare(b.id));
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
  }

  async open() {
    await this.port.open({ baudRate: 115200, bufferSize: 65536 });
    // Held low so a later close does not pulse the lines. The open itself can
    // still restart the tablet; the browser sets these lines as it connects.
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
  }

  async close() {
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
  async _take(deadline) {
    if (!this.pending) this.pending = this.reader.read();
    const left = deadline - performance.now();
    if (left <= 0) return false;
    const timed = await Promise.race([
      this.pending.then((value) => ({ value })),
      new Promise((resolve) => setTimeout(() => resolve(null), left)),
    ]);
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

  async readExact(n, timeoutMs) {
    const deadline = performance.now() + timeoutMs;
    while (this.buf.length < n) {
      if (!await this._take(deadline)) throw new Error("the tablet stopped sending");
    }
    const out = this.buf.slice(0, n);
    this.buf = this.buf.slice(n);
    return out;
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

  async list() {
    this.buf = new Uint8Array(0);
    await this.write(this.text.encode("D"));
    const lines = [];
    const deadline = performance.now() + 8000;
    let quiet = performance.now() + 1500;
    while (performance.now() < deadline) {
      const line = await this.readLine(Math.max(50, quiet - performance.now()));
      if (line === null) {
        if (lines.some((item) => item.startsWith("[files]"))) break;
        if (performance.now() > deadline) break;
        quiet = performance.now() + 400;
        continue;
      }
      lines.push(line);
      if (line.startsWith("[files]")) quiet = performance.now() + 1200;
    }
    if (lines.some((line) => line.startsWith("[files] no SD"))) throw new Error("the tablet has no SD card");
    return sessionsFromLines(lines);
  }

  async fetchFile(name, onProgress) {
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
        for (let attempt = 0; attempt < 20; attempt++) {
          await this.write(this.text.encode(attempt === 0 ? "N" : "R"));
          const raw = await this.readExact(want + 4, 10000);
          const body = raw.slice(0, want);
          const crc = raw[want] | (raw[want + 1] << 8) | (raw[want + 2] << 16) | (raw[want + 3] << 24);
          if ((crc >>> 0) === crc32(body)) {
            chunk = body;
            break;
          }
        }
        if (!chunk) throw new Error("too many bad chunks");
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
  },
};

function deviceCopy() {
  return DEVICE_COPY[typeof lang === "function" && lang() === "nl" ? "nl" : "en"];
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
    opt.textContent = session.name + "   " + megabytes(imu.size);
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
  try {
    const sessions = await tab5.list();
    const session = sessions.find((item) => item.id === id);
    if (!session) throw new Error(copy.empty);
    const wanted = session.files.filter((file) => file.name.endsWith("_imu.csv") || file.name.endsWith("_info.csv"));
    const imu = wanted.find((file) => file.name.endsWith("_imu.csv"));
    if (!imu) throw new Error(copy.empty);
    if (imu.size > TAB5_MAX) throw new Error(copy.tooBig(imu.name, megabytes(imu.size)));
    const body = new FormData();
    for (const file of wanted) {
      setStatus(copy.fetching(file.name, 0));
      const blob = await tab5.fetchFile(file.name, (got, size) => {
        setStatus(copy.fetching(file.name, Math.round(100 * got / size)));
      });
      body.append("file", blob, file.name);
    }
    setStatus(copy.reading);
    const opened = await getJSON("/api/upload", { method: "POST", body });
    document.getElementById("device").close();
    await loadSessions(opened.id);
  } catch (err) {
    const message = String(err.message || err);
    setStatus(message === "session open" ? copy.recording : message);
    document.getElementById("device-get").disabled = false;
  }
}

document.getElementById("tab5").addEventListener("click", () => {
  connectTab5().catch((err) => setStatus(err.message));
});
document.getElementById("device-close").addEventListener("click", () => document.getElementById("device").close());
document.getElementById("device-get").addEventListener("click", () => {
  getFromTab5().catch((err) => setStatus(err.message));
});
