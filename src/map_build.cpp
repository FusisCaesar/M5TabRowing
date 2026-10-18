#include "map_build.h"

#include <SD_MMC.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#include <vector>

#include "map_data.h"

namespace mapbuild {
namespace {

constexpr double SIMPLIFY_M = 0.5;
constexpr uint16_t VERSION = 1;
constexpr size_t MAX_RING_POINTS = 65535;

struct P {
    double x, y;
};
using Line = std::vector<P>;

inline bool same(const P& a, const P& b) { return a.x == b.x && a.y == b.y; }

struct Rect {
    double minx, miny, maxx, maxy;
};

// ---- minimal JSON reader over a NUL-terminated buffer ----------------------

struct Json {
    const char* p;
    bool ok = true;

    void ws() {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }
    char peek() {
        ws();
        return *p;
    }
    bool eat(char c) {
        ws();
        if (*p != c) return false;
        p++;
        return true;
    }

    // Reads a string, decoding escapes, truncated to cap-1 bytes. cap 0 skips it.
    void str(char* dst, size_t cap) {
        ws();
        if (*p != '"') {
            ok = false;
            return;
        }
        p++;
        size_t n = 0;
        auto put = [&](unsigned c) {
            if (n + 1 < cap) dst[n++] = (char)c;
        };
        while (*p && *p != '"') {
            char c = *p++;
            if (c != '\\') {
                put((uint8_t)c);
                continue;
            }
            char e = *p;
            if (!e) break;
            p++;
            switch (e) {
                case 'n': put('\n'); break;
                case 't': put('\t'); break;
                case 'r':
                case 'b':
                case 'f': break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4 && isxdigit((unsigned char)*p); i++, p++)
                        cp = cp * 16 + (isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10);
                    if (cp >= 0xD800 && cp <= 0xDFFF) {  // surrogate pair: no glyphs for those anyway
                        put('?');
                        if (p[0] == '\\' && p[1] == 'u') p += 6;
                    } else if (cp < 0x80) {
                        put(cp);
                    } else if (cp < 0x800) {
                        put(0xC0 | (cp >> 6));
                        put(0x80 | (cp & 0x3F));
                    } else {
                        put(0xE0 | (cp >> 12));
                        put(0x80 | ((cp >> 6) & 0x3F));
                        put(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: put((uint8_t)e); break;
            }
        }
        if (*p == '"') p++;
        else ok = false;
        if (cap) dst[n] = 0;
    }

    double num() {
        ws();
        char* end;
        double v = strtod(p, &end);
        if (end == p) ok = false;
        p = end;
        return v;
    }

    void skip() {
        char c = peek();
        if (c == '"') {
            str(nullptr, 0);
        } else if (c == '{' || c == '[') {
            int depth = 0;
            do {
                c = *p;
                if (c == '"') {
                    str(nullptr, 0);
                    continue;
                }
                if (!c) {
                    ok = false;
                    return;
                }
                if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') depth--;
                p++;
            } while (depth > 0);
        } else if (isalpha((unsigned char)c)) {
            while (isalpha((unsigned char)*p)) p++;
        } else {
            num();
        }
    }

    // Object members: call after '{' and after each value. False at '}' or on error.
    bool nextKey(char* key, size_t cap) {
        if (!ok) return false;
        if (peek() == ',') p++;
        if (peek() == '}') {
            p++;
            return false;
        }
        str(key, cap);
        if (!eat(':')) ok = false;
        return ok;
    }

    // Array elements: call after '[' and after each element. False at ']' or on error.
    bool nextItem() {
        if (!ok) return false;
        if (peek() == ',') p++;
        if (peek() == ']') {
            p++;
            return false;
        }
        if (!*p) ok = false;
        return ok;
    }
};

// ---- OSM elements -----------------------------------------------------------

struct Tags {
    char natural[24], waterway[24], manMade[24], bridge[12], lock[12], leisure[24], sport[40], name[64], width[16],
        area[8], type[24];
    bool water;
    void clear() { memset(this, 0, sizeof(*this)); }
    // Buffer for a tag we keep, or nullptr.
    char* field(const char* k, size_t& cap) {
        struct F {
            const char* key;
            char* buf;
            size_t cap;
        } fs[] = {
            {"natural", natural, sizeof(natural)}, {"waterway", waterway, sizeof(waterway)},
            {"man_made", manMade, sizeof(manMade)}, {"bridge", bridge, sizeof(bridge)},
            {"lock", lock, sizeof(lock)},          {"leisure", leisure, sizeof(leisure)},
            {"sport", sport, sizeof(sport)},       {"name", name, sizeof(name)},
            {"width", width, sizeof(width)},       {"area", area, sizeof(area)},
            {"type", type, sizeof(type)},
        };
        for (const F& f : fs) {
            if (!strcmp(k, f.key)) {
                cap = f.cap;
                return f.buf;
            }
        }
        return nullptr;
    }
};

struct Element {
    char type[12];
    double lat, lon;
    Line geom;
    std::vector<Line> members;  // way members' geometry (relations)
    size_t memberCount;         // members in use; Lines beyond it keep their capacity
    Tags t;
    void clear() {
        type[0] = 0;
        lat = lon = NAN;
        geom.clear();
        memberCount = 0;
        t.clear();
    }
};

enum GType { GPoint, GLine, GArea };

bool is(const char* a, const char* b) { return strcmp(a, b) == 0; }

// Mirrors osm2geojson's rules for which closed ways are polygons, for the tags we query.
bool wayIsArea(const Tags& t) {
    if (is(t.area, "no")) return false;
    if (is(t.area, "yes")) return true;
    if (t.natural[0] && !is(t.natural, "coastline") && !is(t.natural, "cliff") && !is(t.natural, "tree_row"))
        return true;
    if (is(t.waterway, "riverbank") || is(t.waterway, "dock") || is(t.waterway, "boatyard") || is(t.waterway, "dam"))
        return true;
    if (t.manMade[0] && !is(t.manMade, "embankment") && !is(t.manMade, "pipeline") && !is(t.manMade, "cutline"))
        return true;
    if (t.leisure[0]) return true;
    return t.water;
}

struct WaterwayWidth {
    const char* name;
    float m;
};
const WaterwayWidth WATERWAY_WIDTH[] = {{"river", 24}, {"canal", 14}, {"stream", 4},
                                        {"ditch", 2},  {"drain", 2},  {"tidal_channel", 12}};

bool isWaterArea(const Tags& t) { return is(t.natural, "water") || is(t.waterway, "riverbank") || t.water; }

// Same rules as classify() in tools/make_map.py.
bool classify(const Tags& t, GType g, uint8_t& kind, uint8_t& flags, float& width) {
    bool isArea = g == GArea, isLine = g == GLine, isPoint = g == GPoint;
    width = 0;
    if (is(t.sport, "rowing") || is(t.sport, "canoe") || is(t.leisure, "marina") || is(t.leisure, "slipway")) {
        kind = mapdata::Club;
        flags = mapdata::F_POINT;
        return true;
    }
    if (is(t.waterway, "weir") || is(t.waterway, "dam")) {
        kind = mapdata::Weir;
        flags = isLine ? mapdata::F_LINE : 0;
        return true;
    }
    if (is(t.waterway, "lock_gate") || is(t.lock, "yes")) {
        kind = mapdata::Lock;
        flags = isPoint ? mapdata::F_POINT : isLine ? mapdata::F_LINE : 0;
        return true;
    }
    if (is(t.manMade, "bridge") || is(t.bridge, "yes")) {
        kind = mapdata::Bridge;
        flags = isLine ? mapdata::F_LINE : 0;
        width = 6;
        return true;
    }
    if (is(t.manMade, "pier")) {
        kind = mapdata::Pier;
        flags = isLine ? mapdata::F_LINE : 0;
        width = 3;
        return true;
    }
    if (isArea && isWaterArea(t)) {
        kind = mapdata::WaterArea;
        flags = 0;
        return true;
    }
    if (isLine) {
        for (const WaterwayWidth& w : WATERWAY_WIDTH) {
            if (!is(t.waterway, w.name)) continue;
            char* end;
            double v = strtod(t.width, &end);
            width = (end != t.width && (*end == 0 || *end == ' ')) ? (float)v : w.m;
            kind = mapdata::WaterLine;
            flags = mapdata::F_LINE;
            return true;
        }
    }
    return false;
}

// ---- geometry ---------------------------------------------------------------

// Joins multipolygon member ways end to end into rings.
void assembleRings(std::vector<Line>& ms, size_t count, std::vector<Line>& rings) {
    std::vector<uint8_t> used(count, 0);
    for (size_t i = 0; i < count; i++) {
        if (used[i] || ms[i].size() < 2) continue;
        used[i] = 1;
        Line cur = ms[i];
        while (!(cur.size() >= 4 && same(cur.front(), cur.back()))) {
            bool found = false;
            for (size_t j = 0; j < count && !found; j++) {
                if (used[j] || ms[j].size() < 2) continue;
                const Line& m = ms[j];
                if (same(m.front(), cur.back())) cur.insert(cur.end(), m.begin() + 1, m.end());
                else if (same(m.back(), cur.back())) cur.insert(cur.end(), m.rbegin() + 1, m.rend());
                else if (same(m.back(), cur.front())) cur.insert(cur.begin(), m.begin(), m.end() - 1);
                else if (same(m.front(), cur.front())) cur.insert(cur.begin(), m.rbegin(), m.rend() - 1);
                else continue;
                used[j] = 1;
                found = true;
            }
            if (!found) break;  // incomplete relation: the fill closes the ring
        }
        if (cur.size() >= 3) rings.push_back(std::move(cur));
    }
}

// Sutherland-Hodgman clip of a ring (no closing duplicate) to the rectangle.
void clipPoly(Line& ring, const Rect& r) {
    Line out;
    for (int edge = 0; edge < 4 && !ring.empty(); edge++) {
        auto inside = [&](const P& p) {
            switch (edge) {
                case 0: return p.x >= r.minx;
                case 1: return p.x <= r.maxx;
                case 2: return p.y >= r.miny;
                default: return p.y <= r.maxy;
            }
        };
        auto cross = [&](const P& a, const P& b) {
            double t;
            if (edge < 2) {
                double X = edge == 0 ? r.minx : r.maxx;
                t = (X - a.x) / (b.x - a.x);
                return P{X, a.y + t * (b.y - a.y)};
            }
            double Y = edge == 2 ? r.miny : r.maxy;
            t = (Y - a.y) / (b.y - a.y);
            return P{a.x + t * (b.x - a.x), Y};
        };
        out.clear();
        size_t n = ring.size();
        for (size_t i = 0; i < n; i++) {
            const P& cur = ring[i];
            const P& prev = ring[(i + n - 1) % n];
            bool ci = inside(cur), pi = inside(prev);
            if (ci) {
                if (!pi) out.push_back(cross(prev, cur));
                out.push_back(cur);
            } else if (pi) {
                out.push_back(cross(prev, cur));
            }
        }
        ring.swap(out);
    }
}

// Liang-Barsky: clips segment a-b to the rectangle; t0/t1 are the kept range.
bool clipSegment(const P& a, const P& b, const Rect& r, double& t0, double& t1) {
    double dx = b.x - a.x, dy = b.y - a.y;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {a.x - r.minx, r.maxx - a.x, a.y - r.miny, r.maxy - a.y};
    t0 = 0;
    t1 = 1;
    for (int k = 0; k < 4; k++) {
        if (p[k] == 0) {
            if (q[k] < 0) return false;
            continue;
        }
        double t = q[k] / p[k];
        if (p[k] < 0) {
            if (t > t1) return false;
            if (t > t0) t0 = t;
        } else {
            if (t < t0) return false;
            if (t < t1) t1 = t;
        }
    }
    return true;
}

void clipLine(const Line& in, const Rect& r, std::vector<Line>& out) {
    Line cur;
    for (size_t i = 1; i < in.size(); i++) {
        const P &a = in[i - 1], &b = in[i];
        double t0, t1;
        if (!clipSegment(a, b, r, t0, t1)) {
            if (cur.size() >= 2) out.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        P ca{a.x + t0 * (b.x - a.x), a.y + t0 * (b.y - a.y)};
        P cb{a.x + t1 * (b.x - a.x), a.y + t1 * (b.y - a.y)};
        if (!cur.empty() && t0 > 0) {
            out.push_back(std::move(cur));
            cur.clear();
        }
        if (cur.empty()) cur.push_back(ca);
        cur.push_back(cb);
        if (t1 < 1) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (cur.size() >= 2) out.push_back(std::move(cur));
}

double segDist2(const P& p, const P& a, const P& b) {
    double dx = b.x - a.x, dy = b.y - a.y;
    double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    if (t < 0) t = 0;
    else if (t > 1) t = 1;
    double ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    return ex * ex + ey * ey;
}

// Douglas-Peucker, iterative.
void simplify(Line& pts, double tol) {
    size_t n = pts.size();
    if (n < 3) return;
    std::vector<uint8_t> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, n - 1}};
    double tol2 = tol * tol;
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        double best = -1;
        size_t idx = 0;
        for (size_t i = a + 1; i < b; i++) {
            double d = segDist2(pts[i], pts[a], pts[b]);
            if (d > best) {
                best = d;
                idx = i;
            }
        }
        if (best > tol2) {
            keep[idx] = 1;
            stack.push_back({a, idx});
            stack.push_back({idx, b});
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (keep[i]) pts[w++] = pts[i];
    pts.resize(w);
}

void simplifyRing(Line& ring, double tol) {
    if (ring.size() < 3) return;
    ring.push_back(ring.front());
    simplify(ring, tol);
    ring.pop_back();
}

// Centre of the largest ring for areas (holes ignored), length-weighted for lines.
P centroid(const std::vector<Line>& rings, GType g) {
    if (g == GPoint) return rings[0][0];
    if (g == GArea) {
        double bestA = 0;
        P best = rings[0][0];
        for (const Line& r : rings) {
            double a = 0, cx = 0, cy = 0;
            for (size_t i = 0, n = r.size(); i < n; i++) {
                const P &p = r[i], &q = r[(i + 1) % n];
                double c = p.x * q.y - q.x * p.y;
                a += c;
                cx += (p.x + q.x) * c;
                cy += (p.y + q.y) * c;
            }
            if (fabs(a) > bestA) {
                bestA = fabs(a);
                best = {cx / (3 * a), cy / (3 * a)};
            }
        }
        if (bestA > 0) return best;
    }
    double len = 0, cx = 0, cy = 0;
    for (const Line& r : rings)
        for (size_t i = 1; i < r.size(); i++) {
            double l = hypot(r[i].x - r[i - 1].x, r[i].y - r[i - 1].y);
            len += l;
            cx += (r[i].x + r[i - 1].x) / 2 * l;
            cy += (r[i].y + r[i - 1].y) / 2 * l;
        }
    return len > 0 ? P{cx / len, cy / len} : rings[0][0];
}

// ---- output -----------------------------------------------------------------

struct OutFeature {
    uint8_t kind, flags;
    uint16_t widthDm;
    int32_t bbox[4];
    char name[24];
    uint32_t ringFirst;
    uint16_t ringCount;
};

struct Builder {
    double oLat, oLon, mLat, mLon;
    Rect clip;
    std::vector<OutFeature> features;
    std::vector<uint32_t> ringLen;
    std::vector<int32_t> points;  // x,y decimetres
    int kindCount[8] = {0};
    int skipped = 0;

    P project(double lat, double lon) const { return {(lon - oLon) * mLon, (lat - oLat) * mLat}; }

    // Appends one feature from projected rings (polygon rings without closing duplicate).
    void emit(uint8_t kind, uint8_t flags, float widthM, const char* name, std::vector<Line>& rings) {
        OutFeature f{};
        f.kind = kind;
        f.flags = flags;
        long w = lroundf(widthM * 10);
        f.widthDm = (uint16_t)(w > 0xFFFF ? 0xFFFF : w);
        f.bbox[0] = f.bbox[1] = INT32_MAX;
        f.bbox[2] = f.bbox[3] = INT32_MIN;
        // Name truncated to 23 bytes on a UTF-8 character boundary.
        size_t n = strnlen(name, 23);
        while (n > 0 && n < strlen(name) && ((uint8_t)name[n] & 0xC0) == 0x80) n--;
        memcpy(f.name, name, n);
        f.ringFirst = ringLen.size();
        size_t minPts = (flags & mapdata::F_POINT) ? 1 : (flags & mapdata::F_LINE) ? 2 : 3;

        for (Line& ring : rings) {
            if (ring.size() > MAX_RING_POINTS) {
                double tol = SIMPLIFY_M;
                while (ring.size() > MAX_RING_POINTS) {
                    tol *= 2;
                    if (flags & mapdata::F_LINE) simplify(ring, tol);
                    else simplifyRing(ring, tol);
                }
            }
            size_t start = points.size();
            int32_t lx = 0, ly = 0;
            size_t count = 0;
            for (const P& p : ring) {
                int32_t x = (int32_t)lround(p.x * 10), y = (int32_t)lround(p.y * 10);
                if (count && x == lx && y == ly) continue;
                points.push_back(x);
                points.push_back(y);
                lx = x;
                ly = y;
                count++;
            }
            if (count > 1 && points[start] == lx && points[start + 1] == ly) {
                points.resize(points.size() - 2);
                count--;
            }
            if (count < minPts) {
                points.resize(start);
                continue;
            }
            for (size_t i = start; i < points.size(); i += 2) {
                if (points[i] < f.bbox[0]) f.bbox[0] = points[i];
                if (points[i + 1] < f.bbox[1]) f.bbox[1] = points[i + 1];
                if (points[i] > f.bbox[2]) f.bbox[2] = points[i];
                if (points[i + 1] > f.bbox[3]) f.bbox[3] = points[i + 1];
            }
            ringLen.push_back(count);
        }
        f.ringCount = ringLen.size() - f.ringFirst;
        if (!f.ringCount) {
            skipped++;
            return;
        }
        features.push_back(f);
        kindCount[kind & 7]++;
    }

    void process(Element& e, std::vector<Line>& rings, std::vector<Line>& tmp) {
        rings.clear();
        GType g;
        if (is(e.type, "node")) {
            if (isnan(e.lat) || isnan(e.lon)) return;
            g = GPoint;
            rings.push_back(Line{project(e.lat, e.lon)});
        } else if (is(e.type, "way")) {
            if (e.geom.size() < 2) return;
            bool closed = e.geom.size() >= 4 && same(e.geom.front(), e.geom.back());
            g = closed && wayIsArea(e.t) ? GArea : GLine;
            rings.push_back(e.geom);
            if (g == GArea) rings.back().pop_back();
        } else if (is(e.type, "relation")) {
            if (!is(e.t.type, "multipolygon")) return;
            g = GArea;
            assembleRings(e.members, e.memberCount, rings);
            for (Line& r : rings)
                if (same(r.front(), r.back())) r.pop_back();
        } else {
            return;
        }
        if (rings.empty()) return;

        uint8_t kind, flags;
        float width;
        if (!classify(e.t, g, kind, flags, width)) return;

        // A rowing course or marina mapped as a water area is both water and a club.
        if (kind == mapdata::Club && g == GArea && isWaterArea(e.t)) {
            tmp = rings;
            emitShape(mapdata::WaterArea, 0, 0, e.t.name, g, tmp, scratch);
        }
        emitShape(kind, flags, width, e.t.name, g, rings, tmp);
    }

    // Reduces projected geometry to what the feature needs (centroid, or clipped and
    // simplified rings/lines), then emits it. `scratch` is a work buffer.
    void emitShape(uint8_t kind, uint8_t flags, float width, const char* name, GType g, std::vector<Line>& rings,
                   std::vector<Line>& scratch) {
        if (flags & mapdata::F_POINT) {
            P c = centroid(rings, g);
            rings.clear();
            rings.push_back(Line{c});
        } else if (g == GArea) {
            for (Line& r : rings) {
                clipPoly(r, clip);
                simplifyRing(r, SIMPLIFY_M);
            }
        } else {
            scratch.clear();
            for (const Line& r : rings) clipLine(r, clip, scratch);
            rings.swap(scratch);
            for (Line& r : rings) simplify(r, SIMPLIFY_M);
        }
        emit(kind, flags, width, name, rings);
    }

    std::vector<Line> scratch;
};

void readGeometry(Json& j, Builder& b, Line& out) {
    if (j.peek() != '[') {
        j.skip();
        return;
    }
    j.p++;
    char k[8];
    while (j.nextItem()) {
        if (j.peek() != '{') {
            j.skip();  // null for nodes outside the query
            continue;
        }
        j.p++;
        double lat = NAN, lon = NAN;
        while (j.nextKey(k, sizeof(k))) {
            if (is(k, "lat")) lat = j.num();
            else if (is(k, "lon")) lon = j.num();
            else j.skip();
        }
        if (!isnan(lat) && !isnan(lon)) out.push_back(b.project(lat, lon));
    }
}

void readMembers(Json& j, Builder& b, Element& e) {
    if (j.peek() != '[') {
        j.skip();
        return;
    }
    j.p++;
    char k[12], type[8];
    while (j.nextItem()) {
        if (j.peek() != '{') {
            j.skip();
            continue;
        }
        j.p++;
        if (e.members.size() <= e.memberCount) e.members.emplace_back();
        Line& m = e.members[e.memberCount];
        m.clear();
        type[0] = 0;
        while (j.nextKey(k, sizeof(k))) {
            if (is(k, "type")) j.str(type, sizeof(type));
            else if (is(k, "geometry")) readGeometry(j, b, m);
            else j.skip();
        }
        if (is(type, "way") && m.size() >= 2) e.memberCount++;
    }
}

void readTags(Json& j, Tags& t) {
    if (!j.eat('{')) {
        j.skip();
        return;
    }
    char k[32];
    while (j.nextKey(k, sizeof(k))) {
        size_t cap;
        if (char* buf = t.field(k, cap)) {
            j.str(buf, cap);
        } else if (is(k, "water")) {
            t.water = true;
            j.skip();
        } else {
            j.skip();
        }
    }
}

void readElement(Json& j, Builder& b, Element& e) {
    e.clear();
    if (!j.eat('{')) {
        j.skip();
        return;
    }
    char k[16];
    while (j.nextKey(k, sizeof(k))) {
        if (is(k, "type")) j.str(e.type, sizeof(e.type));
        else if (is(k, "lat")) e.lat = j.num();
        else if (is(k, "lon")) e.lon = j.num();
        else if (is(k, "geometry")) readGeometry(j, b, e.geom);
        else if (is(k, "members")) readMembers(j, b, e);
        else if (is(k, "tags")) readTags(j, e.t);
        else j.skip();
    }
}

void put(std::vector<uint8_t>& o, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    o.insert(o.end(), b, b + n);
}
template <typename T>
void put(std::vector<uint8_t>& o, T v) {
    put(o, &v, sizeof(v));
}

}  // namespace

String query(double s, double w, double n, double e) {
    char b[96];
    snprintf(b, sizeof(b), "(%.6f,%.6f,%.6f,%.6f)", s, w, n, e);
    String q = "[out:json][timeout:60];(";  // must stay under the HTTP read timeout in map_job
    const char* parts[] = {
        "way[\"natural\"=\"water\"]",
        "relation[\"natural\"=\"water\"]",
        "way[\"waterway\"=\"riverbank\"]",
        "relation[\"waterway\"=\"riverbank\"]",
        "way[\"water\"]",
        "way[\"waterway\"~\"^(river|canal|stream|ditch|drain|tidal_channel)$\"]",
        "way[\"man_made\"=\"bridge\"]",
        "way[\"bridge\"=\"yes\"]",
        "way[\"waterway\"=\"lock_gate\"]",
        "node[\"waterway\"=\"lock_gate\"]",
        "way[\"lock\"=\"yes\"]",
        "way[\"waterway\"~\"^(weir|dam)$\"]",
        "way[\"man_made\"=\"pier\"]",
        "way[\"leisure\"=\"marina\"]",
        "node[\"leisure\"=\"marina\"]",
        "way[\"sport\"~\"rowing|canoe\"]",
        "node[\"sport\"~\"rowing|canoe\"]",
        "way[\"leisure\"=\"slipway\"]",
        "node[\"leisure\"=\"slipway\"]",
    };
    for (const char* p : parts) {
        q += p;
        q += b;
        q += ';';
    }
    q += ");out geom;";
    return q;
}

bool build(const char* text, double south, double west, double north, double east, const char* name,
           const char* outPath, Print& out, Result& result, char* err, size_t errLen) {
    uint32_t tRead = millis();
    Builder b;
    b.oLat = (south + north) / 2;
    b.oLon = (west + east) / 2;
    b.mLat = 111320.0;
    b.mLon = 111320.0 * cos(b.oLat * M_PI / 180.0);
    P lo = b.project(south, west), hi = b.project(north, east);
    b.clip = {lo.x, lo.y, hi.x, hi.y};
    b.features.reserve(4096);
    b.ringLen.reserve(8192);
    b.points.reserve(200000);

    Json j{text};
    Element e;
    std::vector<Line> rings, tmp;
    int elements = 0;
    char remark[96] = "";
    char k[16];
    if (j.eat('{')) {
        while (j.nextKey(k, sizeof(k))) {
            if (is(k, "remark")) {
                j.str(remark, sizeof(remark));
            } else if (is(k, "elements") && j.peek() == '[') {
                j.p++;
                while (j.nextItem()) {
                    readElement(j, b, e);
                    if (!j.ok) break;
                    b.process(e, rings, tmp);
                    if (++elements % 64 == 0) vTaskDelay(1);  // let lower-priority tasks run
                }
            } else {
                j.skip();
            }
        }
    }
    bool parsedOk = j.ok;
    uint32_t tBuild = millis();

    if (remark[0]) out.printf("[build] server remark: %s\n", remark);
    if (!parsedOk) out.printf("[build] warning: JSON parse stopped early after %d elements\n", elements);
    if (b.features.empty()) {
        // A remark means the server gave up (timeout, too busy); otherwise the area has no water.
        snprintf(err, errLen, "%s", remark[0] ? "server too busy, try again" : "no water in this area");
        out.printf("[build] ERR %s\n", err);
        return false;
    }

    int32_t bb[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (const OutFeature& f : b.features) {
        bb[0] = min(bb[0], f.bbox[0]);
        bb[1] = min(bb[1], f.bbox[1]);
        bb[2] = max(bb[2], f.bbox[2]);
        bb[3] = max(bb[3], f.bbox[3]);
    }

    std::vector<uint8_t> o;
    o.reserve(84 + b.features.size() * 32 + b.ringLen.size() * 2 + b.points.size() * 4);
    put(o, "RMAP", 4);
    put<uint16_t>(o, VERSION);
    put<uint16_t>(o, 0);
    put<double>(o, b.oLat);
    put<double>(o, b.oLon);
    put(o, bb, sizeof(bb));
    char hname[32] = {0};
    strlcpy(hname, name, sizeof(hname));
    put(o, hname, sizeof(hname));
    put<uint32_t>(o, b.features.size());
    put<uint32_t>(o, b.ringLen.size());
    put<uint32_t>(o, b.points.size() / 2);
    size_t pi = 0;  // rings are stored in feature order
    for (const OutFeature& f : b.features) {
        put<uint8_t>(o, f.kind);
        put<uint8_t>(o, f.flags);
        put<uint16_t>(o, f.widthDm);
        put(o, f.bbox, sizeof(f.bbox));
        uint8_t nl = strlen(f.name);
        put<uint8_t>(o, nl);
        put(o, f.name, nl);
        put<uint16_t>(o, f.ringCount);
        for (uint32_t r = f.ringFirst; r < f.ringFirst + f.ringCount; r++) {
            uint32_t c = b.ringLen[r];
            put<uint16_t>(o, c);
            put(o, &b.points[pi * 2], c * 8);
            pi += c;
        }
    }

    File f = SD_MMC.open(outPath, FILE_WRITE, true);
    if (!f) {
        snprintf(err, errLen, "cannot write to SD card");
        out.printf("[build] ERR %s %s\n", err, outPath);
        return false;
    }
    size_t written = 0;
    while (written < o.size()) {
        size_t n = f.write(o.data() + written, min((size_t)16384, o.size() - written));
        if (!n) break;
        written += n;
    }
    f.close();
    uint32_t tWrite = millis();
    if (written != o.size()) {
        snprintf(err, errLen, "SD card full?");
        out.printf("[build] ERR write failed (%s)\n", err);
        SD_MMC.remove(outPath);
        return false;
    }

    out.printf("[build] OK %s %u bytes: %u features (water %d, lines %d, bridges %d, locks %d, weirs %d, piers %d, "
               "clubs %d), %u rings, %u points from %d elements; build %lu ms, write %lu ms\n",
               outPath, (unsigned)o.size(), (unsigned)b.features.size(), b.kindCount[mapdata::WaterArea],
               b.kindCount[mapdata::WaterLine], b.kindCount[mapdata::Bridge], b.kindCount[mapdata::Lock],
               b.kindCount[mapdata::Weir], b.kindCount[mapdata::Pier], b.kindCount[mapdata::Club],
               (unsigned)b.ringLen.size(), (unsigned)(b.points.size() / 2), elements,
               (unsigned long)(tBuild - tRead), (unsigned long)(tWrite - tBuild));
    result.features = b.features.size();
    result.bytes = o.size();
    return true;
}

}  // namespace mapbuild
