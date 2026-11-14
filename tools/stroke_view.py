"""Stroke-by-stroke viewer for a recorded session.

Opens a page in the browser with the same phases, reference and coaching hint as
the Stroke screen, and the marks on the curve those hints are read from.
Zoom with the wheel, drag to pan, and click a mark to look at just that part.

Usage:
  python tools/stroke_view.py
  python tools/stroke_view.py --port 8765 --recordings recordings

On Render the process is started with no arguments. It then listens on 0.0.0.0
and the PORT Render sets. Locally, with no PORT, it stays on 127.0.0.1.
"""
import argparse
import csv
import json
import math
import os
import re
import sys
import threading
import traceback
import webbrowser
from itertools import repeat
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anatomy
import overview

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "stroke_view")
# A recording that ships with the viewer, so there is always something to look at.
DEMO = os.path.join(HERE, "demo")
BEST_FRAC = 0.95
REF_MIN = 3
AVG_N = 5
RUN_CAP = 30
MIN_RATE, MAX_RATE, BAND_W = 14, 40, 4
RATE_MIN = 3

_sessions = {}
_root = "recordings"
# A raw log of a long outing is far bigger than this. The free Render instance has
# 512 MB, and the whole recording is held in memory while it is read.
MAX_UPLOAD = 80 * 1024 * 1024
_UPLOAD_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,120}\.csv\Z")
STATIC = {
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    "/tab5.js": ("tab5.js", "text/javascript; charset=utf-8"),
    "/overview.js": ("overview.js", "text/javascript; charset=utf-8"),
    "/guide.js": ("guide.js", "text/javascript; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/vendor/leaflet.js": ("vendor/leaflet.js", "text/javascript; charset=utf-8"),
    "/vendor/leaflet.css": ("vendor/leaflet.css", "text/css; charset=utf-8"),
}
UPLOAD_SUFFIXES = ("_imu.csv", "_info.csv", "_seconds.csv", "_laps.csv", "_strokes.csv")


def band_of(rate):
    r = int(round(rate))
    return (min(MAX_RATE, max(MIN_RATE, r)) - MIN_RATE) // BAND_W


def _num(v, nd=4):
    if v is None:
        return None
    v = float(v)
    if v != v or v in (float("inf"), float("-inf")):
        return None
    return round(v, nd)


def _series(values):
    return [_num(v) for v in values]


class Template:
    def __init__(self):
        self.n = 0
        self.preroll = np.zeros(anatomy.PREROLL_N)
        self.accel = np.zeros(anatomy.BINS)
        self.drive = 0.0
        self.rec = 0.0

    def blend(self, preroll, accel, drive, rec):
        self.n += 1
        k = 1.0 / (self.n if self.n < RUN_CAP else RUN_CAP)
        self.preroll += (preroll - self.preroll) * k
        self.accel += (accel - self.accel) * k
        self.drive += (drive - self.drive) * k
        self.rec += (rec - self.rec) * k


def _features(feat):
    if feat is None:
        return None
    keys = ("depth", "width", "loss", "notches", "peak", "gain", "peak_pct", "sag", "release",
            "fall", "run_mg", "speed_peak", "lead", "approach_loss")
    return {k: _num(feat[k]) for k in keys}


def _markers(feat, shift):
    if feat is None:
        return None
    return {
        "width_l": _num(feat["width_l"]),
        "width_r": _num(feat["width_r"]),
        "half_level": _num(feat["half_level"]),
        "depth_t": _num(feat["depth_t"]),
        "depth_g": _num(feat["depth_g"]),
        "notch_t": [_num(t) for t in feat["notch_t"]],
        "peak_t": _num(feat["peak_t"]),
        "peak_g": _num(feat["peak"]),
        "t80": _num(feat["t80"]),
        "t20": _num(feat["t20"]),
        "level80": _num(feat["level80"]),
        "level20": _num(feat["level20"]),
        "release_t": _num(feat["release_t"]),
        "release_g": _num(feat["release_g"]),
        "run_base": _num(feat["run_base"]),
        "sag_t": _num(feat["sag_t"]),
        "sag_g": _num(feat["sag_g"]),
        "speed_shift": _num(shift),
    }


def _bounds(feat):
    if feat is None:
        return None
    return {k: _num(feat[k]) for k in ("catch0", "catch1", "finish", "finish_end", "approach", "interval")}


def _shift(feat, mean_speed):
    gt, gv = feat["curve_t"], feat["curve_v"]
    sel = (gt >= 0) & (gt <= feat["interval"])
    base = float(gv[sel].mean()) if sel.any() else 0.0
    return mean_speed - base


class Session:
    def __init__(self, sid, name):
        self.id = sid
        self.name = name
        self.strokes = []
        self.usual = {}
        self.best = {}
        self.usual_motion = {key: {} for key in MOTION}
        self.motion = None  # roll and yaw over the outing, see _motion_summary()
        self.best_feat = {}
        self.usual_feat = {}
        self.avg_feat = {}
        self.t = np.zeros(0)
        self.surge = np.zeros(0, np.float32)
        self.gps = np.zeros(0, np.float32)
        self.origin = 0.0
        self.logged = 0
        self.detected = 0
        self.rescued_n = 0
        self.detector = ""

    def ref_for(self, rate):
        b = band_of(rate)
        if self.best[b].n >= REF_MIN:
            return self.best[b], self.best_feat[b], True
        if self.usual[b].n >= REF_MIN:
            return self.usual[b], self.usual_feat[b], False
        return None, None, False


def _stroke_rows(path):
    st = np.genfromtxt(path, delimiter=",", names=True)
    if st.size == 0:
        return []
    if st.shape == ():
        st = np.array([st], dtype=st.dtype)
    names = st.dtype.names
    rows = []
    for i, row in enumerate(st):
        rows.append({
            "i": i,
            "elapsed": float(row["elapsed_s"]) if "elapsed_s" in names else 0.0,
            "rate": float(row["rate_spm"]),
            "interval": float(row["interval_s"]),
            "drive": float(row["drive_s"]),
            "recovery": float(row["recovery_s"]) if "recovery_s" in names else 0.0,
            "split": float(row["split_s"]) if "split_s" in names else 0.0,
            "speed": float(row["speed_mps"]) if "speed_mps" in names else 0.0,
            "dps": float(row["dps_m"]) if "dps_m" in names else 0.0,
            "t": float(row["t_s"]),
            "rescued": int(row["rescued"]) if "rescued" in names and row["rescued"] == row["rescued"] else 0,
        })
    return rows


def _info(base):
    path = base + "_info.csv"
    if not os.path.exists(path):
        return {}
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        return {r["key"]: r["value"] for r in csv.DictReader(f) if r.get("key")}


def _mean_speed(t, gps, t0, t1):
    lo, hi = int(np.searchsorted(t, t0)), int(np.searchsorted(t, t1, side="right"))
    if hi <= lo:
        return 0.0
    good = gps[lo:hi]
    good = good[good > 0.3]
    return float(good.mean()) if len(good) else 0.0


# What is read off the sensor besides the surge, per stroke: sideways acceleration, roll and yaw.
MOTION = ("sway", "roll", "yaw")


class _Samples:
    """The recording as the replay's rows, made a slice at a time. A list of tuples of Python
    floats takes about 200 bytes a sample, which for a three hour recording is a quarter of a GB."""

    CHUNK = 50000

    def __init__(self, t, ax, ay, az, gps, gyro=None):
        self.cols = (t, ax, ay, az, gps)
        self.gyro = gyro  # gx, gy, gz in deg/s, or None

    def __iter__(self):
        t, ax, ay, az, gps = self.cols
        for lo in range(0, len(t), self.CHUNK):
            hi = lo + self.CHUNK
            cols = [t[lo:hi].tolist(), ax[lo:hi].tolist(), ay[lo:hi].tolist(), az[lo:hi].tolist(),
                    repeat(0), gps[lo:hi].tolist()]
            if self.gyro is not None:
                cols += [g[lo:hi].tolist() for g in self.gyro]
            for row in zip(*cols):
                yield row

    def __len__(self):
        return len(self.cols[0])


def _replay_strokes(imu, t, gps, info, origin):
    """Catches and the surge curve from the current detector.

    That brings the gap-fill the device did not have yet, and forward settled
    from GPS over the whole recording, so the first strokes are the right way up.
    The curve comes from the same run as the catches: the logged one carries
    whatever direction the device believed at the time.
    """
    import replay

    k = float(info.get("threshold_k", 1.8))
    floor = float(info.get("threshold_floor_g", 0.12))
    minima = info.get("minima_threshold") == "1"
    auto = info.get("auto_threshold") == "1"
    gyro = tuple(imu[name] for name in GYRO_COLUMNS) if all(name in imu for name in GYRO_COLUMNS) else None
    rows = _Samples(t, imu["ax_g"], imu["ay_g"], imu["az_g"], gps, gyro)
    events, trace, det = replay.run(rows, k, floor, minima, keep_trace="surge", hindsight=True,
                                    gps_delay=replay.gps_delay_of(info), auto=auto)
    surge = np.frombuffer(trace, dtype=np.float64)
    motion = {"sway": np.frombuffer(det.sway, dtype=np.float32),
              "roll": np.frombuffer(det.roll_trace, dtype=np.float32),
              "yaw": np.frombuffer(det.yaw_trace, dtype=np.float32)}
    meta = []
    rescued = 0
    for ev in events:
        if ev["interval"] <= 0 or ev["drive"] <= 0:
            continue
        speed = _mean_speed(t, gps, ev["t"] - ev["interval"], ev["t"])
        if ev["rescued"]:
            rescued += 1
        meta.append({
            "i": len(meta),
            "elapsed": ev["t"] - origin,
            "rate": float(ev["rate"]),
            "interval": float(ev["interval"]),
            "drive": float(ev["drive"]),
            "recovery": float(ev["recovery"]),
            "split": (500.0 / speed) if speed > 0.3 else 0.0,
            "speed": speed,
            "dps": speed * float(ev["interval"]) if speed > 0.3 else 0.0,
            "t": float(ev["t"]),
            "rescued": 1 if ev["rescued"] else 0,
        })
    label = "%s, %s" % (replay.threshold_label(k, floor, minima, auto), replay.direction_note(det))
    return meta, len(events), rescued, label, surge, motion


GYRO_COLUMNS = ("gx_dps", "gy_dps", "gz_dps")
IMU_COLUMNS = ("t_s", "ax_g", "ay_g", "az_g", "gps_mps") + GYRO_COLUMNS


def _read_imu(path):
    """The columns the replay needs, as float64 arrays. A 30 MB recording has hundreds of
    thousands of rows, and genfromtxt would hold every column of every row as Python objects
    on the way in (over 600 MB, more than a small server has). loadtxt reads only these."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline().strip().split(",")
    missing = [name for name in IMU_COLUMNS[:4] if name not in header]
    if missing:
        raise ValueError("%s is not an _imu.csv: no %s column" % (os.path.basename(path), missing[0]))
    wanted = [name for name in IMU_COLUMNS if name in header]
    data = np.loadtxt(path, delimiter=",", skiprows=1, usecols=[header.index(n) for n in wanted],
                      dtype=np.float64, ndmin=2)
    out = {name: np.ascontiguousarray(data[:, i], dtype=np.float32 if name in GYRO_COLUMNS else np.float64)
           for i, name in enumerate(wanted)}
    del data
    if "gps_mps" not in out:
        out["gps_mps"] = np.full(len(out["t_s"]), -1.0)
    return out

def load_session(imu_path):
    base, imu_path, strokes_path = anatomy.session_paths(imu_path)
    sid = os.path.basename(base)
    label = sid.split("_", 1)
    name = sid if len(label) == 1 else "%s %s" % (label[0], label[1].replace("_", " "))
    imu = _read_imu(imu_path)
    t = imu["t_s"]
    gps = imu["gps_mps"]
    info = _info(base)
    origin = float(info.get("start_t_s", t[0])) if len(t) else 0.0
    logged = _stroke_rows(strokes_path) if os.path.exists(strokes_path) else []
    logged_n = len(logged)
    meta, detected, rescued_n, detector, raw_surge, raw_motion = _replay_strokes(imu, t, gps, info, origin)
    surge = anatomy.lowpass(raw_surge)
    import replay

    gps_delay = replay.gps_delay_of(info)
    session = Session(sid, name)
    session.motion = _motion_summary(t, gps, raw_motion)
    session.logged = logged_n
    session.detected = detected
    session.rescued_n = rescued_n
    session.detector = detector
    session.origin = origin
    built = []
    for row in meta:
        if row["interval"] <= 0 or row["drive"] <= 0:
            continue
        catch = row["t"]
        prev = catch - row["interval"]
        finish = prev + row["drive"]
        cyc = anatomy.cut_cycle(t, surge, prev, finish, catch)
        if cyc is None:
            row["ok"] = False
            built.append(row)
            continue
        feat = anatomy.analyse(*cyc)
        row["ok"] = feat is not None
        row["cyc"] = cyc
        row["feat"] = feat
        lo, hi = int(np.searchsorted(t, prev - anatomy.PREROLL_S)), int(np.searchsorted(t, catch, side="right"))
        row["raw_t"] = t[lo:hi] - prev
        row["raw_a"] = surge[lo:hi]
        # Sideways, roll and yaw from the same samples, binned like the surge. Sideways acceleration is
        # filtered only over this stroke (a recording is too long to run the filter over all of it).
        # Angles are read against their own mean over the stroke.
        for key in MOTION:
            raw = raw_motion[key]
            if key == "sway":
                slo = max(0, lo - 50)
                part = anatomy.lowpass_window(raw[slo:hi])[lo - slo:]
            else:
                part = np.asarray(raw[lo:hi], dtype=np.float64)
            cyc = anatomy.cut_cycle(t[lo:hi], part, prev, finish, catch)
            if key != "sway" and cyc is not None:
                cyc, mean = _centred(cyc)
                part = part - mean
            row["raw_" + key] = part
            row["cyc_" + key] = cyc
        # The receiver reports speed late, so a fix is drawn where the boat was doing that speed.
        glo = int(np.searchsorted(t, prev - anatomy.PREROLL_S + gps_delay))
        ghi = int(np.searchsorted(t, catch + gps_delay, side="right"))
        gt, gv = t[glo:ghi], gps[glo:ghi]
        keep = np.flatnonzero(np.diff(gv, prepend=gv[:1] - 1) != 0)
        row["gps_t"] = gt[keep] - gps_delay - prev
        row["gps_v"] = gv[keep]
        built.append(row)

    # Best distance per stroke, per integer rate, from 3-stroke averages. Same rule as the device.
    bin_best, bin_count = {}, {}
    for i in range(2, len(built)):
        window = built[i - 2:i + 1]
        if any(s.get("dps", 0) <= 0 or s.get("rate", 0) <= 0 for s in window):
            continue
        rate = sum(s["rate"] for s in window) / 3
        dps = sum(s["dps"] for s in window) / 3
        b = int(round(rate))
        bin_count[b] = bin_count.get(b, 0) + 1
        bin_best[b] = max(bin_best.get(b, 0), dps)

    for b in range((MAX_RATE - MIN_RATE) // BAND_W + 1):
        session.usual[b] = Template()
        session.best[b] = Template()
        for key in MOTION:
            session.usual_motion[key][b] = Template()
    for row in built:
        if not row.get("ok"):
            continue
        preroll, accel, drive, rec = row["cyc"]
        b = band_of(row["rate"])
        session.usual[b].blend(preroll, accel, drive, rec)
        for key in MOTION:
            if row.get("cyc_" + key) is not None:
                session.usual_motion[key][b].blend(*row["cyc_" + key])
        best = bin_best.get(int(round(row["rate"])), 0)
        counted = bin_count.get(int(round(row["rate"])), 0)
        if counted >= 3 and row["dps"] > 0 and best > 0 and row["dps"] >= BEST_FRAC * best:
            session.best[b].blend(preroll, accel, drive, rec)
            row["is_best"] = True
        else:
            row["is_best"] = False
    for b, tmpl in session.usual.items():
        session.usual_feat[b] = anatomy.analyse(tmpl.preroll, tmpl.accel, tmpl.drive, tmpl.rec) if tmpl.n else None
    for b, tmpl in session.best.items():
        session.best_feat[b] = anatomy.analyse(tmpl.preroll, tmpl.accel, tmpl.drive, tmpl.rec) if tmpl.n else None
    session.strokes = built
    session.t = t
    session.surge = np.asarray(surge, np.float32)
    # On the timeline the speed is moved back by the same delay, so it lines up with the surge.
    later = np.searchsorted(t, t + gps_delay)
    shown = np.where(later < len(t), gps[np.minimum(later, len(t) - 1)], -1.0) if len(t) else gps
    session.gps = np.asarray(shown, np.float32)
    return session


def _uploads_dir():
    return os.path.join(_root, "uploads")


def _imu_file(sid):
    """The recording to open for this id. An upload wins over a file already on disk."""
    if not sid or os.path.basename(sid.replace("\\", "/")) != sid or ".." in sid:
        return None
    name = sid if sid.endswith("_imu.csv") else sid + "_imu.csv"
    if not _UPLOAD_NAME.match(name) or not name.endswith("_imu.csv"):
        return None
    for folder in (_uploads_dir(), _root, DEMO):
        path = os.path.join(folder, name)
        if os.path.isfile(path):
            return path
    return None


def _inside_root(path):
    root = os.path.realpath(_root)
    target = os.path.realpath(path)
    return os.path.commonpath([root, target]) == root


def list_sessions(root):
    found = []
    seen = set()
    folders = []
    uploads = os.path.join(root, "uploads")
    if os.path.isdir(uploads):
        folders.append(uploads)
    if os.path.isdir(root):
        folders.append(root)
    if os.path.isdir(DEMO):
        folders.append(DEMO)
    for folder in folders:
        for name in sorted(os.listdir(folder)):
            if not name.endswith("_imu.csv"):
                continue
            base = os.path.join(folder, name[: -len("_imu.csv")])
            strokes = base + "_strokes.csv"
            sid = os.path.basename(base)
            if sid in seen:
                continue
            label = sid.replace("_", " ")
            if sid in _sessions:
                n = len(_sessions[sid].strokes)
            elif os.path.exists(strokes):
                with open(strokes, "r", encoding="utf-8", errors="replace") as f:
                    next(f, None)
                    n = sum(1 for line in f if line.strip())
            else:
                n = 0
            demo = os.path.realpath(folder) == os.path.realpath(DEMO)
            found.append({"id": sid, "name": label + (" (demo)" if demo else ""), "strokes": n, "demo": demo})
            seen.add(sid)
    for session in _sessions.values():
        if session.id not in seen:
            found.append({"id": session.id, "name": session.name, "strokes": len(session.strokes)})
    return found


def _average_feat(session, index):
    """Phases of the last few strokes at this rate, which is what the device reads its tip from."""
    if index not in session.avg_feat:
        averaged = _average(session, index)
        session.avg_feat[index] = anatomy.analyse(*averaged[:4]) if averaged else None
    return session.avg_feat[index]


def _hint_row(session, index, row, crew, lang, view):
    if not row.get("ok"):
        return anatomy.coaching(None, None, False, False, crew, row.get("rate") or 0, lang)
    now = _average_feat(session, index) if view == "average" else row["feat"]
    tmpl, feat, vs_best = session.ref_for(row["rate"])
    return anatomy.coaching(now, feat, tmpl is not None, vs_best, crew, row["rate"], lang)


def session_index(session, crew, lang, view="stroke"):
    out = []
    for n, row in enumerate(session.strokes):
        hint = _hint_row(session, n, row, crew, lang, view)
        out.append({
            "i": n,
            "elapsed": _num(row["elapsed"], 1),
            "rate": _num(row["rate"], 1),
            "split": _num(row["split"], 1),
            "dps": _num(row["dps"], 2),
            "rescued": row["rescued"],
            "best": bool(row.get("is_best")),
            "valid": bool(row.get("ok")),
            "hint": hint["text"],
            "warn": hint["warn"],
            "focus": hint["focus"],
            "t0": _num(row["t"] - row["interval"] - session.origin, 2),
            "t1": _num(row["t"] - session.origin, 2),
            "drive": _num(row["drive"], 2),
        })
    return {"id": session.id, "name": session.name, "strokes": out,
            "preroll_s": anatomy.PREROLL_S,
            "logged": session.logged, "detected": session.detected,
            "rescued": session.rescued_n, "detector": session.detector}


def _average(session, index):
    row = session.strokes[index]
    band = band_of(row["rate"])
    chosen = []
    for i in range(index, -1, -1):
        s = session.strokes[i]
        if s.get("ok") and band_of(s["rate"]) == band:
            chosen.append(s)
        if len(chosen) >= AVG_N:
            break
    if not chosen:
        return None
    preroll = np.mean([s["cyc"][0] for s in chosen], axis=0)
    accel = np.mean([s["cyc"][1] for s in chosen], axis=0)
    drive = float(np.mean([s["cyc"][2] for s in chosen]))
    rec = float(np.mean([s["cyc"][3] for s in chosen]))
    speed = [s["speed"] for s in chosen if s["speed"] > 0]
    return preroll, accel, drive, rec, float(np.mean(speed)) if speed else 0.0, len(chosen)


def _motion_summary(t, gps, motion):
    """Roll and yaw over the whole outing: how far the boat is off its mean attitude, as rms degrees
    over a 15 s window, one value per second, only where it was being rowed. None when the recording
    has no gyro."""
    if len(t) < 200:
        return None
    roll, yaw = motion["roll"], motion["yaw"]
    if not (np.any(roll) or np.any(yaw)):
        return None
    sec = np.floor(t - t[0]).astype(np.int64)
    n = int(sec[-1]) + 1
    count = np.bincount(sec, minlength=n).astype(np.float64)
    fixed = gps >= 0
    if fixed.any():
        speed = np.bincount(sec, weights=np.where(fixed, gps, 0.0), minlength=n) / np.maximum(
            np.bincount(sec, weights=fixed.astype(np.float64), minlength=n), 1.0)
        rowing = speed >= 1.0
    else:
        rowing = count > 0
    window = np.ones(15)
    near = np.convolve(count, window, "same")
    out = {"t0": float(t[0]), "n": n}
    for key, x in (("roll", roll), ("yaw", yaw)):
        squares = np.bincount(sec, weights=np.square(x, dtype=np.float64), minlength=n)
        rms = np.sqrt(np.convolve(squares, window, "same") / np.maximum(near, 1.0))
        out[key] = np.where(rowing & (near > 0), rms, np.nan)
        total = count[rowing].sum()
        out[key + "_rms"] = float(np.sqrt(squares[rowing].sum() / total)) if total else 0.0
    return out


_guide_example = None


def guide_example():
    """One clean stroke from the demo recording, for the Guide page to draw and annotate. The demo is
    read on its own and not kept in the session cache, and the answer is remembered, so the Guide costs
    one short read per start of the server and does not push anyone's session out of the cache."""
    global _guide_example
    if _guide_example is not None:
        return _guide_example
    imu = os.path.join(DEMO, "S0018_BBR_race_imu.csv")
    if not os.path.isfile(imu):
        _guide_example = {"has": False}
        return _guide_example
    with _load_lock:
        session = load_session(imu)
    for index, row in enumerate(session.strokes):
        if not row.get("ok"):
            continue
        detail = stroke_detail(session, index, 1, "stroke", "en")
        m, now = detail["markers"], detail["now"]
        if (detail["valid"] and 22 <= detail["rate"] <= 27 and m["sag_t"] is not None and m["sag_g"] > 0.08
                and m["release_t"] is not None and m["notch_t"]):
            keep = ("rate", "split", "dps", "drive", "rec", "now", "bounds", "markers", "curve")
            _guide_example = {"has": True, "id": session.id, "i": index, **{k: detail[k] for k in keep}}
            return _guide_example
    _guide_example = {"has": False}
    return _guide_example


def session_motion(session, origin):
    m = session.motion
    if m is None:
        return {"has": False}
    first = m["t0"] - origin
    return {
        "has": True,
        "t": [round(first + i + 0.5, 1) for i in range(m["n"])],
        "roll": [None if v != v else round(float(v), 2) for v in m["roll"]],
        "yaw": [None if v != v else round(float(v), 2) for v in m["yaw"]],
        "roll_rms": round(m["roll_rms"], 2),
        "yaw_rms": round(m["yaw_rms"], 2),
    }


def _centred(cyc):
    """A binned angle with its mean over the stroke taken off, and that mean."""
    preroll, accel, drive, rec = cyc
    mean = (accel[:anatomy.DRIVE_BINS].mean() * drive + accel[anatomy.DRIVE_BINS:].mean() * rec) / (drive + rec)
    return (preroll - mean, accel - mean, drive, rec), float(mean)


def _motion_average(session, index, key):
    """One motion cycle of the last few strokes at this rate, the same strokes _average() takes."""
    row = session.strokes[index]
    band = band_of(row["rate"])
    chosen = []
    for i in range(index, -1, -1):
        s = session.strokes[i]
        if s.get("ok") and s.get("cyc_" + key) is not None and band_of(s["rate"]) == band:
            chosen.append(s["cyc_" + key])
        if len(chosen) >= AVG_N:
            break
    if not chosen:
        return None
    return (np.mean([c[0] for c in chosen], axis=0), np.mean([c[1] for c in chosen], axis=0),
            float(np.mean([c[2] for c in chosen])), float(np.mean([c[3] for c in chosen])))


def _motion_curve(cyc):
    if cyc is None:
        return None
    out = anatomy.resample(*cyc)
    if out is None:
        return None
    return {"t": _series(out[0]), "a": _series(out[1])}


def _motion_of(session, index, row, averaged, key):
    """What a motion panel shows: this stroke (or the last few), and the usual one at this rate."""
    cyc = _motion_average(session, index, key) if averaged else row.get("cyc_" + key)
    now = _motion_curve(cyc)
    if now is None:
        return None
    usual = session.usual_motion[key].get(band_of(row["rate"]))
    ref = None
    if usual is not None and usual.n >= REF_MIN:
        ref = _motion_curve((usual.preroll, usual.accel, cyc[2], cyc[3]))
    raw = None
    if not averaged and row.get("raw_" + key) is not None:
        raw = {"t": _series(row["raw_t"]), "a": _series(row["raw_" + key])}
    return {"now": now, "ref": ref, "raw": raw}


def stroke_detail(session, index, crew, view, lang):
    row = session.strokes[index]
    if view == "average":
        averaged = _average(session, index)
    else:
        averaged = None
    if averaged:
        preroll, accel, drive, rec, mean_v, samples = averaged
        feat = _average_feat(session, index)
    elif row.get("ok"):
        preroll, accel, drive, rec = row["cyc"]
        feat = row["feat"]
        mean_v = row["speed"] if row["speed"] > 0 else 0.0
        samples = 1
    else:
        feat = None
        mean_v = 0.0
        samples = 0
        drive = rec = 0.0
    tmpl, ref_feat, vs_best = session.ref_for(row["rate"] if row.get("ok") else 0)
    hint = anatomy.coaching(feat, ref_feat, tmpl is not None and feat is not None, vs_best, crew, row.get("rate") or 0, lang)
    shift = _shift(feat, mean_v) if feat is not None else 0.0
    body = {
        "i": index,
        "valid": feat is not None,
        "samples": samples,
        "view": "average" if averaged else "stroke",
        "elapsed": _num(row.get("elapsed") or 0, 1),
        "rescued": int(row.get("rescued") or 0),
        "rate": _num(row.get("rate") or 0, 1),
        "split": _num(row.get("split") or 0, 1),
        "dps": _num(row.get("dps") or 0, 2),
        "drive": _num(drive),
        "rec": _num(rec),
        "vs_best": bool(vs_best) if tmpl is not None else False,
        "have_ref": tmpl is not None and ref_feat is not None,
        "hint": hint["text"],
        "warn": hint["warn"],
        "focus": hint["focus"],
        "now": _features(feat),
        "ref": _features(ref_feat) if tmpl is not None else None,
        "bounds": _bounds(feat),
        "markers": _markers(feat, shift),
        "curve": None,
        "raw": {"t": _series(row.get("raw_t", [])), "a": _series(row.get("raw_a", []))},
        "gps": {"t": _series(row.get("gps_t", [])), "v": _series(row.get("gps_v", []))},
        "ref_preroll": None,
        "ref_accel": None,
        "motion": {key: _motion_of(session, index, row, averaged is not None, key) for key in MOTION}
                  if feat is not None else None,
    }
    if feat is not None:
        body["curve"] = {
            "t": _series(feat["curve_t"]),
            "a": _series(feat["curve_a"]),
            "v": _series(feat["curve_v"] + shift),
        }
    if tmpl is not None and tmpl.n:
        body["ref_preroll"] = _series(tmpl.preroll)
        body["ref_accel"] = _series(tmpl.accel)
        body["ref_n"] = tmpl.n
    return body


def _curve_of(feat, shift):
    if feat is None:
        return None
    return {
        "t": _series(feat["curve_t"]),
        "a": _series(feat["curve_a"]),
        "v": _series(feat["curve_v"] + shift),
    }


# The reference for a rate is the average of its best strokes, by distance per
# stroke. One stroke carries ripple an average of many does not, so comparing the
# two finds differences in the finish and the release that are not there.
RATE_BEST_FRAC = 0.20
RATE_BEST_MIN = 3

_FEW = {
    "en": "Too few strokes at {spm} spm to set the best ones apart",
    "nl": "Te weinig slagen op {spm} spm om de beste apart te zetten",
}


def _pace_of(rows):
    """Split, distance per stroke and speed of a set of strokes, from their total time and distance.

    The mean of the splits is not the split of the mean: slow strokes pull it up.
    """
    time = dist = 0.0
    n = 0
    for row in rows:
        if row.get("speed", 0) > 0.3 and row.get("interval", 0) > 0:
            time += row["interval"]
            dist += row["speed"] * row["interval"]
            n += 1
    if dist <= 0:
        return 0.0, 0.0, 0.0
    return 500.0 * time / dist, dist / n, dist / time


def _mean_cycle(rows):
    preroll = np.mean([row["cyc"][0] for row in rows], axis=0)
    accel = np.mean([row["cyc"][1] for row in rows], axis=0)
    drive = float(np.mean([row["cyc"][2] for row in rows]))
    rec = float(np.mean([row["cyc"][3] for row in rows]))
    return preroll, accel, drive, rec


def _best_of(members):
    """The strokes that went furthest at this rate, furthest first, or [] when too few to set apart."""
    solid = [pair for pair in members if not pair[1].get("rescued") and pair[1].get("dps", 0) > 0]
    pool = solid if len(solid) >= RATE_BEST_MIN else [pair for pair in members if pair[1].get("dps", 0) > 0]
    pool = sorted(pool, key=lambda pair: -pair[1]["dps"])
    k = max(RATE_BEST_MIN, int(round(RATE_BEST_FRAC * len(pool))))
    if 2 * k > len(members):
        return pool[:1], False
    return pool[:k], True


def rate_report(session, crew, lang):
    """The average stroke at each stroke rate, against the average of the best strokes at that rate.

    The average is every readable stroke at that integer rate, not the last few.
    The best strokes are the fifth that went furthest per stroke, preferring catches
    the detector found on its own over ones filled in from the rhythm.
    """
    groups = {}
    readable = 0
    for i, row in enumerate(session.strokes):
        if not row.get("ok") or not row.get("rate"):
            continue
        readable += 1
        groups.setdefault(int(round(row["rate"])), []).append((i, row))
    out = []
    shown = 0
    for spm in sorted(groups):
        members = groups[spm]
        if len(members) < RATE_MIN:
            continue
        rows = [row for _, row in members]
        preroll, accel, drive, rec = _mean_cycle(rows)
        feat = anatomy.analyse(preroll, accel, drive, rec)
        if feat is None:
            continue
        split, dps, mean_v = _pace_of(rows)
        top, apart = _best_of(members)
        best_i, best = top[0] if top else members[0]
        best_rows = [row for _, row in top]
        best_feat = anatomy.analyse(*_mean_cycle(best_rows)) if apart else None
        best_split, best_dps, best_v = _pace_of(best_rows)
        best_shift = _shift(best_feat, best_v) if best_feat is not None and best_v > 0 else 0.0
        if best_feat is not None:
            hint = anatomy.coaching(feat, best_feat, True, True, crew, spm, lang)
        else:
            hint = {"text": _FEW.get(lang, _FEW["en"]).format(spm=spm), "warn": False, "focus": None}
        shown += len(rows)
        out.append({
            "spm": spm,
            "n": len(rows),
            "i": best_i,
            "valid": True,
            "samples": len(rows),
            "view": "rate",
            "elapsed": _num(best.get("elapsed") or 0, 1),
            "rescued": int(best.get("rescued") or 0),
            "rate": spm,
            "split": _num(split, 1),
            "dps": _num(dps, 2),
            "drive": _num(drive),
            "rec": _num(rec),
            "vs_best": best_feat is not None,
            "have_ref": best_feat is not None,
            "hint": hint["text"],
            "warn": hint["warn"],
            "focus": hint["focus"],
            "now": _features(feat),
            "ref": _features(best_feat),
            "bounds": _bounds(feat),
            "markers": _markers(feat, _shift(feat, mean_v) if mean_v else 0.0),
            "curve": _curve_of(feat, _shift(feat, mean_v) if mean_v else 0.0),
            "raw": {"t": [], "a": []},
            "gps": {"t": [], "v": []},
            # `i` is the single furthest stroke, to open. The rest describes the set that was averaged.
            "best": {
                "i": best_i,
                "n": len(best_rows) if best_feat is not None else 0,
                "elapsed": _num(best.get("elapsed") or 0, 1),
                "split": _num(best_split, 1),
                "dps": _num(best_dps, 2),
                "drive": _num(float(np.mean([row["drive"] for row in best_rows])) if best_rows else 0),
                "curve": _curve_of(best_feat, best_shift),
            },
        })
    # A rate is only worth recommending when the effort was the same across rates.
    spells = _spells(session.strokes)
    pace = None if spells else _choose_pace(session, out)
    for row in out:
        row["pace"] = bool(pace and row["spm"] == pace["spm"])
    return {"rates": out, "shown": shown, "readable": readable, "pace": pace, "spells": spells}


# Hard and light spells: an outing rowed as bursts of effort with easier rowing between.
# The drive peak says how hard a stroke was pulled, whatever the rate.
SPELL_MIN_STROKES_TOTAL = 40
SPELL_RATIO = 0.65       # light strokes pull under this share of the hard ones
SPELL_SHARE = 0.15       # each kind is at least this share of the strokes
SPELL_MIN_STROKES = 4    # shorter than this is not a spell
SPELL_MIN_COUNT = 3      # of each kind
SPELL_GAP_S = 8.0        # a pause ends a spell


def _spells(strokes):
    rows = [row for row in strokes if row.get("ok") and row.get("feat") is not None]
    if len(rows) < SPELL_MIN_STROKES_TOTAL:
        return None
    peak = np.array([row["feat"]["peak"] for row in rows])
    smooth = np.convolve(np.pad(peak, 1, mode="edge"), np.ones(3) / 3, mode="valid")
    lo, hi = np.percentile(smooth, 25), np.percentile(smooth, 75)
    for _ in range(30):
        hard = smooth > 0.5 * (lo + hi)
        if hard.all() or not hard.any():
            return None
        new_lo, new_hi = float(smooth[~hard].mean()), float(smooth[hard].mean())
        if abs(new_lo - lo) + abs(new_hi - hi) < 1e-6:
            break
        lo, hi = new_lo, new_hi
    if lo >= SPELL_RATIO * hi or not SPELL_SHARE <= hard.mean() <= 1 - SPELL_SHARE:
        return None
    # One odd stroke does not end a spell.
    votes = np.convolve(np.pad(hard.astype(float), 2, mode="edge"), np.ones(5) / 5, mode="valid")
    hard = votes > 0.5

    runs = []
    for i, row in enumerate(rows):
        fresh = i == 0 or hard[i] != hard[i - 1] or row["t"] - rows[i - 1]["t"] > SPELL_GAP_S
        if fresh:
            runs.append((bool(hard[i]), []))
        runs[-1][1].append(i)
    runs = [(kind, idx) for kind, idx in runs if len(idx) >= SPELL_MIN_STROKES]

    def describe(kind):
        mine = [idx for k, idx in runs if k == kind]
        if len(mine) < SPELL_MIN_COUNT:
            return None
        members = [rows[i] for idx in mine for i in idx]
        time = sum(row["interval"] for row in members)
        split, dps, _speed = _pace_of(members)
        return {
            "n": len(mine),
            "strokes": len(members),
            "median_s": _num(float(np.median([sum(rows[i]["interval"] for i in idx) for idx in mine])), 1),
            "total_s": _num(time, 1),
            "rate": _num(60.0 * len(members) / time if time > 0 else 0, 1),
            "split": _num(split, 1),
            "dps": _num(dps, 2),
            "peak_g": _num(float(np.mean([peak[i] for idx in mine for i in idx])), 3),
        }

    found = {"hard": describe(True), "light": describe(False)}
    if found["hard"] is None or found["light"] is None:
        return None
    return found


# A quicker split only counts as the pace to hold when the rate was actually
# kept up and the average stroke stays clean. A few fast strokes do not.
CLEAN_METRICS = (
    ("width", 1.5), ("notches", 1.0), ("loss", 1.4), ("release", 1.0),
    ("sag", 0.6), ("fall", 0.8), ("run_mg", 0.5), ("lead", 0.4),
)
PACE_LONG_S = 40.0
PACE_TOTAL_S = 60.0


def _bouts(strokes):
    longest, total = {}, {}
    run_spm = None
    run_len = 0.0
    prev_t = None

    def close():
        nonlocal run_spm, run_len
        if run_spm is not None and run_len > 0:
            longest[run_spm] = max(longest.get(run_spm, 0.0), run_len)
            total[run_spm] = total.get(run_spm, 0.0) + run_len
        run_spm = None
        run_len = 0.0

    for row in strokes:
        if not row.get("ok") or not row.get("rate") or row.get("interval", 0) <= 0:
            close()
            prev_t = row.get("t")
            continue
        spm = int(round(row["rate"]))
        gap = None if prev_t is None else row["t"] - prev_t
        if run_spm != spm or (gap is not None and gap > 8.0):
            close()
            run_spm = spm
        run_len += row["interval"]
        prev_t = row["t"]
    close()
    return longest, total


def _sat(seconds, full):
    if seconds <= 0:
        return 0.0
    return min(1.0, math.log1p(seconds) / math.log1p(full))


def _hold_score(longest, total):
    # Extra time still counts, but the first minute matters more than the tenth.
    return 0.6 * _sat(longest, 240.0) + 0.4 * _sat(total, 600.0)


def _clean_keep(now, ref):
    """How close the average stroke stays to the best stroke at the same rate. 1 is as clean."""
    if not now or not ref:
        return 0.5
    parts = []
    for key, _weight in CLEAN_METRICS:
        a, b = now.get(key), ref.get(key)
        if a is None or b is None or a <= 0:
            continue
        parts.append(min(1.0, b / a))
    return sum(parts) / len(parts) if parts else 0.5


def _clean_scores(rates):
    """1 is the cleanest average stroke in this set. Missing measures drop out."""
    scores = []
    for row in rates:
        now = row.get("now") or {}
        weighted = 0.0
        weight = 0.0
        for key, w in CLEAN_METRICS:
            vals = [r.get("now", {}).get(key) for r in rates]
            vals = [v for v in vals if v is not None]
            v = now.get(key)
            if v is None or len(vals) < 2:
                continue
            lo, hi = min(vals), max(vals)
            unit = 0.5 if hi - lo < 1e-6 else (hi - v) / (hi - lo)
            weighted += w * unit
            weight += w
        relative = weighted / weight if weight else 0.5
        keep = _clean_keep(now, row.get("ref"))
        scores.append(0.65 * relative + 0.35 * keep)
    return scores


def _choose_pace(session, rates):
    usable = [r for r in rates if r.get("split") and r["split"] > 0 and r.get("now")]
    if not usable:
        return None
    longest, total = _bouts(session.strokes)
    for row in usable:
        row["hold_s"] = _num(longest.get(row["spm"], 0.0), 1)
        row["total_s"] = _num(total.get(row["spm"], 0.0), 1)
    held = [r for r in usable if (r["hold_s"] or 0) >= PACE_LONG_S or (r["total_s"] or 0) >= PACE_TOTAL_S]
    if not held:
        held = [r for r in usable if (r["hold_s"] or 0) >= 15 or (r["total_s"] or 0) >= 30] or usable
    scale = [r for r in usable if r["n"] >= 5] or usable
    splits = [r["split"] for r in scale]
    slow, fast = max(splits), min(splits)
    span = slow - fast
    clean = {r["spm"]: s for r, s in zip(usable, _clean_scores(usable))}
    ranked = []
    for row in held:
        pace = 0.5 if span < 5 else (slow - row["split"]) / span
        hold = _hold_score(row["hold_s"] or 0, row["total_s"] or 0)
        tidy = clean.get(row["spm"], 0.5)
        # A fast split cannot carry a rate that was only a short burst.
        score = hold * (0.55 + 0.45 * pace) * (0.70 + 0.30 * tidy)
        ranked.append((score, row))
    ranked.sort(key=lambda item: (-item[0], item[1]["split"], -item[1]["hold_s"]))
    winner = ranked[0][1]
    burst = None
    quicker = [r for r in usable
               if r["spm"] > winner["spm"] and r["split"] + 3 < winner["split"]
               and (r["hold_s"] or 0) < PACE_LONG_S and (r["total_s"] or 0) < PACE_TOTAL_S]
    if quicker:
        burst = min(quicker, key=lambda r: (r["split"], -(r["hold_s"] or 0)))
    return {
        "spm": winner["spm"],
        "split": winner["split"],
        "hold_s": winner["hold_s"],
        "total_s": winner["total_s"],
        "n": winner["n"],
        "settled": (winner["hold_s"] or 0) >= 60 or (winner["total_s"] or 0) >= 120,
        "burst": None if burst is None else {
            "spm": burst["spm"],
            "split": burst["split"],
            "hold_s": burst["hold_s"],
            "total_s": burst["total_s"],
        },
    }


def session_trace(session, t0, t1, n):
    """Min/max surge over the window, so a zoomed-out session still shows the stroke shape."""
    t = session.t
    if len(t) == 0:
        return {"span": [0, 1], "t": [], "lo": [], "hi": [], "gps": []}
    origin = session.origin
    span0, span1 = 0.0, float(t[-1] - origin)
    if t0 is None:
        t0 = span0
    if t1 is None:
        t1 = span1
    t0 = max(span0, min(span1, t0))
    t1 = max(t0 + 0.05, min(span1, t1))
    lo = int(np.searchsorted(t, t0 + origin))
    hi = int(np.searchsorted(t, t1 + origin, side="right"))
    hi = min(len(t), max(hi, lo + 1))
    buckets = max(1, min(n, hi - lo))
    edges = np.linspace(lo, hi, buckets + 1).astype(int)
    ts, amin, amax, gps = [], [], [], []
    surge, speed = session.surge, session.gps
    for i in range(buckets):
        a0, a1 = int(edges[i]), int(edges[i + 1])
        if a1 <= a0:
            continue
        sl = surge[a0:a1]
        gs = speed[a0:a1]
        good = gs[gs >= 0]
        ts.append(float(t[a0] - origin))
        amin.append(float(sl.min()))
        amax.append(float(sl.max()))
        gps.append(float(good.mean()) if len(good) else None)
    return {
        "span": [_num(span0, 2), _num(span1, 2)],
        "t0": _num(t0, 3),
        "t1": _num(t1, 3),
        "t": _series(ts),
        "lo": _series(amin),
        "hi": _series(amax),
        "gps": [_num(v, 3) for v in gps],
    }


def session_overview(sid):
    imu = _imu_file(sid)
    if imu is None:
        raise FileNotFoundError(sid)
    data = overview.build(imu, _sessions.get(sid))
    if data is None:
        return {"has_seconds": False}
    data["has_seconds"] = True
    data["id"] = sid
    return data


_load_lock = threading.Lock()
# How many analysed sessions stay in memory. A small hosted server has 512 MB in all.
_cache_max = 100


def _remember(session):
    _sessions.pop(session.id, None)
    _sessions[session.id] = session
    while len(_sessions) > _cache_max:
        _sessions.pop(next(iter(_sessions)))


def _load_and_keep(path):
    """Analyses a recording, one at a time: two long ones together would not fit a small server."""
    with _load_lock:
        session = load_session(path)
        _remember(session)
    return session


def get_session(sid):
    if sid in _sessions:
        return _sessions[sid]
    imu = _imu_file(sid)
    if imu is None:
        raise FileNotFoundError(sid)
    with _load_lock:
        if sid in _sessions:
            return _sessions[sid]
        session = load_session(imu)
        _remember(session)
        return session


UPLOAD_HELP = "upload the _imu.csv, with the _seconds.csv, _laps.csv, _strokes.csv and _info.csv beside it when you have them"


def _upload_target(name):
    """Where an uploaded file goes. ValueError for a name that is not a session file."""
    base = os.path.basename(str(name).replace("\\", "/"))
    if not _UPLOAD_NAME.match(base) or not base.endswith(UPLOAD_SUFFIXES):
        raise ValueError(UPLOAD_HELP)
    folder = _uploads_dir()
    os.makedirs(folder, exist_ok=True)
    dest = os.path.join(folder, base)
    if not _inside_root(dest):
        raise ValueError(UPLOAD_HELP)
    if base.endswith("_imu.csv"):
        _sessions.pop(base[: -len("_imu.csv")], None)
    return dest


_PART_NAME = re.compile(rb'filename="([^"]*)"')


def save_multipart(rfile, content_type, length, target=_upload_target, block=1 << 20):
    """Reads a multipart/form-data body from rfile and writes each file part to disk as it
    arrives, a block at a time, so a 50 MB recording never sits in memory. Returns the paths."""
    m = re.search(r'boundary="?([^";]+)"?', content_type)
    if not m:
        raise ValueError(UPLOAD_HELP)
    delim = b"\r\n--" + m.group(1).encode("ascii")
    keep = len(delim) + 4
    buf = b"\r\n"  # the first boundary has no line break before it; this gives it one
    left = length
    state = "preamble"
    out = None
    tmp = None
    saved = []
    try:
        while True:
            if left > 0 and state != "done":
                data = rfile.read(min(block, left))
                if not data:
                    raise ValueError("the upload stopped before the end")
                left -= len(data)
                buf += data
            elif left <= 0 and state == "body" and delim not in buf:
                raise ValueError("the upload stopped before the end")
            progressed = True
            while progressed:
                progressed = False
                if state == "preamble":
                    i = buf.find(delim)
                    if i >= 0:
                        buf = buf[i + len(delim):]
                        state = "after"
                        progressed = True
                    else:
                        buf = buf[-keep:]
                elif state == "after":
                    if len(buf) >= 2:
                        if buf[:2] == b"--":
                            state = "done"
                        else:
                            buf = buf[2:]  # the line break after the boundary
                            state = "headers"
                        progressed = True
                elif state == "headers":
                    i = buf.find(b"\r\n\r\n")
                    if i >= 0:
                        name = _PART_NAME.search(buf[:i])
                        buf = buf[i + 4:]
                        out = None
                        if name and name.group(1):
                            path = target(name.group(1).decode("utf-8", "replace"))
                            # written beside the target and moved in when complete, so a
                            # second upload of the same name never reads a half-written file
                            tmp = "%s.%d.part" % (path, threading.get_ident())
                            out = open(tmp, "wb")
                        state = "body"
                        progressed = True
                elif state == "body":
                    i = buf.find(delim)
                    if i >= 0:
                        if out:
                            out.write(buf[:i])
                            out.close()
                            try:
                                os.replace(tmp, path)
                            except PermissionError:  # Windows: that recording is being analysed
                                raise ValueError("this recording is being analysed right now, try again in a moment")
                            saved.append(path)
                        out = None
                        buf = buf[i + len(delim):]
                        state = "after"
                        progressed = True
                    elif len(buf) > keep:
                        if out:
                            out.write(buf[:-keep])
                        buf = buf[-keep:]
            if state == "done" or (left <= 0 and not progressed and state != "body"):
                break
    finally:
        if out:
            out.close()
            try:
                os.remove(tmp)
            except OSError:
                pass
    return saved


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("[stroke] " + (fmt % args) + "\n")

    def _send(self, code, body, content_type):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj), "application/json")

    def do_GET(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        try:
            if url.path in ("/", "/index.html"):
                self._send(200, open(os.path.join(PAGE, "index.html"), "rb").read(), "text/html; charset=utf-8")
            elif url.path in STATIC:
                name, kind = STATIC[url.path]
                self._send(200, open(os.path.join(PAGE, *name.split("/")), "rb").read(), kind)
            elif url.path == "/health":
                self._send(200, "ok", "text/plain; charset=utf-8")
            elif url.path == "/api/sessions":
                self._json(200, list_sessions(_root))
            elif url.path == "/api/session":
                sid = q.get("id", [""])[0]
                crew = int(q.get("crew", ["1"])[0])
                lang = q.get("lang", ["en"])[0]
                view = q.get("view", ["stroke"])[0]
                self._json(200, session_index(get_session(sid), crew, lang, view))
            elif url.path == "/api/stroke":
                sid = q.get("id", [""])[0]
                index = int(q.get("i", ["0"])[0])
                crew = int(q.get("crew", ["1"])[0])
                view = q.get("view", ["stroke"])[0]
                lang = q.get("lang", ["en"])[0]
                session = get_session(sid)
                if index < 0 or index >= len(session.strokes):
                    self._json(404, {"error": "no such stroke"})
                    return
                self._json(200, stroke_detail(session, index, crew, view, lang))
            elif url.path == "/api/rates":
                sid = q.get("id", [""])[0]
                crew = int(q.get("crew", ["1"])[0])
                lang = q.get("lang", ["en"])[0]
                self._json(200, rate_report(get_session(sid), crew, lang))
            elif url.path == "/api/overview":
                self._json(200, session_overview(q.get("id", [""])[0]))
            elif url.path == "/api/guide":
                self._json(200, guide_example())
            elif url.path == "/api/motion":
                session = get_session(q.get("id", [""])[0])
                self._json(200, session_motion(session, session.origin))
            elif url.path == "/api/export":
                sid = q.get("id", [""])[0]
                imu = _imu_file(sid)
                if imu is None:
                    raise FileNotFoundError(sid)
                try:
                    data, filename, kind = overview.export_file(imu, q.get("format", ["gpx"])[0], sid)
                except ValueError as exc:
                    self._json(400, {"error": str(exc)})
                    return
                self.send_response(200)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Content-Disposition", 'attachment; filename="%s"' % filename)
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(data)
            elif url.path == "/api/trace":
                sid = q.get("id", [""])[0]
                n = int(q.get("n", ["900"])[0])
                n = max(40, min(2000, n))
                raw0, raw1 = q.get("t0", [None])[0], q.get("t1", [None])[0]
                t0 = float(raw0) if raw0 not in (None, "") else None
                t1 = float(raw1) if raw1 not in (None, "") else None
                self._json(200, session_trace(get_session(sid), t0, t1, n))
            else:
                self._send(404, "not found", "text/plain")
        except FileNotFoundError as exc:
            self._json(404, {"error": "session not found: %s" % exc})
        except Exception as exc:
            traceback.print_exc()
            self._json(500, {"error": str(exc)})

    def _upload(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0 or length > MAX_UPLOAD:
            self._json(400, {"error": "the upload has to be one recording under 80 MB"})
            return
        ctype = self.headers.get("Content-Type", "")
        if "multipart/form-data" not in ctype:
            self._json(400, {"error": "upload the _imu.csv, with the _seconds.csv, _laps.csv, _strokes.csv and _info.csv beside it when you have them"})
            return
        saved = save_multipart(self.rfile, ctype, length)
        if not any(path.endswith("_imu.csv") for path in saved):
            self._json(400, {"error": "upload the _imu.csv, with the _seconds.csv, _laps.csv, _strokes.csv and _info.csv beside it when you have them"})
            return
        imu = next(path for path in saved if path.endswith("_imu.csv"))
        session = _load_and_keep(imu)
        self._json(200, {"id": session.id, "name": session.name, "strokes": len(session.strokes)})

    def do_POST(self):
        url = urlparse(self.path)
        try:
            if url.path == "/api/upload":
                self._upload()
                return
            n = int(self.headers.get("Content-Length", "0") or "0")
            body = json.loads(self.rfile.read(n) or b"{}")
            if url.path != "/api/open":
                self._json(404, {"error": "not found"})
                return
            path = str(body.get("path", "")).strip().strip('"')
            if not path or not os.path.isfile(path) or not _inside_root(path):
                self._json(400, {"error": "that file is not in the recordings folder"})
                return
            session = _load_and_keep(path)
            self._json(200, {"id": session.id, "name": session.name, "strokes": len(session.strokes)})
        except ValueError as exc:
            self._json(400, {"error": str(exc)})
        except Exception as exc:
            traceback.print_exc()
            self._json(500, {"error": str(exc)})


def main():
    global _root
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--recordings", default=None)
    ap.add_argument("--open", action="store_true", help="open the browser")
    args = ap.parse_args()
    # Render sets PORT and reaches the process on every interface. Locally neither
    # is set, so the viewer stays on this machine only.
    hosted = "PORT" in os.environ
    if hosted:
        global _cache_max
        _cache_max = 2
    port = args.port if args.port is not None else int(os.environ.get("PORT", "8765"))
    host = "0.0.0.0" if hosted else "127.0.0.1"
    _root = args.recordings or os.environ.get("RECORDINGS_DIR", "recordings")
    os.makedirs(_root, exist_ok=True)
    server = ThreadingHTTPServer((host, port), Handler)
    url = "http://127.0.0.1:%d/" % port
    print("stroke viewer at", url)
    print("sessions from", os.path.abspath(_root))
    if args.open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
