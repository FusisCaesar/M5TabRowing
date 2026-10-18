"""Build a small offline waterway map (.rmap) for the Tab5 from OpenStreetMap.

Downloads water, bridges, locks, weirs, piers and boat clubs for one area via the
Overpass API, projects them to local metres, simplifies, and writes a compact
binary the Tab5 draws directly. Also writes a preview PNG so you can check it.

Usage:
  python tools/make_map.py --center 52.09,5.11 --radius-km 6 --name "Utrecht" --out maps/utrecht.rmap
  python tools/make_map.py --bbox 52.0,5.0,52.15,5.25 --name "Area" --out maps/area.rmap

Dependencies: pip install requests shapely osm2geojson pillow

The map data is (c) OpenStreetMap contributors, ODbL. The Tab5 shows the
attribution on screen; keep it when you share screenshots.
"""
import argparse
import math
import os
import struct
import sys

import requests

try:
    import osm2geojson
    from shapely.geometry import LineString, MultiLineString, MultiPolygon, Point, Polygon, box, shape
    from shapely.ops import transform as shp_transform
except ImportError as e:
    sys.exit(f"missing dependency: {e}. Run: pip install requests shapely osm2geojson pillow")

OVERPASS_URL = "https://overpass-api.de/api/interpreter"

# Feature kinds, mirrored in src/map_data.h.
WATER_AREA, WATER_LINE, BRIDGE, LOCK, WEIR, PIER, CLUB = range(7)
# Feature flags (bitfield), mirrored in src/map_data.h.
F_LINE = 1 << 0   # geometry is an open polyline (else filled rings)
F_POINT = 1 << 1  # geometry is a single point

MAGIC = b"RMAP"
VERSION = 1

# Default width (metres) for water lines that have no width tag.
WATERWAY_WIDTH = {"river": 24.0, "canal": 14.0, "stream": 4.0, "ditch": 2.0, "drain": 2.0, "tidal_channel": 12.0}


def build_query(south, west, north, east):
    b = f"{south},{west},{north},{east}"
    return f"""
[out:json][timeout:180];
(
  way["natural"="water"]({b});
  relation["natural"="water"]({b});
  way["waterway"="riverbank"]({b});
  relation["waterway"="riverbank"]({b});
  way["water"]({b});
  way["waterway"~"^(river|canal|stream|ditch|drain|tidal_channel)$"]({b});
  way["man_made"="bridge"]({b});
  way["bridge"="yes"]({b});
  way["waterway"="lock_gate"]({b});
  node["waterway"="lock_gate"]({b});
  way["lock"="yes"]({b});
  way["waterway"~"^(weir|dam)$"]({b});
  way["man_made"="pier"]({b});
  way["leisure"="marina"]({b});
  node["leisure"="marina"]({b});
  way["sport"~"rowing|canoe"]({b});
  node["sport"~"rowing|canoe"]({b});
  way["leisure"="slipway"]({b});
  node["leisure"="slipway"]({b});
);
out geom;
"""


def is_water_area(tags):
    return tags.get("natural") == "water" or tags.get("waterway") == "riverbank" or "water" in tags


def classify_all(tags, geom):
    """All (kind, flags, width_m) to emit for one OSM feature."""
    cls = classify(tags, geom)
    if cls is None:
        return []
    # A rowing course or marina mapped as a water area is both water and a club.
    if cls[0] == CLUB and geom.geom_type in ("Polygon", "MultiPolygon") and is_water_area(tags):
        return [(WATER_AREA, 0, 0.0), cls]
    return [cls]


