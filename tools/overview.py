"""Session overview for the stroke viewer: the same figures as the device's Session review.

Reads S####_*seconds.csv (track, speed, rate, heart rate), *laps.csv, *strokes.csv and *info.csv and
returns the summary, the per-second curves and the track. Mirrors src/review.cpp so a session reads the
same on the web as on the device. Also builds the GPX / TCX download for Strava, Garmin Connect, etc.
"""
import os
import tempfile

import export

SMOOTH_HALF = 2          # 5 s centred average, like the device
MOVING_MPS = 1.0         # slower is drifting or stopped
MIN_SPLIT_SPEED = 0.5


def _round(value, digits):
    return None if value is None else round(value, digits)


def prefix_of(imu_path):
    """'.../S0018_BBR_race_imu.csv' -> '.../S0018_BBR_race_' (what export.load expects)."""
    return imu_path[: -len("imu.csv")]


def _percentile(sorted_values, p):
    return sorted_values[min(len(sorted_values) - 1, int(len(sorted_values) * p))]


def _best_500(el, dist):
    """Fastest 500 m: the shortest time between two rows that are 500 m apart, as a split."""
    best = 0.0
    i = 0
    for j in range(len(el)):
        while i + 1 < j and dist[j] - dist[i + 1] >= 500:
            i += 1
        if dist[j] - dist[i] >= 500:
            t = (el[j] - el[i]) * 500 / (dist[j] - dist[i])
            if not best or t < best:
                best = t
    return best


def _stroke_figures(prefix, session):
    """(strokes, average rate, metres per stroke). The device's strokes file when there is one,
    else the strokes the viewer found by replaying the recording."""
    rows = export.read_csv(prefix + "strokes.csv")
    if rows:
        rates = [export.num(r, "rate_spm") for r in rows]
        dps = [export.num(r, "dps_m") for r in rows]
        rates = [x for x in rates if x > 0]
        dps = [x for x in dps if x > 0]
        return len(rows), (sum(rates) / len(rates) if rates else 0.0), (sum(dps) / len(dps) if dps else 0.0)
    if session is not None and getattr(session, "strokes", None):
        rates = [float(s.get("rate", 0) or 0) for s in session.strokes]
        rates = [x for x in rates if x > 0]
        dps = [float(s.get("dps", 0) or 0) for s in session.strokes]
        dps = [x for x in dps if x > 0]
        return len(session.strokes), (sum(rates) / len(rates) if rates else 0.0), (sum(dps) / len(dps) if dps else 0.0)
    return 0, 0.0, 0.0


def build(imu_path, session=None):
    """Overview of one session, or None when it has no per-second log."""
    prefix = prefix_of(imu_path)
    s = export.load(prefix)
    if not s or not s["points"]:
        return None
    pts = s["points"]
    n = len(pts)
    el = [p["elapsed"] for p in pts]
    raw = [p["speed"] for p in pts]
    dist = [p["distance"] for p in pts]
    rate = [p["rate"] for p in pts]
    hr = [p["hr"] for p in pts]

    speed = []
    for i in range(n):
        lo, hi = max(0, i - SMOOTH_HALF), min(n - 1, i + SMOOTH_HALF)
        speed.append(sum(raw[lo:hi + 1]) / (hi - lo + 1))

    fixes = [i for i, p in enumerate(pts) if p["fix"]]
    has_gps = len(fixes) >= 2
    hrs = [x for x in hr if x > 0]
    strokes, avg_rate, dps = _stroke_figures(prefix, session)

    duration = el[-1]
    distance = dist[-1] if has_gps else 0.0
    avg_split = duration * 500 / distance if distance > 10 else 0.0
    max_speed = max(speed) if has_gps else 0.0

    moving = sorted(v for v in speed if v >= MOVING_MPS)
    if len(moving) >= 5:
        slow, fast = _percentile(moving, 0.1), _percentile(moving, 0.9)
    else:
        slow, fast = MOVING_MPS, 4.0

    info = {r["key"]: r["value"] for r in export.read_csv(prefix + "info.csv")}
    race = None
    if info.get("kind") == "race":
        race = {
            "segment": info.get("segment", ""),
            "course_m": export.num(info, "course_m"),
            "time_s": export.num(info, "race_time_s"),
            "distance_m": export.num(info, "race_distance_m"),
        }

    laps = []
    for r in export.read_csv(prefix + "laps.csv"):
        laps.append({
            "lap": int(export.num(r, "lap")),
            "time_s": export.num(r, "lap_time_s"),
            "distance_m": export.num(r, "lap_distance_m"),
            "split_s": export.num(r, "avg_split_s"),
            "rate": export.num(r, "avg_rate_spm"),
            "strokes": int(export.num(r, "strokes")),
            "hr": export.num(r, "avg_hr_bpm"),
            "hr_max": export.num(r, "max_hr_bpm"),
        })

    return {
        "start_utc": export.iso(s["start"]),
        "stats": {
            "duration_s": duration,
            "distance_m": distance,
            "avg_split_s": avg_split,
            "best_500_s": _best_500(el, dist) if has_gps else 0.0,
            "avg_rate": avg_rate,
            "strokes": strokes,
            "dps_m": dps if has_gps else 0.0,
            "max_speed_mps": max_speed,
            "hr_avg": sum(hrs) / len(hrs) if hrs else 0.0,
            "hr_max": max(hrs) if hrs else 0.0,
        },
        "has_gps": has_gps,
        "race": race,
        "scale": {"slow_mps": slow, "fast_mps": fast},
        "laps": laps,
        # One entry per recorded second; lat/lon are null where there was no fix.
        "t": [_round(x, 1) for x in el],
        "lat": [_round(p["lat"], 6) if p["fix"] else None for p in pts],
        "lon": [_round(p["lon"], 6) if p["fix"] else None for p in pts],
        "speed": [_round(v, 2) for v in speed],
        "split": [_round(500.0 / v, 1) if v >= MIN_SPLIT_SPEED else None for v in speed],
        "rate": [_round(x, 1) for x in rate],
        "hr": [int(round(x)) if x > 0 else None for x in hr],
        "distance": [_round(x, 1) for x in dist],
    }


def export_file(imu_path, fmt, name):
    """(bytes, file name, content type) of the session as GPX or TCX. ValueError if it cannot be made."""
    prefix = prefix_of(imu_path)
    s = export.load(prefix)
    if not s or not s["points"]:
        raise ValueError("this session has no per-second log (_seconds.csv); upload it with the recording")
    stem = os.path.basename(prefix).rstrip("_")
    pretty = name.replace("_", " ")
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, stem + "." + fmt)
        if fmt == "gpx":
            if not export.write_gpx(path, pretty, s):
                raise ValueError("this session has no GPS positions, so there is no track to export")
            kind = "application/gpx+xml"
        elif fmt == "tcx":
            export.write_tcx(path, pretty, s)
            kind = "application/vnd.garmin.tcx+xml"
        else:
            raise ValueError("format must be gpx or tcx")
        with open(path, "rb") as f:
            return f.read(), os.path.basename(path), kind
