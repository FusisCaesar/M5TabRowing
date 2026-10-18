"""Export recorded sessions as GPX and TCX for Strava, Garmin Connect, etc.

Usage:
  python tools/export.py 21                 # recordings/S0021_*  -> recordings/S0021.gpx + .tcx
  python tools/export.py 21 22 --format gpx
  python tools/export.py all --dir recordings --out exports

Fetch the session first (python tools/fetch.py COM10 21) or copy it from the card.
GPX carries position, time, speed, stroke rate (as cadence) and heart rate and is marked as rowing,
which Strava picks up. TCX adds distance and one lap per LAP press; set the sport to
rowing after the upload, TCX has no rowing type.
"""
import argparse
import csv
import datetime as dt
import glob
import os
import re
import sys
from xml.sax.saxutils import escape

SECONDS_RE = re.compile(r"S(\d{4})(?:_([\w-]+))?_seconds\.csv$")


def find_sessions(folder):
    found = {}
    for path in glob.glob(os.path.join(folder, "S*_seconds.csv")):
        m = SECONDS_RE.search(os.path.basename(path))
        if m:
            found[int(m.group(1))] = (path[: -len("seconds.csv")], m.group(2) or "")
    return found


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def num(row, key, default=0.0):
    try:
        return float(row.get(key) or default)
    except ValueError:
        return default


def parse_utc(text):
    try:
        return dt.datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)
    except (TypeError, ValueError):
        return None


def iso(t):
    return t.strftime("%Y-%m-%dT%H:%M:%SZ")


def load(prefix):
    seconds = read_csv(prefix + "seconds.csv")
    info = {r["key"]: r["value"] for r in read_csv(prefix + "info.csv")}
    laps = read_csv(prefix + "laps.csv")
    if not seconds:
        return None

    # Times: the row's own UTC, else the session start plus elapsed time.
    start = parse_utc(info.get("start_utc"))
    if start is None:
        for r in seconds:
            t = parse_utc(r.get("utc"))
            if t:
                start = t - dt.timedelta(seconds=num(r, "elapsed_s"))
                break
    if start is None:
        print("  no GPS time in this session; using 2000-01-01 as the start date")
        start = dt.datetime(2000, 1, 1, tzinfo=dt.timezone.utc)

    points = []
    for r in seconds:
        el = num(r, "elapsed_s")
        lat, lon = num(r, "lat"), num(r, "lon")
        points.append({
            "elapsed": el,
            "time": parse_utc(r.get("utc")) or start + dt.timedelta(seconds=el),
            "lat": lat,
            "lon": lon,
            "fix": lat != 0 or lon != 0,
            "speed": num(r, "speed_mps"),
            "distance": num(r, "distance_m"),
            "rate": num(r, "rate_spm"),
            "hr": num(r, "hr_bpm"),
        })
    return {"start": start, "points": points, "laps": laps}