def classify(tags, geom):
    """Return (kind, flags, width_m) or None to skip."""
    gt = geom.geom_type
    is_area = gt in ("Polygon", "MultiPolygon")
    is_line = gt in ("LineString", "MultiLineString")
    is_point = gt == "Point"

    waterway = tags.get("waterway", "")
    natural = tags.get("natural", "")
    man_made = tags.get("man_made", "")
    leisure = tags.get("leisure", "")
    sport = tags.get("sport", "")

    if sport in ("rowing", "canoe") or leisure in ("marina", "slipway"):
        # Represent clubs/marinas/slipways as a single point (centroid).
        return CLUB, F_POINT, 0.0
    if waterway in ("weir", "dam"):
        return WEIR, F_LINE if is_line else 0, 0.0
    if waterway == "lock_gate" or tags.get("lock") == "yes":
        if is_point:
            return LOCK, F_POINT, 0.0
        return LOCK, F_LINE if is_line else 0, 0.0
    if man_made == "bridge" or tags.get("bridge") == "yes":
        return BRIDGE, F_LINE if is_line else 0, 6.0
    if man_made == "pier":
        return PIER, F_LINE if is_line else 0, 3.0
    if is_area and is_water_area(tags):
        return WATER_AREA, 0, 0.0
    if is_line and waterway in WATERWAY_WIDTH:
        try:
            width = float(str(tags.get("width", "")).split()[0])
        except (ValueError, IndexError):
            width = WATERWAY_WIDTH[waterway]
        return WATER_LINE, F_LINE, width
    return None


