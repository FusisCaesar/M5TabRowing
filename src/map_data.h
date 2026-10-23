#pragma once

#include <stdint.h>

// Offline waterway maps built by tools/make_map.py and stored as /map/*.rmap on
// the SD card. One file covers one area, projected to local metres (stored as
// decimetres relative to the file's origin). This module scans the card, keeps the
// area that contains the current fix loaded in PSRAM, and projects lat/lon to the
// loaded map's coordinates for the renderer.
namespace mapdata {

// Feature kinds, mirrored in tools/make_map.py.
enum Kind : uint8_t { WaterArea = 0, WaterLine, Bridge, Lock, Weir, Pier, Club };

enum Flags : uint8_t {
    F_LINE = 1 << 0,   // open polyline (else filled rings with even-odd holes)
    F_POINT = 1 << 1,  // single point
};

struct Ring {
    uint32_t first;  // index of the first point pair in the point pool
    uint32_t count;  // number of points
};

struct Feature {
    uint8_t kind;
    uint8_t flags;
    uint16_t widthDm;   // line width in decimetres (water lines)
    int32_t bbox[4];    // minx, miny, maxx, maxy in decimetres
    uint16_t ringCount;
    uint32_t ringFirst;  // index into the ring array
    char name[24];
};

struct Map {
    bool valid = false;
    double originLat = 0, originLon = 0;
    int32_t bbox[4] = {0, 0, 0, 0};
    char name[32] = "";
    Feature* features = nullptr;
    int featureCount = 0;
    Ring* rings = nullptr;
    int ringCount = 0;
    int32_t* points = nullptr;  // x,y interleaved, decimetres
    int pointPairCount = 0;
};

void begin();

// Hold while reading current() from another task (the map is drawn in the background);
// replacing or freeing the loaded map waits for it.
struct Guard {
    Guard();
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// Loads the area whose bounds contain the fix, switching files as you move. With
// no fix, keeps whatever is loaded (or the first file, from begin()).
void update(double lat, double lon, bool haveFix);

const Map& current();
bool loaded();
int fileCount();

struct FileInfo {
    const char* path;
    const char* name;
    uint32_t bytes;
    double south, west, north, east;
    bool loaded;
};

// Map files found by begin(), in card order.
bool fileInfo(int i, FileInfo& out);
// Deletes the file from the card and rescans.
bool removeFile(int i);
// Changes the name stored in the file's header (up to 31 characters); the file name stays.
bool renameFile(int i, const char* name);
// Loads this file now (e.g. a fresh download); update() still switches by position later.
bool show(const char* path);
// Loads a file from the card list and keeps it on screen until release(), so a fix
// on another water does not switch it away while targets are being placed.
bool showIndex(int i);
void release();
bool held();

// Projects lat/lon to the loaded map's decimetre coordinates. False if no map.
bool project(double lat, double lon, int32_t& x, int32_t& y);

// Centre of the loaded map in lat/lon (for the simulator and no-fix framing).
bool centerLatLon(double& lat, double& lon);

}  // namespace mapdata