def write_gpx(path, name, s):
    out = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<gpx version="1.1" creator="M5Tab Rowing" xmlns="http://www.topografix.com/GPX/1/1"'
        ' xmlns:gpxtpx="http://www.garmin.com/xmlschemas/TrackPointExtension/v2">',
        f"  <metadata><name>{escape(name)}</name><time>{iso(s['start'])}</time></metadata>",
        f"  <trk><name>{escape(name)}</name><type>rowing</type><trkseg>",
    ]
    n = 0
    for p in s["points"]:
        if not p["fix"]:
            continue
        n += 1
        hr = f"<gpxtpx:hr>{round(p['hr'])}</gpxtpx:hr>" if p["hr"] > 0 else ""
        out.append(
            f'    <trkpt lat="{p["lat"]:.7f}" lon="{p["lon"]:.7f}"><time>{iso(p["time"])}</time>'
            f"<extensions><gpxtpx:TrackPointExtension>{hr}<gpxtpx:cad>{round(p['rate'])}</gpxtpx:cad>"
            f"<gpxtpx:speed>{p['speed']:.2f}</gpxtpx:speed></gpxtpx:TrackPointExtension></extensions></trkpt>"
        )
    out += ["  </trkseg></trk>", "</gpx>", ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    return n


def write_tcx(path, name, s):
    pts = s["points"]
    # Lap boundaries from the laps file; whatever follows the last LAP press is a lap too.
    ends = [num(r, "elapsed_s") for r in s["laps"]]
    if not ends or ends[-1] < pts[-1]["elapsed"] - 1:
        ends.append(pts[-1]["elapsed"] + 1)

    out = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<TrainingCenterDatabase xmlns="http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2"'
        ' xmlns:ns3="http://www.garmin.com/xmlschemas/ActivityExtension/v2">',
        "  <Activities>",
        '    <Activity Sport="Other">',
        f"      <Id>{iso(s['start'])}</Id>",
    ]
    lo = 0.0
    prev_dist = 0.0
    for end in ends:
        lap = [p for p in pts if lo <= p["elapsed"] < end]
        lo = end
        if not lap:
            continue
        dist = lap[-1]["distance"] - prev_dist
        prev_dist = lap[-1]["distance"]
        rates = [p["rate"] for p in lap if p["rate"] > 0]
        hrs = [p["hr"] for p in lap if p["hr"] > 0]
        out += [
            f'      <Lap StartTime="{iso(lap[0]["time"])}">',
            f"        <TotalTimeSeconds>{lap[-1]['elapsed'] - lap[0]['elapsed'] + 1:.1f}</TotalTimeSeconds>",
            f"        <DistanceMeters>{dist:.1f}</DistanceMeters>",
            f"        <MaximumSpeed>{max(p['speed'] for p in lap):.2f}</MaximumSpeed>",
            "        <Calories>0</Calories>",
        ]
        if hrs:
            out += [
                f"        <AverageHeartRateBpm><Value>{round(sum(hrs) / len(hrs))}</Value></AverageHeartRateBpm>",
                f"        <MaximumHeartRateBpm><Value>{round(max(hrs))}</Value></MaximumHeartRateBpm>",
            ]
        out.append("        <Intensity>Active</Intensity>")
        if rates:
            out.append(f"        <Cadence>{round(sum(rates) / len(rates))}</Cadence>")
        out += ["        <TriggerMethod>Manual</TriggerMethod>", "        <Track>"]
        for p in lap:
            pos = (f"<Position><LatitudeDegrees>{p['lat']:.7f}</LatitudeDegrees>"
                   f"<LongitudeDegrees>{p['lon']:.7f}</LongitudeDegrees></Position>") if p["fix"] else ""
            cad = f"<Cadence>{round(p['rate'])}</Cadence>" if p["rate"] > 0 else ""
            hr = f"<HeartRateBpm><Value>{round(p['hr'])}</Value></HeartRateBpm>" if p["hr"] > 0 else ""
            out.append(
                f"          <Trackpoint><Time>{iso(p['time'])}</Time>{pos}"
                f"<DistanceMeters>{p['distance']:.1f}</DistanceMeters>{hr}{cad}"
                f"<Extensions><ns3:TPX><ns3:Speed>{p['speed']:.2f}</ns3:Speed></ns3:TPX></Extensions></Trackpoint>"
            )
        out += ["        </Track>", "      </Lap>"]
    out += [f"      <Notes>{escape(name)}</Notes>", "    </Activity>", "  </Activities>",
            "</TrainingCenterDatabase>", ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sessions", nargs="+", help="session numbers, or 'all'")
    ap.add_argument("--dir", default="recordings", help="folder with the session CSV files")
    ap.add_argument("--out", help="output folder (default: same as --dir)")
    ap.add_argument("--format", choices=["gpx", "tcx", "both"], default="both")
    a = ap.parse_args()

    found = find_sessions(a.dir)
    if not found:
        sys.exit(f"no sessions in {a.dir}; fetch one first: python tools/fetch.py COM10 latest")
    wanted = sorted(found) if a.sessions == ["all"] else [int(x) for x in a.sessions]
    out_dir = a.out or a.dir
    os.makedirs(out_dir, exist_ok=True)

    for number in wanted:
        if number not in found:
            print(f"S{number:04d}: not in {a.dir}")
            continue
        prefix, label = found[number]
        name = label.replace("_", " ") if label else f"Rowing session {number}"
        s = load(prefix)
        if not s:
            print(f"S{number:04d}: empty")
            continue
        base = os.path.join(out_dir, f"S{number:04d}" + (f"_{label}" if label else ""))
        msg = []
        if a.format in ("gpx", "both"):
            n = write_gpx(base + ".gpx", name, s)
            msg.append(f"{base}.gpx ({n} points)" if n else f"{base}.gpx (no GPS positions!)")
        if a.format in ("tcx", "both"):
            write_tcx(base + ".tcx", name, s)
            msg.append(f"{base}.tcx")
        last = s["points"][-1]
        print(f"S{number:04d} {name}: {last['distance']:.0f} m in {last['elapsed']:.0f} s -> " + ", ".join(msg))


if __name__ == "__main__":
    main()
