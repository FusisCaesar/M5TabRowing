#include "segments.h"

#include <Preferences.h>
#include <math.h>
#include <stdio.h>
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

Segment items[MAX];
int n = 0;
int sel = -1;
int leg = 0;      // leg of the selected segment the boat is on
int nextPt = 0;   // point the boat is heading for
bool pastStart = false;
bool armedBehind = false;
bool crossPending = false;
double armTravel = 0;

void resetRun() {
    leg = 0;
    nextPt = 0;
    pastStart = false;
    armedBehind = false;
    crossPending = false;
    armTravel = 0;
}

void persist() {
    Preferences prefs;
    prefs.begin("segments", false);
    prefs.putUChar("n", (uint8_t)n);
    prefs.putUChar("sel", (uint8_t)(sel + 1));
    prefs.putBytes("v1", items, sizeof(items));
    prefs.end();
}

void eraseAt(int index) {
    for (int j = index; j < n - 1; j++) items[j] = items[j + 1];
    n--;
    if (sel == index) sel = -1;
    else if (sel > index) sel--;
}

bool numberUsed(int num) {
    char tag[8];
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
    for (int num = 1; num < 100; num++)
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

bool valid(const int8_t* points, int npoints) {
    if (!points || npoints < 2 || npoints > MAX_POINTS) return false;
    for (int i = 0; i < npoints; i++) {
        if (!targets::get(points[i])) return false;
        for (int j = 0; j < i; j++)
            if (points[j] == points[i]) return false;
    }
    return true;
}

void copyPoints(Segment& s, const int8_t* points, int npoints) {
    s.count = (uint8_t)npoints;
    for (int i = 0; i < npoints; i++) s.point[i] = points[i];
    for (int i = npoints; i < MAX_POINTS; i++) s.point[i] = -1;
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

bool axisOf(double lat, double lon, Axis& ax) {
    if (sel < 0 || sel >= n) return false;
    const targets::Target* a = pointAt(items[sel], 0);
    const targets::Target* b = pointAt(items[sel], 1);
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
    n = prefs.getUChar("n", 0);
    sel = (int)prefs.getUChar("sel", 0) - 1;
    if (n < 0 || n > MAX) n = 0;
    memset(items, 0, sizeof(items));
    if (n > 0) prefs.getBytes("v1", items, sizeof(items));
    prefs.end();

    bool dirty = false;
    for (int i = n - 1; i >= 0; i--) {
        int w = 0;
        int count = items[i].count;
        if (count < 0 || count > MAX_POINTS) count = 0;
        for (int j = 0; j < count; j++) {
            if (!targets::get(items[i].point[j])) {
                dirty = true;
                continue;
            }
            items[i].point[w++] = items[i].point[j];
        }
        items[i].count = (uint8_t)w;
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

bool add(const int8_t* points, int npoints) {
    if (n >= MAX || !valid(points, npoints)) return false;
    int num = nextNumber();
    if (!num) return false;
    const targets::Target* start = targets::get(points[0]);
    const char* map = start ? mapNameAt(start->lat, start->lon) : nullptr;
    if (map && map[0]) snprintf(items[n].name, sizeof(items[n].name), "%s - S%d", map, num);
    else snprintf(items[n].name, sizeof(items[n].name), "S%d", num);
    copyPoints(items[n], points, npoints);
    sel = n;
    n++;
    resetRun();
    persist();
    return true;
}

bool update(int index, const int8_t* points, int npoints) {
    if (index < 0 || index >= n || !valid(points, npoints)) return false;
    copyPoints(items[index], points, npoints);
    if (index == sel) resetRun();
    persist();
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
            items[i].point[w++] = (int8_t)p;
        }
        items[i].count = (uint8_t)w;
        for (int j = w; j < MAX_POINTS; j++) items[i].point[j] = -1;
        if (w < 2) {
            eraseAt(i);
            dirty = true;
        }
    }
    if (sel >= n) sel = -1;
    resetRun();
    if (dirty) persist();
}

float lengthAlong(const int8_t* points, int npoints) {
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
    if (index < 0 || index >= n) return -1;
    return lengthAlong(items[index].point, items[index].count);
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

bool startLine(double& lat1, double& lon1, double& lat2, double& lon2) {
    if (sel < 0 || sel >= n) return false;
    const targets::Target* a = pointAt(items[sel], 0);
    const targets::Target* b = pointAt(items[sel], 1);
    if (!a || !b) return false;
    double abE, abN;
    metresDelta(a->lat, a->lon, b->lat, b->lon, abE, abN);
    double len = sqrt(abE * abE + abN * abN);
    if (len < 1) return false;
    // Square to the first leg, START_LINE_HALF_M to each side.
    double east = -abN / len * START_LINE_HALF_M;
    double north = abE / len * START_LINE_HALF_M;
    shiftFrom(a->lat, a->lon, east, north, lat1, lon1);
    shiftFrom(a->lat, a->lon, -east, -north, lat2, lon2);
    return true;
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

}  // namespace segments
