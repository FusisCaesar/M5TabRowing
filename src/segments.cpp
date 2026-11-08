#include "segments.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "map_data.h"

namespace segments {

namespace {

constexpr float PASS_M = 40;       // close enough to a point to have reached it
constexpr float CORRIDOR_M = 80;   // still "on" the leg when passing wide of a point

constexpr float LINE_HYST_M = 3;       // metres past or before the line that count as a side
constexpr float CONFIRM_PAST_M = 40;    // a crossing still counts this far beyond the line
constexpr float CROSS_TRAVEL_M = 4;     // the boat has to actually move across, not sit on the line
constexpr float HEADING_LIMIT_DEG = 70;

Segment* items = nullptr;
int n = 0;
int cap = 0;
int sel = -1;
int leg = 0;      // leg of the selected segment the boat is on
int nextPt = 0;   // point the boat is heading for
bool pastStart = false;
bool armedBehind = false;
bool crossPending = false;
double armTravel = 0;
bool finishArmed = false;
bool finishPending = false;
double finishArmTravel = 0;

void resetRun() {
    leg = 0;
    nextPt = 0;
    pastStart = false;
    armedBehind = false;
    crossPending = false;
    armTravel = 0;
    finishArmed = false;
    finishPending = false;
    finishArmTravel = 0;
}

bool grow(int need) {
    if (need <= cap) return true;
    int ncap = cap < 4 ? 4 : cap;
    while (ncap < need) ncap *= 2;
    Segment* p = (Segment*)heap_caps_realloc(items, ncap * sizeof(Segment), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (Segment*)heap_caps_realloc(items, ncap * sizeof(Segment), MALLOC_CAP_8BIT);
    if (!p) return false;
    for (int i = cap; i < ncap; i++) {
        p[i].name[0] = 0;
        p[i].count = 0;
        p[i].point = nullptr;
    }
    items = p;
    cap = ncap;
    return true;
}

bool setPoints(Segment& s, const int* points, int npoints) {
    int16_t* p = nullptr;
    if (npoints > 0) {
        p = (int16_t*)heap_caps_malloc(npoints * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = (int16_t*)heap_caps_malloc(npoints * sizeof(int16_t), MALLOC_CAP_8BIT);
        if (!p) return false;
        for (int i = 0; i < npoints; i++) p[i] = (int16_t)points[i];
    }
    heap_caps_free(s.point);
    s.point = p;
    s.count = npoints;
    return true;
}

// False when flash cannot hold the courses.
bool persist() {
    size_t bytes = 0;
    for (int i = 0; i < n; i++) bytes += NAME_LEN + sizeof(uint16_t) + items[i].count * sizeof(int16_t);
    uint8_t* buf = bytes ? (uint8_t*)malloc(bytes) : nullptr;
    if (bytes && !buf) return false;
    uint8_t* w = buf;
    for (int i = 0; i < n; i++) {
        memcpy(w, items[i].name, NAME_LEN);
        w += NAME_LEN;
        uint16_t c = (uint16_t)items[i].count;
        memcpy(w, &c, sizeof(c));
        w += sizeof(c);
        if (c) {
            memcpy(w, items[i].point, c * sizeof(int16_t));
            w += c * sizeof(int16_t);
        }
    }
    Preferences prefs;
    prefs.begin("segments", false);
    prefs.putUShort("n", (uint16_t)n);
    prefs.putUShort("sel", (uint16_t)(sel + 1));
    prefs.putUChar("ver", 2);
    bool ok = true;
    if (bytes) ok = prefs.putBytes("v2", buf, bytes) == bytes;
    else prefs.remove("v2");
    prefs.end();
    free(buf);
    return ok;
}

void eraseAt(int index) {
    heap_caps_free(items[index].point);
    for (int j = index; j < n - 1; j++) items[j] = items[j + 1];
    n--;
    items[n].point = nullptr;
    items[n].count = 0;
    items[n].name[0] = 0;
    if (sel == index) sel = -1;
    else if (sel > index) sel--;
}

bool numberUsed(int num) {
    char tag[16];
    snprintf(tag, sizeof(tag), "S%d", num);
    size_t tlen = strlen(tag);
    for (int i = 0; i < n; i++) {
        const char* s = items[i].name;
        size_t len = strlen(s);
        if (strcmp(s, tag) == 0) return true;
        if (len >= tlen + 3 && strcmp(s + len - tlen, tag) == 0 && memcmp(s + len - tlen - 3, " - ", 3) == 0)
            return true;
    }
    return false;
}

int nextNumber() {
    for (int num = 1; num < 100000; num++)
        if (!numberUsed(num)) return num;
    return 0;
}

const char* mapNameAt(double lat, double lon) {
    const char* fallback = nullptr;
    for (int i = 0; i < mapdata::fileCount(); i++) {
        mapdata::FileInfo fi;
        if (!mapdata::fileInfo(i, fi) || !fi.name || !fi.name[0]) continue;
        if (lat < fi.south || lat > fi.north || lon < fi.west || lon > fi.east) continue;
        if (fi.loaded) return fi.name;
        if (!fallback) fallback = fi.name;
    }
    return fallback;
}

bool valid(const int* points, int npoints) {
    if (!points || npoints < 2) return false;
    for (int i = 0; i < npoints; i++) {
        if (!targets::get(points[i])) return false;
        for (int j = 0; j < i; j++)
            if (points[j] == points[i]) return false;
    }
    return true;
}

void metresDelta(double lat, double lon, double lat2, double lon2, double& east, double& north) {
    north = (lat2 - lat) * 111320.0;
    east = (lon2 - lon) * 111320.0 * cos(lat * M_PI / 180.0);
}

// 0 at the first point, 1 at the second, unclamped. 0 when the leg is tiny.
double alongFraction(double lat, double lon, double alat, double alon, double blat, double blon) {
    double abE, abN, apE, apN;
    metresDelta(alat, alon, blat, blon, abE, abN);
    metresDelta(alat, alon, lat, lon, apE, apN);
    double den = abE * abE + abN * abN;
    if (den < 1) return 0;
    return (apE * abE + apN * abN) / den;
}

double crossMetres(double lat, double lon, double alat, double alon, double blat, double blon) {
    double abE, abN, apE, apN;
    metresDelta(alat, alon, blat, blon, abE, abN);
    metresDelta(alat, alon, lat, lon, apE, apN);
    double den = sqrt(abE * abE + abN * abN);
    if (den < 1) return 0;
    return fabs(abE * apN - abN * apE) / den;
}

const targets::Target* pointAt(const Segment& s, int i) {
    if (i < 0 || i >= s.count) return nullptr;
    return targets::get(s.point[i]);
}

// True once the boat has arrived at B: near it, or past it while still on the leg.
bool reached(double lat, double lon, const targets::Target* a, const targets::Target* b) {
    if (targets::metresBetween(lat, lon, b->lat, b->lon) <= PASS_M) return true;
    double t = alongFraction(lat, lon, a->lat, a->lon, b->lat, b->lon);
    if (t < 1.0) return false;
    return crossMetres(lat, lon, a->lat, a->lon, b->lat, b->lon) <= CORRIDOR_M;
}

struct Axis {
    double along = 0;    // metres, negative before the start pin
    double cross = 0;    // metres, left of the first leg positive
    double bearing = 0;  // degrees, direction of the first leg
    bool ok = false;
};

// along is metres past the finish pin, along the last leg. Negative is still on the course.
bool finishAxisOf(double lat, double lon, Axis& ax) {
    if (sel < 0 || sel >= n || items[sel].count < 2) return false;
    const targets::Target* a = pointAt(items[sel], items[sel].count - 2);
    const targets::Target* b = pointAt(items[sel], items[sel].count - 1);
    if (!a || !b) return false;
    double abE, abN, apE, apN;
    metresDelta(a->lat, a->lon, b->lat, b->lon, abE, abN);
    double len = sqrt(abE * abE + abN * abN);
    if (len < 1) return false;
    metresDelta(b->lat, b->lon, lat, lon, apE, apN);
    ax.along = (apE * abE + apN * abN) / len;
    ax.cross = (abE * apN - abN * apE) / len;
    ax.bearing = atan2(abE, abN) * 180.0 / M_PI;
    if (ax.bearing < 0) ax.bearing += 360.0;
    ax.ok = true;
    return true;
}

bool axisOfIndex(int index, double lat, double lon, Axis& ax) {
    if (index < 0 || index >= n || items[index].count < 2) return false;
    const targets::Target* a = pointAt(items[index], 0);
    const targets::Target* b = pointAt(items[index], 1);
    if (!a || !b) return false;
    double abE, abN, apE, apN;
    metresDelta(a->lat, a->lon, b->lat, b->lon, abE, abN);
    double len = sqrt(abE * abE + abN * abN);
    if (len < 1) return false;
    metresDelta(a->lat, a->lon, lat, lon, apE, apN);
    ax.along = (apE * abE + apN * abN) / len;
    ax.cross = (abE * apN - abN * apE) / len;
    ax.bearing = atan2(abE, abN) * 180.0 / M_PI;
    if (ax.bearing < 0) ax.bearing += 360.0;
    ax.ok = true;
    return true;
}

bool axisOf(double lat, double lon, Axis& ax) { return axisOfIndex(sel, lat, lon, ax); }

void shiftFrom(double lat, double lon, double east, double north, double& outLat, double& outLon) {
    outLat = lat + north / 111320.0;
    double c = cos(lat * M_PI / 180.0);
    if (fabs(c) < 0.01) c = c < 0 ? -0.01 : 0.01;
    outLon = lon + east / (111320.0 * c);
}

// Keep the distance tiles and the course cursor on the right side of the start line.
void note(double lat, double lon) {
    if (lat == 0 && lon == 0) return;
    Axis ax;
    if (!axisOf(lat, lon, ax)) return;
    bool inGate = fabs(ax.cross) <= START_LINE_HALF_M;
    if (ax.along <= -LINE_HYST_M) {
        pastStart = false;
        // Back through the line itself restarts the piece. Beside it, only the distance changes.
        if (inGate) {
            leg = 0;
            nextPt = 0;
        }
    } else if (ax.along >= LINE_HYST_M || leg > 0) {
        pastStart = true;
    }
}

float legsFrom(const Segment& s, int first) {
    float d = 0;
    for (int i = first; i < s.count - 1; i++) {
        const targets::Target* a = pointAt(s, i);
        const targets::Target* b = pointAt(s, i + 1);
        if (!a || !b) return -1;
        d += targets::metresBetween(a->lat, a->lon, b->lat, b->lon);
    }
    return d;
}

}  // namespace

void begin() {
    Preferences prefs;
    prefs.begin("segments", false);
    bool upgrade = prefs.getUChar("ver", 1) < 2;
    if (!upgrade) {
        n = prefs.getUShort("n", 0);
        sel = (int)prefs.getUShort("sel", 0) - 1;
        size_t len = prefs.getBytesLength("v2");
        uint8_t* raw = len ? (uint8_t*)malloc(len) : nullptr;
        if (raw) prefs.getBytes("v2", raw, len);
        prefs.end();
        const uint8_t* r = raw;
        const uint8_t* end = raw ? raw + len : nullptr;
        int got = 0;
        bool ok = raw || n == 0;
        for (int i = 0; ok && i < n; i++) {
            if (r + NAME_LEN + sizeof(uint16_t) > end) {
                ok = false;
                break;
            }
            if (!grow(got + 1)) {
                ok = false;
                break;
            }
            memcpy(items[got].name, r, NAME_LEN);
            items[got].name[NAME_LEN - 1] = 0;
            r += NAME_LEN;
            uint16_t c = 0;
            memcpy(&c, r, sizeof(c));
            r += sizeof(c);
            if (r + c * sizeof(int16_t) > end) {
                ok = false;
                break;
            }
            int* pts = c ? (int*)malloc(c * sizeof(int)) : nullptr;
            if (c && !pts) {
                ok = false;
                break;
            }
            for (int k = 0; k < c; k++) {
                int16_t id = 0;
                memcpy(&id, r, sizeof(id));
                r += sizeof(id);
                pts[k] = id;
            }
            if (!setPoints(items[got], pts, c)) ok = false;
            free(pts);
            if (ok) got++;
        }
        free(raw);
        n = ok ? got : 0;
        if (!ok) {
            for (int i = 0; i < got; i++) {
                heap_caps_free(items[i].point);
                items[i].point = nullptr;
            }
            n = 0;
            sel = -1;
            resetRun();
            return;
        }
    } else {
        int nOld = prefs.getUChar("n", 0);
        sel = (int)prefs.getUChar("sel", 0) - 1;
        size_t len = prefs.getBytesLength("v1");
        uint8_t* raw = len ? (uint8_t*)malloc(len) : nullptr;
        if (raw) prefs.getBytes("v1", raw, len);
        prefs.end();
        // The previous build stored eight fixed records: 40-byte name, count, 12 indexes.
        int stride = 53;
        if (len >= 8 && len % 8 == 0) stride = (int)(len / 8);
        if (nOld > 8) nOld = 8;
        for (int i = 0; raw && i < nOld; i++) {
            const uint8_t* rec = raw + i * stride;
            if (rec + 41 > raw + len) break;
            if (!grow(n + 1)) break;
            memcpy(items[n].name, rec, NAME_LEN);
            items[n].name[NAME_LEN - 1] = 0;
            int c = rec[40];
            if (c < 0) c = 0;
            if (c > 12) c = 12;
            int pts[12];
            for (int k = 0; k < c; k++) pts[k] = (int8_t)rec[41 + k];
            if (setPoints(items[n], pts, c)) n++;
        }
        free(raw);
        persist();
    }

    bool dirty = false;
    for (int i = n - 1; i >= 0; i--) {
        int w = 0;
        for (int j = 0; j < items[i].count; j++) {
            if (!targets::get(items[i].point[j])) {
                dirty = true;
                continue;
            }
            items[i].point[w++] = items[i].point[j];
        }
        items[i].count = w;
        if (w < 2) {
            eraseAt(i);
            dirty = true;
        }
    }
    if (sel < -1 || sel >= n) {
        sel = -1;
        dirty = true;
    }
    resetRun();
    if (dirty) persist();
}

int count() { return n; }

const Segment* get(int index) {
    if (index < 0 || index >= n) return nullptr;
    return &items[index];
}

int selected() { return sel; }

const char* selectedName() {
    if (sel < 0 || sel >= n) return "";
    return items[sel].name;
}

void select(int index) {
    if (index < -1 || index >= n) index = -1;
    if (index == sel) return;
    sel = index;
    resetRun();
    persist();
}

bool add(const int* points, int npoints) {
    if (!valid(points, npoints) || !grow(n + 1)) return false;
    int num = nextNumber();
    if (!num) return false;
    const targets::Target* start = targets::get(points[0]);
    const char* map = start ? mapNameAt(start->lat, start->lon) : nullptr;
    if (map && map[0]) snprintf(items[n].name, sizeof(items[n].name), "%s - S%d", map, num);
    else snprintf(items[n].name, sizeof(items[n].name), "S%d", num);
    if (!setPoints(items[n], points, npoints)) return false;
    int prev = sel;
    sel = n;
    n++;
    resetRun();
    if (!persist()) {
        heap_caps_free(items[n - 1].point);
        items[n - 1].point = nullptr;
        items[n - 1].count = 0;
        n--;
        sel = prev;
        return false;
    }
    return true;
}

bool duplicate(int index, bool reverse) {
    if (index < 0 || index >= n || items[index].count < 2) return false;
    int npoints = items[index].count;
    int* pts = (int*)malloc((size_t)npoints * sizeof(int));
    if (!pts) return false;
    for (int i = 0; i < npoints; i++)
        pts[i] = items[index].point[reverse ? npoints - 1 - i : i];
    bool ok = add(pts, npoints);
    free(pts);
    return ok;
}

bool update(int index, const int* points, int npoints) {
    if (index < 0 || index >= n || !valid(points, npoints)) return false;
    int16_t* old = items[index].point;
    int oldCount = items[index].count;
    items[index].point = nullptr;
    if (!setPoints(items[index], points, npoints)) {
        items[index].point = old;
        items[index].count = oldCount;
        return false;
    }
    heap_caps_free(old);
    if (index == sel) resetRun();
    if (!persist()) return false;
    return true;
}

bool rename(int index, const char* name) {
    if (index < 0 || index >= n || !name || !name[0]) return false;
    if (strcmp(items[index].name, name) == 0) return true;
    strlcpy(items[index].name, name, sizeof(items[index].name));
    persist();
    return true;
}

void remove(int index) {
    if (index < 0 || index >= n) return;
    eraseAt(index);
    resetRun();
    persist();
}

void onTargetRemoved(int targetIndex) {
    bool dirty = false;
    for (int i = n - 1; i >= 0; i--) {
        int w = 0;
        for (int j = 0; j < items[i].count; j++) {
            int p = items[i].point[j];
            if (p == targetIndex) {
                dirty = true;
                continue;
            }
            if (p > targetIndex) {
                p--;
                dirty = true;
            }
            items[i].point[w++] = (int16_t)p;
        }
        items[i].count = w;
        if (w < 2) {
            eraseAt(i);
            dirty = true;
        }
    }
    if (sel >= n) sel = -1;
    resetRun();
    if (dirty) persist();
}

float lengthAlong(const int* points, int npoints) {
    if (!points || npoints < 2) return -1;
    float d = 0;
    for (int i = 1; i < npoints; i++) {
        const targets::Target* a = targets::get(points[i - 1]);
        const targets::Target* b = targets::get(points[i]);
        if (!a || !b) return -1;
        d += targets::metresBetween(a->lat, a->lon, b->lat, b->lon);
    }
    return d;
}

float length(int index) {
    if (index < 0 || index >= n || items[index].count < 2) return -1;
    float d = 0;
    for (int i = 1; i < items[index].count; i++) {
        const targets::Target* a = pointAt(items[index], i - 1);
        const targets::Target* b = pointAt(items[index], i);
        if (!a || !b) return -1;
        d += targets::metresBetween(a->lat, a->lon, b->lat, b->lon);
    }
    return d;
}

void track(double lat, double lon) {
    if (sel < 0 || sel >= n || (lat == 0 && lon == 0)) return;
    const Segment& s = items[sel];
    if (s.count < 2) return;
    while (leg < s.count - 1) {
        const targets::Target* a = pointAt(s, leg);
        const targets::Target* b = pointAt(s, leg + 1);
        if (!a || !b) return;
        if (!reached(lat, lon, a, b)) break;
        leg++;
    }
    if (leg >= s.count - 1) {
        nextPt = s.count - 1;
        return;
    }
    const targets::Target* a = pointAt(s, leg);
    const targets::Target* b = pointAt(s, leg + 1);
    if (a && b && leg == 0 &&
        alongFraction(lat, lon, a->lat, a->lon, b->lat, b->lon) < 0)
        nextPt = 0;
    else
        nextPt = leg + 1;
}

float remaining(double lat, double lon) {
    if (sel < 0 || sel >= n || (lat == 0 && lon == 0)) return -1;
    const Segment& s = items[sel];
    if (s.count < 2) return -1;
    track(lat, lon);
    if (leg >= s.count - 1) return 0;
    const targets::Target* a = pointAt(s, leg);
    const targets::Target* b = pointAt(s, leg + 1);
    if (!a || !b) return -1;
    float after = legsFrom(s, leg + 1);
    if (after < 0) return -1;
    double t = alongFraction(lat, lon, a->lat, a->lon, b->lat, b->lon);
    if (leg == 0 && t < 0 && !pastStart) {
        float course = legsFrom(s, 0);
        if (course < 0) return -1;
        return targets::metresBetween(lat, lon, a->lat, a->lon) + course;
    }
    if (t > 1) return targets::metresBetween(lat, lon, b->lat, b->lon) + after;
    if (t < 0) t = 0;
    float legLen = targets::metresBetween(a->lat, a->lon, b->lat, b->lon);
    float cross = (float)crossMetres(lat, lon, a->lat, a->lon, b->lat, b->lon);
    return cross + (float)((1.0 - t) * legLen) + after;
}

int nextPoint() {
    if (sel < 0 || sel >= n) return -1;
    return nextPt;
}

int selectedRoute(double* lat, double* lon, int cap) {
    if (sel < 0 || sel >= n || !lat || !lon || cap <= 0) return 0;
    const Segment& s = items[sel];
    int got = 0;
    int lim = s.count < cap ? s.count : cap;
    for (int i = 0; i < lim; i++) {
        const targets::Target* t = pointAt(s, i);
        if (!t) return 0;
        lat[got] = t->lat;
        lon[got] = t->lon;
        got++;
    }
    return got;
}

bool lineSquare(double lat, double lon, double dirE, double dirN, double& lat1, double& lon1, double& lat2,
                double& lon2) {
    double len = sqrt(dirE * dirE + dirN * dirN);
    if (len < 1) return false;
    double east = -dirN / len * START_LINE_HALF_M;
    double north = dirE / len * START_LINE_HALF_M;
    shiftFrom(lat, lon, east, north, lat1, lon1);
    shiftFrom(lat, lon, -east, -north, lat2, lon2);
    return true;
}

bool startLineBetween(double aLat, double aLon, double bLat, double bLon, double& lat1, double& lon1,
                       double& lat2, double& lon2) {
    double abE, abN;
    metresDelta(aLat, aLon, bLat, bLon, abE, abN);
    return lineSquare(aLat, aLon, abE, abN, lat1, lon1, lat2, lon2);
}

bool startLine(double& lat1, double& lon1, double& lat2, double& lon2) {
    if (sel < 0 || sel >= n) return false;
    const targets::Target* a = pointAt(items[sel], 0);
    const targets::Target* b = pointAt(items[sel], 1);
    if (!a || !b) return false;
    return startLineBetween(a->lat, a->lon, b->lat, b->lon, lat1, lon1, lat2, lon2);
}

bool finishLineBetween(double prevLat, double prevLon, double finLat, double finLon, double& lat1, double& lon1,
                        double& lat2, double& lon2) {
    double abE, abN;
    metresDelta(prevLat, prevLon, finLat, finLon, abE, abN);
    return lineSquare(finLat, finLon, abE, abN, lat1, lon1, lat2, lon2);
}

bool finishLine(double& lat1, double& lon1, double& lat2, double& lon2) {
    if (sel < 0 || sel >= n || items[sel].count < 2) return false;
    const targets::Target* a = pointAt(items[sel], items[sel].count - 2);
    const targets::Target* b = pointAt(items[sel], items[sel].count - 1);
    if (!a || !b) return false;
    return finishLineBetween(a->lat, a->lon, b->lat, b->lon, lat1, lon1, lat2, lon2);
}

bool usesTarget(int targetIndex) {
    for (int i = 0; i < n; i++)
        for (int k = 0; k < items[i].count; k++)
            if (items[i].point[k] == targetIndex) return true;
    return false;
}

bool onCourse() { return sel >= 0 && pastStart; }

float distanceToStart(double lat, double lon) {
    Axis ax;
    if (lat == 0 && lon == 0) return -1;
    if (!axisOf(lat, lon, ax)) return -1;
    double before = ax.along < 0 ? -ax.along : 0;
    double side = fabs(ax.cross) - START_LINE_HALF_M;
    if (side < 0) side = 0;
    return (float)hypot(before, side);
}

// Metres still before this start line. Negative once the boat is on or past it.
float approachToStart(int index, double lat, double lon) {
    Axis ax;
    if ((lat == 0 && lon == 0) || !axisOfIndex(index, lat, lon, ax) || ax.along >= 0) return -1;
    double side = fabs(ax.cross) - START_LINE_HALF_M;
    if (side < 0) side = 0;
    return (float)hypot(-ax.along, side);
}

int nearestStart(double lat, double lon, float withinM) {
    int best = -1;
    float bestD = withinM;
    for (int i = 0; i < n; i++) {
        float d = approachToStart(i, lat, lon);
        if (d < 0 || d > bestD) continue;
        bestD = d;
        best = i;
    }
    return best;
}

bool pollStart(double lat, double lon, float speedMps, float courseDeg, double travelled, bool reliable) {
    note(lat, lon);
    if (!reliable || sel < 0 || (lat == 0 && lon == 0)) return false;
    Axis ax;
    if (!axisOf(lat, lon, ax)) return false;
    bool inGate = fabs(ax.cross) <= START_LINE_HALF_M;
    if (!inGate) {
        // Leaving the line sideways is not a crossing.
        if (ax.along > -LINE_HYST_M) {
            armedBehind = false;
            crossPending = false;
        }
        return false;
    }
    if (ax.along <= -LINE_HYST_M) {
        if (!armedBehind) armTravel = travelled;
        armedBehind = true;
        crossPending = false;
        return false;
    }
    if (armedBehind && ax.along >= LINE_HYST_M) {
        crossPending = true;
        armedBehind = false;
    }
    if (!crossPending || ax.along > CONFIRM_PAST_M) {
        if (ax.along > CONFIRM_PAST_M) crossPending = false;
        return false;
    }
    if (speedMps < cfg::GPS_DIR_MIN_SPEED_MPS) return false;
    if (travelled - armTravel < CROSS_TRAVEL_M) return false;
    double diff = fabs(courseDeg - ax.bearing);
    if (diff > 180) diff = 360 - diff;
    if (diff > HEADING_LIMIT_DEG) return false;
    crossPending = false;
    pastStart = true;
    return true;
}

bool pollFinish(double lat, double lon, float speedMps, float courseDeg, double travelled, bool reliable) {
    if (!pastStart || sel < 0 || sel >= n || (lat == 0 && lon == 0)) {
        finishArmed = false;
        finishPending = false;
        return false;
    }
    track(lat, lon);
    if (sel < 0 || leg < items[sel].count - 2) {
        finishArmed = false;
        finishPending = false;
        return false;
    }
    if (!reliable) return false;
    Axis ax;
    if (!finishAxisOf(lat, lon, ax)) return false;
    bool inGate = fabs(ax.cross) <= START_LINE_HALF_M;
    if (!inGate) {
        if (ax.along > -LINE_HYST_M) {
            finishArmed = false;
            finishPending = false;
        }
        return false;
    }
    if (ax.along <= -LINE_HYST_M) {
        if (!finishArmed) finishArmTravel = travelled;
        finishArmed = true;
        finishPending = false;
        return false;
    }
    if (finishArmed && ax.along >= LINE_HYST_M) {
        finishPending = true;
        finishArmed = false;
    }
    if (!finishPending || ax.along > CONFIRM_PAST_M) {
        if (ax.along > CONFIRM_PAST_M) finishPending = false;
        return false;
    }
    if (speedMps < cfg::GPS_DIR_MIN_SPEED_MPS) return false;
    if (travelled - finishArmTravel < CROSS_TRAVEL_M) return false;
    double diff = fabs(courseDeg - ax.bearing);
    if (diff > 180) diff = 360 - diff;
    if (diff > HEADING_LIMIT_DEG) return false;
    finishPending = false;
    return true;
}

}  // namespace segments
