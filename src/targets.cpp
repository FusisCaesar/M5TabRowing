#include "targets.h"

#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "map_data.h"
#include "segments.h"

namespace targets {

namespace {

Target items[MAX];
int n = 0;
int sel = -1;

void persist() {
    Preferences prefs;
    prefs.begin("targets", false);
    prefs.putUChar("n", (uint8_t)n);
    prefs.putUChar("sel", (uint8_t)(sel + 1));  // 0 = none
    prefs.putUChar("v", 2);
    prefs.putBytes("v2", items, sizeof(items));
    prefs.end();
}

// True when this target already uses the number, including a renamed "Map - T3".
bool numberUsed(int num) {
    char tag[8];
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
    for (int num = 1; num < 100; num++)
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
    n = prefs.getUChar("n", 0);
    sel = (int)prefs.getUChar("sel", 0) - 1;
    if (n < 0 || n > MAX) n = 0;
    if (n > 0 && prefs.getUChar("v", 1) >= 2) {
        prefs.getBytes("v2", items, sizeof(items));
    } else if (n > 0) {
        // The first version stored a 12-character name in front of the coordinates.
        struct Old {
            char name[12];
            double lat, lon;
        } old[MAX];
        memset(old, 0, sizeof(old));
        prefs.getBytes("items", old, sizeof(old));
        for (int i = 0; i < n; i++) {
            strlcpy(items[i].name, old[i].name, sizeof(items[i].name));
            items[i].lat = old[i].lat;
            items[i].lon = old[i].lon;
        }
        prefs.putUChar("v", 2);
        prefs.putBytes("v2", items, sizeof(items));
    }
    prefs.end();
    if (sel < -1 || sel >= n) sel = -1;
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
    if (n >= MAX) return false;
    int num = nextNumber();
    if (!num) return false;
    const char* map = mapNameAt(lat, lon);
    if (map && map[0]) snprintf(items[n].name, sizeof(items[n].name), "%s - T%d", map, num);
    else snprintf(items[n].name, sizeof(items[n].name), "T%d", num);
    items[n].lat = lat;
    items[n].lon = lon;
    sel = n;
    n++;
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
