#include "targets.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "map_data.h"
#include "segments.h"

namespace targets {

namespace {

Target* items = nullptr;
int n = 0;
int cap = 0;
int sel = -1;

bool grow(int need) {
    if (need <= cap) return true;
    int ncap = cap < 8 ? 8 : cap;
    while (ncap < need) ncap *= 2;
    Target* p = (Target*)heap_caps_realloc(items, ncap * sizeof(Target), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (Target*)heap_caps_realloc(items, ncap * sizeof(Target), MALLOC_CAP_8BIT);
    if (!p) return false;
    items = p;
    cap = ncap;
    return true;
}

// False when flash cannot hold the list. The points stay in memory either way.
bool persist() {
    Preferences prefs;
    prefs.begin("targets", false);
    prefs.putUShort("n", (uint16_t)n);
    prefs.putUShort("sel", (uint16_t)(sel + 1));  // 0 = none
    prefs.putUChar("v", 3);
    bool ok = true;
    if (n > 0) ok = prefs.putBytes("v3", items, n * sizeof(Target)) == n * sizeof(Target);
    else prefs.remove("v3");
    prefs.end();
    return ok;
}

// True when this target already uses the number, including a renamed "Map - T3".
bool numberUsed(int num) {
    char tag[16];
    snprintf(tag, sizeof(tag), "T%d", num);
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

// Name of the map file that covers this point, preferring the one on screen.
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

}  // namespace

void begin() {
    Preferences prefs;
    prefs.begin("targets", false);
    int ver = prefs.getUChar("v", 1);
    bool rewrite = ver < 3;
    if (ver >= 3) {
        n = prefs.getUShort("n", 0);
        sel = (int)prefs.getUShort("sel", 0) - 1;
        if (n > 0 && grow(n)) prefs.getBytes("v3", items, n * sizeof(Target));
        else n = 0;
    } else if (ver >= 2) {
        n = prefs.getUChar("n", 0);
        sel = (int)prefs.getUChar("sel", 0) - 1;
        if (n > 12) n = 12;
        if (n > 0 && grow(n)) prefs.getBytes("v2", items, n * sizeof(Target));
        else n = 0;
    } else if (prefs.getUChar("n", 0) > 0) {
        // The first version stored twelve 12-character names in front of the coordinates.
        n = prefs.getUChar("n", 0);
        sel = (int)prefs.getUChar("sel", 0) - 1;
        if (n > 12) n = 12;
        struct Old {
            char name[12];
            double lat, lon;
        } old[12];
        memset(old, 0, sizeof(old));
        prefs.getBytes("items", old, sizeof(old));
        if (grow(n)) {
            for (int i = 0; i < n; i++) {
                strlcpy(items[i].name, old[i].name, sizeof(items[i].name));
                items[i].lat = old[i].lat;
                items[i].lon = old[i].lon;
            }
        } else {
            n = 0;
        }
    }
    prefs.end();
    if (sel < -1 || sel >= n) {
        sel = -1;
        rewrite = true;
    }
    if (rewrite) persist();
}

int count() { return n; }

const Target* get(int index) {
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
    persist();
}

bool add(double lat, double lon) {
    if (!grow(n + 1)) return false;
    int num = nextNumber();
    if (!num) return false;
    const char* map = mapNameAt(lat, lon);
    if (map && map[0]) snprintf(items[n].name, sizeof(items[n].name), "%s - T%d", map, num);
    else snprintf(items[n].name, sizeof(items[n].name), "T%d", num);
    items[n].lat = lat;
    items[n].lon = lon;
    int prev = sel;
    sel = n;
    n++;
    if (!persist()) {
        n--;
        sel = prev;
        return false;
    }
    return true;
}

void move(int index, double lat, double lon, bool save) {
    if (index < 0 || index >= n) return;
    items[index].lat = lat;
    items[index].lon = lon;
    if (save) persist();
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
    for (int j = index; j < n - 1; j++) items[j] = items[j + 1];
    n--;
    if (sel == index) sel = -1;
    else if (sel > index) sel--;
    persist();
    segments::onTargetRemoved(index);
}

float metresBetween(double lat, double lon, double lat2, double lon2) {
    double dy = (lat2 - lat) * 111320.0;
    double dx = (lon2 - lon) * 111320.0 * cos(lat * M_PI / 180.0);
    return (float)sqrt(dx * dx + dy * dy);
}

float distanceFrom(double lat, double lon) {
    if (sel < 0 || sel >= n) return -1;
    return metresBetween(lat, lon, items[sel].lat, items[sel].lon);
}

}  // namespace targets
