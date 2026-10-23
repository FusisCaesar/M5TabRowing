#include "map_data.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "config.h"

namespace mapdata {

namespace {

constexpr char MAGIC[4] = {'R', 'M', 'A', 'P'};

struct FileHeader {
    char path[64];
    double originLat, originLon;
    int32_t bbox[4];
    char name[32];
    uint32_t bytes;
};

FileHeader files[cfg::MAP_MAX_FILES];
int nFiles = 0;
int curIndex = -1;
bool pinned = false;  // a chosen file stays loaded until release()
Map cur;

// Sequential reader over a loaded byte buffer; memcpy avoids unaligned loads.
struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    template <typename T>
    T get() {
        T v{};
        if (p + sizeof(T) > end) {
            ok = false;
            return v;
        }
        memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    void read(void* dst, size_t n) {
        if (p + n > end) {
            ok = false;
            return;
        }
        memcpy(dst, p, n);
        p += n;
    }
    void skip(size_t n) { p += n; }
};

double metersPerDegLon(double lat) { return 111320.0 * cos(lat * M_PI / 180.0); }

bool projectTo(double oLat, double oLon, double lat, double lon, int32_t& x, int32_t& y) {
    double xm = (lon - oLon) * metersPerDegLon(oLat);
    double ym = (lat - oLat) * 111320.0;
    x = (int32_t)lround(xm * 10.0);
    y = (int32_t)lround(ym * 10.0);
    return true;
}

bool headerContains(const FileHeader& h, double lat, double lon) {
    int32_t x, y;
    projectTo(h.originLat, h.originLon, lat, lon, x, y);
    return x >= h.bbox[0] && x <= h.bbox[2] && y >= h.bbox[1] && y <= h.bbox[3];
}

bool readFileHeader(const char* path, FileHeader& out) {
    File f = SD_MMC.open(path);
    if (!f) return false;
    uint8_t buf[84];
    bool ok = f.read(buf, sizeof(buf)) == (int)sizeof(buf);
    out.bytes = f.size();
    f.close();
    if (!ok || memcmp(buf, MAGIC, 4) != 0) return false;
    uint16_t version;
    memcpy(&version, buf + 4, 2);
    if (version != 1) return false;
    strlcpy(out.path, path, sizeof(out.path));
    memcpy(&out.originLat, buf + 8, 8);
    memcpy(&out.originLon, buf + 16, 8);
    memcpy(out.bbox, buf + 24, 16);
    memcpy(out.name, buf + 40, 32);
    out.name[31] = '\0';
    return true;
}

SemaphoreHandle_t mutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}

void freeCurrent() {
    Guard guard;
    heap_caps_free(cur.features);
    heap_caps_free(cur.rings);
    heap_caps_free(cur.points);
    cur = Map{};
}

bool loadFile(int index) {
    if (index < 0 || index >= nFiles) return false;
    const FileHeader& fh = files[index];
    File f = SD_MMC.open(fh.path);
    if (!f) return false;
    size_t size = f.size();
    uint8_t* raw = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!raw) {
        f.close();
        Serial.printf("[map] out of PSRAM for %s\n", fh.path);
        return false;
    }
    size_t got = f.read(raw, size);
    f.close();
    if (got != size) {
        heap_caps_free(raw);
        return false;
    }

    Cursor c{raw, raw + size};
    c.skip(4 + 2 + 2);  // magic, version, flags
    double originLat = c.get<double>();
    double originLon = c.get<double>();
    int32_t bbox[4];
    c.read(bbox, sizeof(bbox));
    char name[32];
    c.read(name, sizeof(name));
    name[31] = '\0';
    uint32_t featureCount = c.get<uint32_t>();
    uint32_t ringTotal = c.get<uint32_t>();
    uint32_t pointTotal = c.get<uint32_t>();
    if (!c.ok) {
        heap_caps_free(raw);
        return false;
    }

