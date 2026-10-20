#include "map_view.h"

#include <esp_heap_caps.h>
#include <math.h>

#include "config.h"
#include "icons.h"
#include "map_data.h"
#include "track.h"

namespace mapview {

namespace {

// Palette (RGB565 via color565 at use).
uint16_t COL_LAND, COL_WATER, COL_BRIDGE, COL_LOCK, COL_WEIR, COL_PIER, COL_CLUB, COL_TRACK, COL_BOAT,
    COL_TEXT, COL_SHADE;
bool paletteReady = false;
bool paletteDay = true;  // force a first init against View.day (default night)

// Reusable scratch for transformed feature points.
constexpr int SCRATCH = 20000;
float* sxBuf = nullptr;
float* syBuf = nullptr;
constexpr int MAX_RINGS = 512;
int ringStart[MAX_RINGS];
int ringCnt[MAX_RINGS];
constexpr int MAX_XINTS = 512;
float xints[MAX_XINTS];

constexpr int ZOOM_SMALL = 64;
constexpr int ZOOM_LARGE = 128;
constexpr int ZOOM_MARGIN = 12;
constexpr int ZOOM_GAP = 8;

int zoomSize(bool large) { return large ? ZOOM_LARGE : ZOOM_SMALL; }

float rotationDeg(uint8_t orientation, float course) {
    if (orientation == BowUp) return course;
    if (orientation == HeadUp) return course + 180.0f;
    return 0.0f;
}

float rotationDeg(const View& v) { return rotationDeg(v.orientation, v.course); }

void initPalette(M5Canvas& c, bool day) {
    if (paletteReady && paletteDay == day) return;
    paletteDay = day;
    paletteReady = true;
    if (day) {
        COL_LAND = c.color565(228, 224, 214);
        COL_WATER = c.color565(120, 172, 220);
        COL_BRIDGE = c.color565(92, 92, 98);
        COL_LOCK = c.color565(205, 70, 70);
        COL_WEIR = c.color565(170, 110, 50);
        COL_PIER = c.color565(140, 140, 140);
        COL_CLUB = c.color565(210, 60, 60);
        COL_TRACK = c.color565(245, 120, 25);
        COL_BOAT = c.color565(255, 210, 0);
        COL_TEXT = c.color565(40, 40, 40);
        COL_SHADE = c.color565(255, 255, 255);
    } else {
        COL_LAND = c.color565(28, 30, 36);
        COL_WATER = c.color565(48, 92, 148);
        COL_BRIDGE = c.color565(78, 80, 88);
        COL_LOCK = c.color565(220, 78, 78);
        COL_WEIR = c.color565(176, 122, 58);
        COL_PIER = c.color565(110, 112, 120);
        COL_CLUB = c.color565(230, 86, 86);
        COL_TRACK = c.color565(255, 148, 40);
        COL_BOAT = c.color565(255, 214, 64);
        COL_TEXT = c.color565(230, 230, 232);
        COL_SHADE = c.color565(42, 44, 52);
    }
}

uint16_t featureColor(uint8_t kind) {
    switch (kind) {
        case mapdata::WaterArea:
        case mapdata::WaterLine: return COL_WATER;
        case mapdata::Bridge: return COL_BRIDGE;
        case mapdata::Lock: return COL_LOCK;
        case mapdata::Weir: return COL_WEIR;
        case mapdata::Pier: return COL_PIER;
        case mapdata::Club: return COL_CLUB;
    }
    return COL_BRIDGE;
}

struct Xform {
    float bx, by;        // boat position, decimetres
    float cosB, sinB;    // rotation
    float invmpp;        // pixels per metre
    float cx, cy;        // canvas centre for the boat
    void toScreen(int32_t px, int32_t py, float& sx, float& sy) const {
        float dx = (px - bx) * 0.1f;  // metres
        float dy = (py - by) * 0.1f;
        float xr = dx * cosB - dy * sinB;
        float yr = dx * sinB + dy * cosB;
        sx = cx + xr * invmpp;
        sy = cy - yr * invmpp;  // world y up -> screen y down
    }
};

// Even-odd scanline fill of all rings currently in the scratch buffers.
void fillRings(M5Canvas& c, int nr, uint16_t color) {
    int h = c.height(), w = c.width();
    float minY = 1e9f, maxY = -1e9f;
    for (int r = 0; r < nr; r++) {
        for (int i = 0; i < ringCnt[r]; i++) {
            float y = syBuf[ringStart[r] + i];
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    int y0 = (int)fmaxf(0, floorf(minY));
    int y1 = (int)fminf(h - 1, ceilf(maxY));
    for (int y = y0; y <= y1; y++) {
        float yc = y + 0.5f;
        int n = 0;
        for (int r = 0; r < nr && n < MAX_XINTS; r++) {
            int s = ringStart[r], cnt = ringCnt[r];
            for (int k = 0; k < cnt; k++) {
                int a = s + k, b = s + (k + 1) % cnt;
                float ya = syBuf[a], yb = syBuf[b];
                if ((ya <= yc) == (yb <= yc)) continue;  // no crossing
                float t = (yc - ya) / (yb - ya);
                if (n < MAX_XINTS) xints[n++] = sxBuf[a] + t * (sxBuf[b] - sxBuf[a]);
            }
        }
        if (n < 2) continue;
        // insertion sort (spans are short)
        for (int i = 1; i < n; i++) {
            float v = xints[i];
            int j = i - 1;
            while (j >= 0 && xints[j] > v) {
                xints[j + 1] = xints[j];
                j--;
            }
            xints[j + 1] = v;
        }
        for (int i = 0; i + 1 < n; i += 2) {
            int xa = (int)ceilf(xints[i]);
            int xb = (int)floorf(xints[i + 1]);
            if (xb < 0 || xa > w - 1) continue;
            xa = max(xa, 0);
            xb = min(xb, w - 1);
            if (xb >= xa) c.drawFastHLine(xa, y, xb - xa + 1, color);
        }
    }
}

// Transforms one feature's rings into the scratch buffers. Returns ring count, or
// -1 if it does not fit.
int transformFeature(const mapdata::Map& m, const mapdata::Feature& ft, const Xform& xf) {
    int nr = 0, total = 0;
    for (int r = 0; r < ft.ringCount && nr < MAX_RINGS; r++) {
        const mapdata::Ring& ring = m.rings[ft.ringFirst + r];
        if (total + (int)ring.count > SCRATCH) break;
        ringStart[nr] = total;
        ringCnt[nr] = ring.count;
        const int32_t* pts = &m.points[ring.first * 2];
        for (uint32_t i = 0; i < ring.count; i++) {
            xf.toScreen(pts[i * 2], pts[i * 2 + 1], sxBuf[total], syBuf[total]);
            total++;
        }
        nr++;
    }
    return nr;
}

void drawLineFeature(M5Canvas& c, int nr, float widthPx, uint16_t color) {
    int wi = (int)fmaxf(1.0f, widthPx);
    for (int r = 0; r < nr; r++) {
        int s = ringStart[r], cnt = ringCnt[r];
        for (int k = 0; k + 1 < cnt; k++) {
            c.drawWideLine(sxBuf[s + k], syBuf[s + k], sxBuf[s + k + 1], syBuf[s + k + 1], wi, color);
        }
    }
}

bool boxOverlap(const int32_t* a, float minX, float minY, float maxX, float maxY) {
    return !(a[2] < minX || a[0] > maxX || a[3] < minY || a[1] > maxY);
}

void drawScaleBar(M5Canvas& c, float mpp) {
    const int nice[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    float target = c.width() * 0.22f * mpp;
    int meters = nice[0];
    for (int v : nice)
        if (v <= target) meters = v;
    int px = (int)(meters / mpp);
    int x = 16, y = c.height() - 18;
    c.drawFastHLine(x, y, px, COL_TEXT);
    c.drawFastVLine(x, y - 5, 10, COL_TEXT);
    c.drawFastVLine(x + px, y - 5, 10, COL_TEXT);
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextSize(1);
    c.setTextDatum(bottom_left);
    c.setTextColor(COL_TEXT);
    char buf[16];
    if (meters >= 1000) snprintf(buf, sizeof(buf), "%.1f km", meters / 1000.0f);
    else snprintf(buf, sizeof(buf), "%d m", meters);
    c.drawString(buf, x + 4, y - 4);
}

void drawNorthArrow(M5Canvas& c, const Xform& xf) {
    // North (world +y) direction on screen.
    float nx = -xf.sinB, ny = -xf.cosB;
    float cx = c.width() - 30, cy = 34, L = 16;
    c.drawWideLine(cx - nx * L, cy - ny * L, cx + nx * L, cy + ny * L, 3, COL_LOCK);
    c.fillCircle(cx + nx * L, cy + ny * L, 4, COL_LOCK);
    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextDatum(middle_center);
    c.setTextColor(COL_TEXT);
    c.drawString("N", cx + nx * (L + 10), cy + ny * (L + 10));
}

void drawBoat(M5Canvas& c, float cx, float cy, float courseDeg, float mapBearingDeg) {
    float bearing = courseDeg - mapBearingDeg;
    float a = bearing * (float)M_PI / 180.0f;
    float dirx = sinf(a), diry = -cosf(a);  // screen: up is -y
    float perpx = -diry, perpy = dirx;
    float L = 16, Wd = 9;
    float tipx = cx + dirx * L, tipy = cy + diry * L;
    float blx = cx - dirx * (L * 0.6f) + perpx * Wd, bly = cy - diry * (L * 0.6f) + perpy * Wd;
    float brx = cx - dirx * (L * 0.6f) - perpx * Wd, bry = cy - diry * (L * 0.6f) - perpy * Wd;
    c.fillTriangle(tipx, tipy, blx, bly, brx, bry, COL_BOAT);
    c.drawTriangle(tipx, tipy, blx, bly, brx, bry, COL_TEXT);
}

icons::Icon orientationIcon(uint8_t o) {
    if (o == BowUp) return icons::Icon::BoatBow;
    if (o == NorthUp) return icons::Icon::Compass;
    return icons::Icon::BoatHead;
}

void drawControls(M5Canvas& c, const View& v) {
    int z = zoomSize(v.largeControls);
    int x = c.width() - z - ZOOM_MARGIN;
    int yOut = c.height() - z - ZOOM_MARGIN;
    int yIn = yOut - ZOOM_GAP - z;
    int r = v.largeControls ? 16 : 12;
    auto button = [&](int y) {
        c.fillRoundRect(x, y, z, z, r, COL_SHADE);
        c.drawRoundRect(x, y, z, z, r, COL_TEXT);
    };
    if (v.largeControls) {
        int yO = yIn - ZOOM_GAP - z;
        button(yO);
        icons::draw(c, orientationIcon(v.orientation), x + z / 2, yO + z / 2, (int)(z * 0.55f), COL_TEXT, COL_SHADE);
    }
    button(yIn);
    button(yOut);
    if (v.showRecenter) {
        int rx = x - ZOOM_GAP - z;
        c.fillRoundRect(rx, yOut, z, z, r, COL_SHADE);
        c.drawRoundRect(rx, yOut, z, z, r, COL_TEXT);
        icons::draw(c, icons::Icon::Locate, rx + z / 2, yOut + z / 2, (int)(z * 0.55f), COL_TEXT, COL_SHADE);
    }
    c.setFont(v.largeControls ? &fonts::FreeSansBold24pt7b : &fonts::FreeSansBold18pt7b);
    c.setTextSize(1);
    c.setTextDatum(middle_center);
    c.setTextColor(COL_TEXT);
    c.drawString("+", x + z / 2, yIn + z / 2 - 2);
    c.drawString("-", x + z / 2, yOut + z / 2 - 2);
}

void drawStatus(M5Canvas& c, const char* msg) {
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextSize(1);
    c.setTextDatum(middle_center);
    c.setTextColor(COL_TEXT);
    c.drawString(msg, c.width() / 2, c.height() / 2);
}

}  // namespace

void begin() {
    sxBuf = (float*)heap_caps_malloc(sizeof(float) * SCRATCH, MALLOC_CAP_SPIRAM);
    syBuf = (float*)heap_caps_malloc(sizeof(float) * SCRATCH, MALLOC_CAP_SPIRAM);
    if (!sxBuf || !syBuf) Serial.println("[mapview] scratch alloc failed");
}

const char* orientationName(uint8_t o) {
    switch (o) {
        case BowUp: return "BOW UP";
        case NorthUp: return "NORTH UP";
        default: return "HEAD UP";
    }
}

bool zoomInRect(const M5Canvas& c, int& x, int& y, int& w, int& h, bool large) {
    int z = zoomSize(large);
    x = c.width() - z - ZOOM_MARGIN;
    y = c.height() - 2 * z - ZOOM_MARGIN - ZOOM_GAP;
    w = h = z;
    return true;
}

bool zoomOutRect(const M5Canvas& c, int& x, int& y, int& w, int& h, bool large) {
    int z = zoomSize(large);
    x = c.width() - z - ZOOM_MARGIN;
    y = c.height() - z - ZOOM_MARGIN;
    w = h = z;
    return true;
}

bool orientationRect(const M5Canvas& c, int& x, int& y, int& w, int& h, bool large) {
    if (!large) return false;
    int z = ZOOM_LARGE;
    x = c.width() - z - ZOOM_MARGIN;
    y = c.height() - 3 * z - ZOOM_MARGIN - 2 * ZOOM_GAP;
    w = h = z;
    return true;
}

bool recenterRect(const M5Canvas& c, int& x, int& y, int& w, int& h, bool large) {
    int z = zoomSize(large);
    x = c.width() - 2 * z - ZOOM_MARGIN - ZOOM_GAP;
    y = c.height() - z - ZOOM_MARGIN;
    w = h = z;
    return true;
}

void panCenter(double& lat, double& lon, float dxPx, float dyPx, float metersPerPixel, uint8_t orientation,
               float course) {
    float B = rotationDeg(orientation, course) * (float)M_PI / 180.0f;
    float cosB = cosf(B), sinB = sinf(B);
    float xr = -dxPx * metersPerPixel;
    float yr = dyPx * metersPerPixel;
    float east = xr * cosB + yr * sinB;
    float north = -xr * sinB + yr * cosB;
    lat += north / 111320.0;
    lon += east / (111320.0 * cos(lat * M_PI / 180.0));
}

static void renderLocked(M5Canvas& c, const View& v);

void render(M5Canvas& c, const View& v) {
    // The scratch buffers are shared, and the main map is drawn from its own task.
    static SemaphoreHandle_t busy = xSemaphoreCreateMutex();
    xSemaphoreTake(busy, portMAX_DELAY);
    {
        mapdata::Guard guard;
        renderLocked(c, v);
    }
    xSemaphoreGive(busy);
}

static void renderLocked(M5Canvas& c, const View& v) {
    initPalette(c, v.day);
    c.fillSprite(COL_LAND);

    const mapdata::Map& m = mapdata::current();
    bool review = v.trackCount > 0;
    bool haveMap = mapdata::loaded();
    if (!haveMap && !review) {
        drawStatus(c, mapdata::fileCount() == 0 ? "No map yet - tap MAPS to download one" : "Loading map...");
        return;
    }

    // Boat position in map coordinates; fall back to map centre with no fix.
    double blat = v.lat, blon = v.lon;
    if (!v.haveFix && !review) mapdata::centerLatLon(blat, blon);
    double olat = v.followBoat ? blat : v.centerLat;
    double olon = v.followBoat ? blon : v.centerLon;
    // Without a map file, project around the view centre instead of the map's origin.
    auto project = [&](double lat, double lon, int32_t& x, int32_t& y) {
        if (haveMap) {
            mapdata::project(lat, lon, x, y);
        } else {
            x = (int32_t)lround((lon - olon) * 111320.0 * cos(olat * M_PI / 180.0) * 10.0);
            y = (int32_t)lround((lat - olat) * 111320.0 * 10.0);
        }
    };
    int32_t oxi, oyi, bxi, byi;
    project(olat, olon, oxi, oyi);
    project(blat, blon, bxi, byi);

    Xform xf;
    xf.bx = oxi;
    xf.by = oyi;
    float B = rotationDeg(v);
    float a = B * (float)M_PI / 180.0f;
    xf.cosB = cosf(a);
    xf.sinB = sinf(a);
    xf.invmpp = 1.0f / v.metersPerPixel;
    xf.cx = c.width() / 2.0f;
    xf.cy = v.orientation == NorthUp ? c.height() / 2.0f : c.height() * 0.62f;

    // World (decimetre) bounding box of the canvas, for culling.
    float minVX = 1e18f, minVY = 1e18f, maxVX = -1e18f, maxVY = -1e18f;
    const float corners[4][2] = {{0, 0}, {(float)c.width(), 0}, {0, (float)c.height()},
                                 {(float)c.width(), (float)c.height()}};
    for (auto& cn : corners) {
        float xr = (cn[0] - xf.cx) * v.metersPerPixel;
        float yr = (xf.cy - cn[1]) * v.metersPerPixel;
        float dx = xr * xf.cosB + yr * xf.sinB;   // inverse rotation
        float dy = -xr * xf.sinB + yr * xf.cosB;
        float wx = xf.bx + dx * 10.0f;
        float wy = xf.by + dy * 10.0f;
        minVX = fminf(minVX, wx);
        maxVX = fmaxf(maxVX, wx);
        minVY = fminf(minVY, wy);
        maxVY = fmaxf(maxVY, wy);
    }

    // Draw by kind so areas sit under lines and markers.
    const uint8_t order[] = {mapdata::WaterArea, mapdata::WaterLine, mapdata::Pier,
                             mapdata::Weir,      mapdata::Bridge,    mapdata::Lock};
    for (uint8_t kind : order) {
        for (int i = 0; haveMap && i < m.featureCount; i++) {
            const mapdata::Feature& ft = m.features[i];
            if (ft.kind != kind) continue;
            if (!boxOverlap(ft.bbox, minVX, minVY, maxVX, maxVY)) continue;
            int nr = transformFeature(m, ft, xf);
            if (nr <= 0) continue;
            if (ft.flags & mapdata::F_POINT) continue;
            if (ft.flags & mapdata::F_LINE) {
                float wpx;
                switch (kind) {
                    case mapdata::WaterLine: wpx = (ft.widthDm * 0.1f) * xf.invmpp; break;
                    case mapdata::Bridge: wpx = 5; break;
                    case mapdata::Weir: wpx = 4; break;
                    case mapdata::Lock: wpx = 4; break;
                    default: wpx = 2; break;
                }
                drawLineFeature(c, nr, wpx, featureColor(kind));
            } else {
                fillRings(c, nr, featureColor(kind));
            }
        }
    }

    // Recorded track in speed colours, with start and finish markers.
    if (review) {
        float px = 0, py = 0;
        for (int i = 0; i < v.trackCount; i++) {
            int32_t tx, ty;
            project(v.trackLat[i], v.trackLon[i], tx, ty);
            float sx, sy;
            xf.toScreen(tx, ty, sx, sy);
            if (i && fabsf(sx - px) + fabsf(sy - py) < 2 && i < v.trackCount - 1) continue;
            if (i) c.drawWideLine(px, py, sx, sy, 4, v.trackColor[i]);
            px = sx;
            py = sy;
        }
        float sx, sy;
        int32_t tx, ty;
        project(v.trackLat[0], v.trackLon[0], tx, ty);
        xf.toScreen(tx, ty, sx, sy);
        c.fillCircle(sx, sy, 8, TFT_WHITE);
        c.fillCircle(sx, sy, 6, TFT_DARKGREEN);
        c.fillRect(px - 6, py - 6, 13, 13, TFT_WHITE);
        c.fillRect(px - 4, py - 4, 9, 9, TFT_BLACK);
    }

    // Track breadcrumb.
    int tn = review ? 0 : track::count();
    if (tn > 1) {
        float px = 0, py = 0;
        bool have = false;
        for (int i = 0; i < tn; i++) {
            track::Point tp = track::get(i);
            int32_t tx, ty;
            mapdata::project(tp.lat, tp.lon, tx, ty);
            float sx, sy;
            xf.toScreen(tx, ty, sx, sy);
            if (have) c.drawWideLine(px, py, sx, sy, 3, COL_TRACK);
            px = sx;
            py = sy;
            have = true;
        }
    }

    // Boat club markers and names (only when zoomed in enough to be useful).
    bool labels = v.metersPerPixel < 1.2f;
    for (int i = 0; haveMap && i < m.featureCount; i++) {
        const mapdata::Feature& ft = m.features[i];
        if (ft.kind != mapdata::Club || !(ft.flags & mapdata::F_POINT)) continue;
        if (!boxOverlap(ft.bbox, minVX, minVY, maxVX, maxVY)) continue;
        const mapdata::Ring& ring = m.rings[ft.ringFirst];
        const int32_t* pts = &m.points[ring.first * 2];
        float sx, sy;
        xf.toScreen(pts[0], pts[1], sx, sy);
        c.fillCircle(sx, sy, 5, COL_CLUB);
        c.drawCircle(sx, sy, 5, COL_TEXT);
        if (labels && ft.name[0]) {
            c.setFont(&fonts::FreeSans9pt7b);
            c.setTextSize(1);
            c.setTextDatum(middle_left);
            c.setTextColor(COL_TEXT);
            c.drawString(ft.name, sx + 9, sy);
        }
    }

    if (!review) {
        float bsx, bsy;
        xf.toScreen(bxi, byi, bsx, bsy);
        drawBoat(c, bsx, bsy, v.course, B);
    }
    drawScaleBar(c, v.metersPerPixel);
    if (v.orientation != NorthUp) drawNorthArrow(c, xf);

    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextSize(1);
    c.setTextDatum(bottom_right);
    c.setTextColor(COL_TEXT);
    c.drawString("(c) OpenStreetMap", c.width() - 6, c.height() - 4);

    if (!v.haveFix && !review) {
        c.setFont(&fonts::FreeSansBold12pt7b);
        c.setTextDatum(top_center);
        c.setTextColor(COL_LOCK);
        c.drawString("Waiting for GPS", c.width() / 2, 8);
    }
    if (v.showControls) drawControls(c, v);
}

}  // namespace mapview