def rings_of(geom):
    """Yield coordinate rings (lists of (x, y)) for a projected geometry."""
    gt = geom.geom_type
    if gt == "Polygon":
        yield list(geom.exterior.coords)
        for interior in geom.interiors:
            yield list(interior.coords)
    elif gt == "MultiPolygon":
        for poly in geom.geoms:
            yield from rings_of(poly)
    elif gt == "LineString":
        yield list(geom.coords)
    elif gt == "MultiLineString":
        for line in geom.geoms:
            yield list(line.coords)
    elif gt == "Point":
        yield [(geom.x, geom.y)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bbox", help="south,west,north,east in degrees")
    ap.add_argument("--center", help="lat,lon in degrees (use with --radius-km)")
    ap.add_argument("--radius-km", type=float, default=5.0)
    ap.add_argument("--name", default="Map")
    ap.add_argument("--out", required=True, help="output .rmap path")
    ap.add_argument("--simplify-m", type=float, default=0.5)
    ap.add_argument("--preview", default=None, help="preview PNG path (default: <out>.png)")
    args = ap.parse_args()

    if args.bbox:
        south, west, north, east = (float(v) for v in args.bbox.split(","))
    elif args.center:
        lat, lon = (float(v) for v in args.center.split(","))
        dlat = args.radius_km / 111.32
        dlon = args.radius_km / (111.32 * math.cos(math.radians(lat)))
        south, west, north, east = lat - dlat, lon - dlon, lat + dlat, lon + dlon
    else:
        ap.error("give --bbox or --center")

    olat = (south + north) / 2.0
    olon = (west + east) / 2.0
    mlon = 111320.0 * math.cos(math.radians(olat))
    mlat = 111320.0

    def project(lon, lat):
        return ((lon - olon) * mlon, (lat - olat) * mlat)

    print(f"[make_map] querying Overpass for {south:.4f},{west:.4f},{north:.4f},{east:.4f} ...")
    headers = {"User-Agent": "M5TabRowing-make_map/1.0 (offline rowing map builder)"}
    resp = requests.post(OVERPASS_URL, data={"data": build_query(south, west, north, east)},
                         headers=headers, timeout=240)
    resp.raise_for_status()
    gj = osm2geojson.json2geojson(resp.json())
    print(f"[make_map] {len(gj['features'])} OSM features, classifying ...")

    clip = box(*project(west, south), *project(east, north))
    features = []  # (kind, flags, width_dm, bbox, rings)
    for feat in gj["features"]:
        props = feat["properties"]
        tags = props.get("tags", {})
        try:
            geom = shape(feat["geometry"])
        except (ValueError, AttributeError):
            continue
        if geom.is_empty:
            continue
        # shapely transform wants a function (x, y) -> (x, y); apply per-coordinate.
        projected = shp_transform(lambda x, y, z=None: project(x, y), geom)
        name = tags.get("name", "")

        for kind, flags, width_m in classify_all(tags, geom):
            if flags & F_POINT:
                pgeom = projected.centroid
            else:
                pgeom = projected.intersection(clip)
                if pgeom.is_empty:
                    continue
                pgeom = pgeom.simplify(args.simplify_m)
                if pgeom.is_empty:
                    continue

            rings = []
            for ring in rings_of(pgeom):
                # To decimetres, deduplicate consecutive identical points.
                pts = []
                for x, y in ring:
                    ix, iy = int(round(x * 10)), int(round(y * 10))
                    if not pts or pts[-1] != (ix, iy):
                        pts.append((ix, iy))
                # Drop a closing duplicate; the device closes rings itself.
                if len(pts) > 1 and pts[0] == pts[-1]:
                    pts.pop()
                min_pts = 1 if flags & F_POINT else (2 if flags & F_LINE else 3)
                if len(pts) >= min_pts:
                    rings.append(pts)
            if not rings:
                continue

            xs = [p[0] for r in rings for p in r]
            ys = [p[1] for r in rings for p in r]
            bbox = (min(xs), min(ys), max(xs), max(ys))
            features.append((kind, flags, int(round(width_m * 10)), bbox, name[:23], rings))

    if not features:
        sys.exit("[make_map] no water features found; widen the area?")

    write_rmap(args.out, args.name, olat, olon, features)
    counts = {}
    for f in features:
        counts[f[0]] = counts.get(f[0], 0) + 1
    print(f"[make_map] wrote {args.out}: {len(features)} features {counts}")

    preview = args.preview or (args.out + ".png")
    write_preview(preview, features)
    print(f"[make_map] preview {preview}")


def write_rmap(path, name, olat, olon, features):
    all_bx = [f[3][0] for f in features] + [f[3][2] for f in features]
    all_by = [f[3][1] for f in features] + [f[3][3] for f in features]
    minx, maxx = min(all_bx), max(all_bx)
    miny, maxy = min(all_by), max(all_by)
    ring_total = sum(len(f[5]) for f in features)
    point_total = sum(len(r) for f in features for r in f[5])

    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(MAGIC)
        fh.write(struct.pack("<HH", VERSION, 0))
        fh.write(struct.pack("<dd", olat, olon))
        fh.write(struct.pack("<iiii", minx, miny, maxx, maxy))
        fh.write(struct.pack("<32s", name.encode("utf-8")[:31]))
        fh.write(struct.pack("<III", len(features), ring_total, point_total))
        for kind, flags, width_dm, bbox, fname, rings in features:
            fh.write(struct.pack("<BBH", kind, flags, min(width_dm, 0xFFFF)))
            fh.write(struct.pack("<iiii", *bbox))
            nb = fname.encode("utf-8")[:23]
            fh.write(struct.pack("<B", len(nb)))
            fh.write(nb)
            fh.write(struct.pack("<H", len(rings)))
            for ring in rings:
                fh.write(struct.pack("<H", len(ring)))
                for x, y in ring:
                    fh.write(struct.pack("<ii", x, y))


def write_preview(path, features, size=900):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("[make_map] pillow not installed, skipping preview")
        return
    all_bx = [f[3][0] for f in features] + [f[3][2] for f in features]
    all_by = [f[3][1] for f in features] + [f[3][3] for f in features]
    minx, maxx, miny, maxy = min(all_bx), max(all_bx), min(all_by), max(all_by)
    span = max(maxx - minx, maxy - miny) or 1
    scale = (size - 20) / span

    def sx(x):
        return 10 + (x - minx) * scale

    def sy(y):
        return size - 10 - (y - miny) * scale  # flip: north up

    img = Image.new("RGB", (size, size), (235, 230, 220))
    d = ImageDraw.Draw(img)
    water = (120, 170, 220)
    for kind, flags, width_dm, bbox, fname, rings in features:
        if kind in (WATER_AREA,) and not (flags & (F_LINE | F_POINT)):
            for ring in rings:
                d.polygon([(sx(x), sy(y)) for x, y in ring], fill=water)
    for kind, flags, width_dm, bbox, fname, rings in features:
        if flags & F_POINT:
            x, y = rings[0][0]
            d.ellipse([sx(x) - 4, sy(y) - 4, sx(x) + 4, sy(y) + 4], fill=(200, 60, 60))
        elif flags & F_LINE:
            col = {WATER_LINE: water, BRIDGE: (90, 90, 90), LOCK: (200, 60, 60),
                   WEIR: (160, 100, 40), PIER: (120, 120, 120)}.get(kind, (80, 80, 80))
            w = max(1, int(width_dm / 10 * scale)) if kind == WATER_LINE else 2
            for ring in rings:
                d.line([(sx(x), sy(y)) for x, y in ring], fill=col, width=w)
    d.text((10, size - 18), "(c) OpenStreetMap contributors", fill=(60, 60, 60))
    img.save(path)


if __name__ == "__main__":
    main()