    Feature* features = (Feature*)heap_caps_malloc(sizeof(Feature) * featureCount, MALLOC_CAP_SPIRAM);
    Ring* rings = (Ring*)heap_caps_malloc(sizeof(Ring) * (ringTotal ? ringTotal : 1), MALLOC_CAP_SPIRAM);
    int32_t* points = (int32_t*)heap_caps_malloc(sizeof(int32_t) * 2 * (pointTotal ? pointTotal : 1),
                                                 MALLOC_CAP_SPIRAM);
    if (!features || !rings || !points) {
        heap_caps_free(features);
        heap_caps_free(rings);
        heap_caps_free(points);
        heap_caps_free(raw);
        Serial.printf("[map] out of PSRAM building %s\n", fh.path);
        return false;
    }

    uint32_t ringIdx = 0, pointIdx = 0;
    for (uint32_t i = 0; i < featureCount && c.ok; i++) {
        Feature& ft = features[i];
        ft.kind = c.get<uint8_t>();
        ft.flags = c.get<uint8_t>();
        ft.widthDm = c.get<uint16_t>();
        c.read(ft.bbox, sizeof(ft.bbox));
        uint8_t nameLen = c.get<uint8_t>();
        memset(ft.name, 0, sizeof(ft.name));
        if (nameLen > sizeof(ft.name) - 1) {
            c.read(ft.name, sizeof(ft.name) - 1);
            c.skip(nameLen - (sizeof(ft.name) - 1));
        } else {
            c.read(ft.name, nameLen);
        }
        ft.ringCount = c.get<uint16_t>();
        ft.ringFirst = ringIdx;
        for (uint16_t r = 0; r < ft.ringCount && c.ok; r++) {
            uint16_t pc = c.get<uint16_t>();
            if (ringIdx >= ringTotal || pointIdx + pc > pointTotal) {
                c.ok = false;
                break;
            }
            rings[ringIdx].first = pointIdx;
            rings[ringIdx].count = pc;
            ringIdx++;
            c.read(&points[pointIdx * 2], (size_t)pc * 2 * sizeof(int32_t));
            pointIdx += pc;
        }
    }
    heap_caps_free(raw);
    if (!c.ok) {
        heap_caps_free(features);
        heap_caps_free(rings);
        heap_caps_free(points);
        Serial.printf("[map] corrupt file %s\n", fh.path);
        return false;
    }

    Guard guard;
    freeCurrent();
    cur.valid = true;
    cur.originLat = originLat;
    cur.originLon = originLon;
    memcpy(cur.bbox, bbox, sizeof(bbox));
    strlcpy(cur.name, name, sizeof(cur.name));
    cur.features = features;
    cur.featureCount = (int)featureCount;
    cur.rings = rings;
    cur.ringCount = (int)ringIdx;
    cur.points = points;
    cur.pointPairCount = (int)pointIdx;
    curIndex = index;
    Serial.printf("[map] loaded %s (%s): %d features\n", fh.path, cur.name, cur.featureCount);
    return true;
}

}  // namespace

Guard::Guard() { xSemaphoreTakeRecursive(mutex(), portMAX_DELAY); }
Guard::~Guard() { xSemaphoreGiveRecursive(mutex()); }

void begin() {
    nFiles = 0;
    curIndex = -1;
    pinned = false;
    File dir = SD_MMC.open("/map");
    if (dir) {
        for (File f = dir.openNextFile(); f && nFiles < cfg::MAP_MAX_FILES; f = dir.openNextFile()) {
            const char* name = f.name();
            size_t len = strlen(name);
            bool isRmap = len > 5 && strcasecmp(name + len - 5, ".rmap") == 0;
            char path[64];
            // openNextFile() may return a bare name or a full path depending on core version.
            if (name[0] == '/') strlcpy(path, name, sizeof(path));
            else snprintf(path, sizeof(path), "/map/%s", name);
            f.close();
            if (isRmap && readFileHeader(path, files[nFiles])) nFiles++;
        }
        dir.close();
    }
    Serial.printf("[map] %d map file(s) on card\n", nFiles);
    if (nFiles > 0) loadFile(0);
}

void update(double lat, double lon, bool haveFix) {
    if (nFiles == 0) return;
    if (pinned && cur.valid) return;
    if (haveFix) {
        if (cur.valid && curIndex >= 0 && headerContains(files[curIndex], lat, lon)) return;
        for (int i = 0; i < nFiles; i++) {
            if (headerContains(files[i], lat, lon)) {
                if (i != curIndex) loadFile(i);
                return;
            }
        }
        // Outside every map: keep what we have.
        return;
    }
    if (!cur.valid) loadFile(0);
}

const Map& current() { return cur; }
bool loaded() { return cur.valid; }
int fileCount() { return nFiles; }

bool fileInfo(int i, FileInfo& out) {
    if (i < 0 || i >= nFiles) return false;
    const FileHeader& h = files[i];
    out.path = h.path;
    out.name = h.name;
    out.bytes = h.bytes;
    double mLon = metersPerDegLon(h.originLat);
    out.south = h.originLat + h.bbox[1] / 10.0 / 111320.0;
    out.north = h.originLat + h.bbox[3] / 10.0 / 111320.0;
    out.west = h.originLon + h.bbox[0] / 10.0 / mLon;
    out.east = h.originLon + h.bbox[2] / 10.0 / mLon;
    out.loaded = cur.valid && i == curIndex;
    return true;
}

bool removeFile(int i) {
    if (i < 0 || i >= nFiles) return false;
    char path[64];
    strlcpy(path, files[i].path, sizeof(path));
    if (i == curIndex) freeCurrent();
    bool ok = SD_MMC.remove(path);
    Serial.printf("[map] delete %s: %s\n", path, ok ? "OK" : "failed");
    begin();
    return ok;
}

bool renameFile(int i, const char* name) {
    if (i < 0 || i >= nFiles || !name[0]) return false;
    constexpr size_t NAME_OFFSET = 40;  // magic, version, flags, origin, bbox
    char field[32] = {0};
    strncpy(field, name, sizeof(field) - 1);
    File f = SD_MMC.open(files[i].path, "r+");
    bool ok = f && f.seek(NAME_OFFSET) && f.write((const uint8_t*)field, sizeof(field)) == sizeof(field);
    if (f) f.close();
    if (ok) {
        strlcpy(files[i].name, field, sizeof(files[i].name));
        Guard guard;
        if (i == curIndex) strlcpy(cur.name, field, sizeof(cur.name));
    }
    Serial.printf("[map] rename %s to '%s': %s\n", files[i].path, field, ok ? "OK" : "failed");
    return ok;
}

bool show(const char* path) {
    for (int i = 0; i < nFiles; i++)
        if (strcmp(files[i].path, path) == 0) return i == curIndex || loadFile(i);
    return false;
}

bool showIndex(int i) {
    if (i < 0 || i >= nFiles) return false;
    bool ok = i == curIndex || loadFile(i);
    if (ok) pinned = true;
    return ok;
}

void release() { pinned = false; }

bool held() { return pinned && cur.valid; }

bool project(double lat, double lon, int32_t& x, int32_t& y) {
    if (!cur.valid) return false;
    return projectTo(cur.originLat, cur.originLon, lat, lon, x, y);
}

bool centerLatLon(double& lat, double& lon) {
    if (!cur.valid) return false;
    double cx = (cur.bbox[0] + cur.bbox[2]) / 2.0 / 10.0;  // metres
    double cy = (cur.bbox[1] + cur.bbox[3]) / 2.0 / 10.0;
    lat = cur.originLat + cy / 111320.0;
    lon = cur.originLon + cx / metersPerDegLon(cur.originLat);
    return true;
}

}  // namespace mapdata
