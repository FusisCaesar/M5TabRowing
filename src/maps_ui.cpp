#include "maps_ui.h"

#include <M5Unified.h>
#include <math.h>

#include <algorithm>

#include "display_fb.h"
#include "icons.h"
#include "gps_nav.h"
#include "keyboard.h"
#include "map_data.h"
#include "map_job.h"
#include "map_library.h"
#include "net.h"
#include "tiles.h"

namespace mapsui {
namespace {

constexpr int HEADER_H = 52;
// Rows as on the Settings screen: 90 px, with right-aligned 74 px icon buttons.
constexpr int ROW_H = 90;
constexpr int BTN_W = 74, BTN2_X = 1280 - 20 - BTN_W, BTN1_X = BTN2_X - 16 - BTN_W;
constexpr int MIN_ZOOM = 3, MAX_ZOOM = 17;
constexpr uint32_t FRAME_MS = 100;

int X0, Y0, W, H;
M5Canvas cv(&M5.Display);
Colors c;
View cur = View::Library;
bool dirty = true;
uint32_t lastDraw = 0;
uint32_t lastSig = 0;

char msg[80] = "";
uint32_t msgUntil = 0;
bool showRequested = false;

// Lists
int scroll = 0, scrollStart = 0;
int confirmIdx = -1;
uint32_t confirmUntil = 0;

// Renaming a map with the on-screen keyboard
bool editing = false;
int editTarget = 0;  // map index

// Area picker
double cLat = 52.2, cLon = 5.3;
int zoom = 8;
bool positioned = false;
bool selectMode = false;
bool haveSel = false;
double selS, selW, selN, selE;
double dragLat0, dragLon0;      // select: first corner
double dragCx0, dragCy0;        // pan: world centre at drag start
int dragX0, dragY0;
enum class Drag { None, Pan, Select, Scroll } drag = Drag::None;

struct Rect {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

int bodyH() { return H - HEADER_H; }

// ---- Web Mercator ----------------------------------------------------------

double worldSize(int z) { return 256.0 * (double)(1 << z); }
double worldX(double lon, int z) { return (lon + 180.0) / 360.0 * worldSize(z); }
double worldY(double lat, int z) {
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * worldSize(z);
}
double lonOf(double wx, int z) { return wx / worldSize(z) * 360.0 - 180.0; }
double latOf(double wy, int z) {
    double n = M_PI - 2.0 * M_PI * wy / worldSize(z);
    return 180.0 / M_PI * atan(sinh(n));
}

// Canvas position of lat/lon in the area picker.
void toScreen(double lat, double lon, int& sx, int& sy) {
    sx = (int)lround(worldX(lon, zoom) - worldX(cLon, zoom) + W / 2.0);
    sy = (int)lround(worldY(lat, zoom) - worldY(cLat, zoom) + HEADER_H + bodyH() / 2.0);
}
void toLatLon(int sx, int sy, double& lat, double& lon) {
    lon = lonOf(worldX(cLon, zoom) + sx - W / 2.0, zoom);
    lat = latOf(worldY(cLat, zoom) + sy - HEADER_H - bodyH() / 2.0, zoom);
}

void areaKm(double s, double w, double n, double e, double& wKm, double& hKm) {
    hKm = (n - s) * 111.32;
    wKm = (e - w) * 111.32 * cos((s + n) / 2.0 * M_PI / 180.0);
}

bool selValid() {
    double wKm, hKm;
    areaKm(selS, selW, selN, selE, wKm, hKm);
    return haveSel && wKm <= mapjob::MAX_EDGE_KM && hKm <= mapjob::MAX_EDGE_KM && wKm >= 0.2 && hKm >= 0.2;
}

// ---- widgets ---------------------------------------------------------------

void button(const Rect& r, const char* label, bool enabled, bool lit = false, uint16_t fill = 0) {
    uint16_t bg = lit ? c.accent : fill ? fill : enabled ? c.btn : c.bg;
    int rad = r.h >= 60 ? 16 : 12;
    cv.fillRoundRect(r.x, r.y, r.w, r.h, rad, bg);
    cv.drawRoundRect(r.x, r.y, r.w, r.h, rad, c.dim);
    cv.setTextSize(1);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    if (r.h < 60 || cv.textWidth(label) > r.w - 12) cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextDatum(middle_center);
    cv.setTextColor(lit || fill ? c.bg : enabled ? c.btnText : c.dim);
    cv.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
}

void iconButton(const Rect& r, icons::Icon icon, bool enabled = true, uint16_t fill = 0) {
    uint16_t bg = fill ? fill : enabled ? c.btn : c.bg;
    cv.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 16, bg);
    if (!enabled && !fill) cv.drawRoundRect(r.x, r.y, r.w, r.h, 16, c.dim);
    icons::draw(cv, icon, r.x + r.w / 2, r.y + r.h / 2, 36, fill ? c.bg : enabled ? c.btnText : c.dim, bg);
}

// Name on the first line of a list row, details below it.
void rowText(int y, const char* name, const char* details, uint16_t detailColor) {
    cv.setTextDatum(middle_left);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    cv.setTextColor(c.fg);
    cv.drawString(name, 24, y + 30);
    cv.setFont(&fonts::FreeSans12pt7b);
    cv.setTextColor(detailColor);
    cv.drawString(details, 24, y + 66);
}

void fmtBytes(char* out, size_t n, uint32_t b) {
    if (b >= 1024 * 1024) snprintf(out, n, "%.1f MB", b / 1048576.0);
    else snprintf(out, n, "%u KB", (unsigned)((b + 512) / 1024));
}

bool onCard(const char* path, int* index = nullptr) {
    mapdata::FileInfo fi;
    for (int i = 0; mapdata::fileInfo(i, fi); i++) {
        if (strcmp(fi.path, path) == 0) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

void startJob(const char* name, double s, double w, double n, double e) {
    char why[64];
    if (!mapjob::start(name, s, w, n, e, why, sizeof(why))) message(why);
    dirty = true;
}

// ---- header ----------------------------------------------------------------

void drawHeader() {
    cv.fillRect(0, 0, W, HEADER_H, c.bg);
    cv.drawFastHLine(0, HEADER_H - 1, W, c.dim);
    cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextSize(1);
    cv.setTextDatum(middle_left);
    cv.setTextColor(c.label);
    const char* title = net::setupActive()      ? "WI-FI SETUP"
                        : editing              ? "RENAME MAP"
                        : cur == View::Library ? "MAP LIBRARY"
                        : cur == View::Draw    ? "DRAW AN AREA"
                                               : "MAPS ON CARD";
    cv.drawString(title, 20, HEADER_H / 2);

    char mid[96] = "";
    uint16_t midColor = c.fg;
    mapjob::Status js = mapjob::status();
    unsigned secs = js.elapsedMs / 1000;
    char size[16];
    fmtBytes(size, sizeof(size), js.bytes);
    if (millis() < msgUntil) {
        snprintf(mid, sizeof(mid), "%s", msg);
        midColor = c.warn;
    } else {
        switch (js.phase) {
            case mapjob::Phase::Waiting:
                snprintf(mid, sizeof(mid), "%s: map server preparing data  %u s", js.name, secs);
                midColor = c.accent;
                break;
            case mapjob::Phase::Downloading:
                snprintf(mid, sizeof(mid), "%s: downloading %s", js.name, size);
                midColor = c.accent;
                break;
            case mapjob::Phase::Converting:
                snprintf(mid, sizeof(mid), "%s: converting...", js.name);
                midColor = c.accent;
                break;
            case mapjob::Phase::Done:
                snprintf(mid, sizeof(mid), "%s ready (%u s)", js.name, secs);
                midColor = c.ok;
                break;
            case mapjob::Phase::Failed:
                snprintf(mid, sizeof(mid), "%s failed: %s", js.name, js.msg);
                midColor = c.warn;
                break;
            case mapjob::Phase::Idle: break;
        }
    }
    cv.setTextDatum(middle_center);
    cv.setTextColor(midColor);
    cv.drawString(mid, W / 2 + 40, HEADER_H / 2);

    char right[48];
    switch (net::state()) {
        case net::State::Connected: snprintf(right, sizeof(right), "Wi-Fi: %s", net::ssid()); break;
        case net::State::Connecting: snprintf(right, sizeof(right), "Wi-Fi connecting..."); break;
        case net::State::Failed: snprintf(right, sizeof(right), "Wi-Fi not connected"); break;
        case net::State::Off: snprintf(right, sizeof(right), "Wi-Fi off"); break;
    }
    cv.setTextDatum(middle_right);
    cv.setTextColor(net::connected() ? c.ok : c.label);
    cv.drawString(right, W - 20, HEADER_H / 2);
}

// ---- Wi-Fi setup -----------------------------------------------------------

void drawSetup() {
    cv.fillRect(0, HEADER_H, W, bodyH(), c.bg);
    char qr[64];
    snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", net::setupName());
    int q = 300;
    cv.fillRect(40, HEADER_H + 40, q + 40, q + 40, TFT_WHITE);
    cv.qrcode(qr, 60, HEADER_H + 60, q, 3);

    int x = 440, y = HEADER_H + 60;
    cv.setTextDatum(top_left);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    cv.setTextColor(c.fg);
    cv.drawString("Set up Wi-Fi from your phone", x, y);
    cv.setFont(&fonts::FreeSansBold12pt7b);
    y += 70;
    char line[96];
    snprintf(line, sizeof(line), "1. Scan the code, or join the Wi-Fi network  %s", net::setupName());
    cv.drawString(line, x, y);
    y += 50;
    cv.drawString("2. A page opens. If not, browse to  http://192.168.4.1", x, y);
    y += 50;
    cv.drawString("3. Pick your home network (2.4 GHz) and enter its password", x, y);
    y += 70;
    cv.setTextColor(net::connected() ? c.ok : net::state() == net::State::Failed ? c.warn : c.label);
    if (net::setupClosing()) snprintf(line, sizeof(line), "Connected! This setup network closes in a moment.");
    else if (net::connected()) snprintf(line, sizeof(line), "Now using %s. Pick another network to switch.", net::ssid());
    else if (net::state() == net::State::Connecting) snprintf(line, sizeof(line), "Connecting...");
    else if (net::state() == net::State::Failed) snprintf(line, sizeof(line), "Not connected yet (wrong password?)");
    else snprintf(line, sizeof(line), "Waiting for your phone...");
    cv.drawString(line, x, y);
    y += 60;
    cv.setFont(&fonts::FreeSans12pt7b);
    cv.setTextColor(c.label);
    cv.drawString("Tap CANCEL SETUP to close this network.", x, y);
}

// ---- library ---------------------------------------------------------------

int maxScroll(int rows) { return std::max(0, rows * ROW_H - bodyH()); }

Rect rowButton(int row) { return {BTN2_X, HEADER_H + row * ROW_H - scroll + (ROW_H - BTN_W) / 2, BTN_W, BTN_W}; }

void drawLibrary() {
    cv.fillRect(0, HEADER_H, W, bodyH(), c.bg);
    cv.setClipRect(0, HEADER_H, W, bodyH());
    mapjob::Status js = mapjob::status();
    bool jobBusy = mapjob::busy();
    for (int i = 0; i < maplib::COUNT; i++) {
        int y = HEADER_H + i * ROW_H - scroll;
        if (y + ROW_H < HEADER_H || y > H) continue;
        const maplib::Entry& e = maplib::ENTRIES[i];
        cv.drawFastHLine(0, y + ROW_H - 1, W, c.dim);
        char path[48];
        mapjob::pathFor(e.name, path, sizeof(path));
        bool have = onCard(path);
        bool thisJob = jobBusy && strcmp(js.name, e.name) == 0;
        char details[96];
        snprintf(details, sizeof(details), "%s   %.0f x %.0f km%s", e.region, e.radiusKm * 2, e.radiusKm * 2,
                 thisJob ? "   downloading..." : have ? "   on card" : "");
        rowText(y, e.name, details, have || thisJob ? c.ok : c.label);
        if (thisJob) button(rowButton(i), "...", true, true);
        else iconButton(rowButton(i), have ? icons::Icon::Refresh : icons::Icon::Download, !jobBusy);
    }
    cv.clearClipRect();
    int ms = maxScroll(maplib::COUNT);
    if (ms > 0) {
        int bh = bodyH() * bodyH() / (maplib::COUNT * ROW_H);
        int by = HEADER_H + (bodyH() - bh) * scroll / ms;
        cv.fillRect(W - 6, by, 4, bh, c.dim);
    }
}

void tapLibrary(int x, int y) {
    for (int i = 0; i < maplib::COUNT; i++) {
        if (!rowButton(i).hit(x, y) || y < HEADER_H) continue;
        const maplib::Entry& e = maplib::ENTRIES[i];
        double s, w, n, east;
        maplib::bounds(e, s, w, n, east);
        startJob(e.name, s, w, n, east);
        return;
    }
}

// ---- maps on card ----------------------------------------------------------

int cardRows() { return std::max(1, mapdata::fileCount()); }

Rect deleteBtn(int row) { return {BTN2_X, HEADER_H + row * ROW_H - scroll + (ROW_H - BTN_W) / 2, BTN_W, BTN_W}; }
Rect renameBtn(int row) { return {BTN1_X, HEADER_H + row * ROW_H - scroll + (ROW_H - BTN_W) / 2, BTN_W, BTN_W}; }

void drawOnCard() {
    cv.fillRect(0, HEADER_H, W, bodyH(), c.bg);
    if (confirmIdx >= 0 && millis() > confirmUntil) confirmIdx = -1;
    cv.setClipRect(0, HEADER_H, W, bodyH());
    if (!mapdata::fileCount()) {
        cv.setFont(&fonts::FreeSans12pt7b);
        cv.setTextColor(c.label);
        cv.setTextDatum(middle_left);
        cv.drawString("No maps yet. Download one from LIBRARY, or draw your own area with DRAW AREA.", 24,
                      HEADER_H + ROW_H / 2);
    }
    for (int r = 0; r < mapdata::fileCount(); r++) {
        int y = HEADER_H + r * ROW_H - scroll;
        if (y + ROW_H < HEADER_H || y > H) continue;
        mapdata::FileInfo fi;
        if (!mapdata::fileInfo(r, fi)) continue;
        char a[96];
        char size[16];
        double wKm, hKm;
        areaKm(fi.south, fi.west, fi.north, fi.east, wKm, hKm);
        fmtBytes(size, sizeof(size), fi.bytes);
        bool confirm = r == confirmIdx;
        if (confirm) snprintf(a, sizeof(a), "Tap the red button again to delete this map");
        else snprintf(a, sizeof(a), "%.1f x %.1f km   %s   %s", wKm, hKm, size,
                     fi.loaded ? "showing" : "tap to show");
        rowText(y, fi.name, a, confirm ? c.warn : fi.loaded ? c.ok : c.label);
        cv.drawFastHLine(0, y + ROW_H - 1, W, c.dim);
        iconButton(renameBtn(r), icons::Icon::Pencil);
        iconButton(deleteBtn(r), icons::Icon::Trash, true, confirm ? c.warn : 0);
    }
    cv.clearClipRect();
    int rows = cardRows();
    int ms = maxScroll(rows);
    if (ms > 0) {
        int bh = bodyH() * bodyH() / (rows * ROW_H);
        int by = HEADER_H + (bodyH() - bh) * scroll / ms;
        cv.fillRect(W - 6, by, 4, bh, c.dim);
    }
}

void tapKeyboard(int x, int y) {
    osk::Result r = osk::tap(x, y, W, HEADER_H);
    if (r == osk::Result::Save) {
        const char* name = osk::text();
        bool ok = name[0] && mapdata::renameFile(editTarget, name);
        message(ok ? "map renamed" : name[0] ? "could not rename the map" : "a map needs a name");
        editing = false;
    } else if (r == osk::Result::Cancel) {
        editing = false;
    }
    dirty = true;
}

void tapOnCard(int x, int y) {
    if (y < HEADER_H) return;
    for (int r = 0; r < mapdata::fileCount(); r++) {
        bool del = deleteBtn(r).hit(x, y), ren = renameBtn(r).hit(x, y);
        if (!del && !ren) continue;
        if (ren) {
            mapdata::FileInfo fi;
            if (mapdata::fileInfo(r, fi)) {
                editing = true;
                editTarget = r;
                osk::start(fi.name, 31);
                confirmIdx = -1;
            }
        } else if (confirmIdx == r && millis() < confirmUntil) {
            confirmIdx = -1;
            mapdata::removeFile(r);
            scroll = std::min(scroll, maxScroll(cardRows()));
        } else {
            confirmIdx = r;
            confirmUntil = millis() + 4000;
        }
        dirty = true;
        return;
    }
    int r = (y - HEADER_H + scroll) / ROW_H;
    if (r < 0 || r >= mapdata::fileCount()) return;
    if (!mapdata::showIndex(r)) {
        message("could not open that map");
        return;
    }
    showRequested = true;
    dirty = true;
}

// ---- area picker -----------------------------------------------------------

Rect selectBtn() { return {W - 250, HEADER_H + 12, 234, 60}; }
Rect downloadBtn() { return {16, H - 80, 330, 64}; }
Rect zoomInBtn() { return {W - 80, H - 160, 64, 64}; }
Rect zoomOutBtn() { return {W - 80, H - 84, 64, 64}; }

void positionPicker() {
    if (positioned) return;
    positioned = true;
    gps::Snapshot g = gps::get();
    double lat, lon;
    if (g.fix) {
        cLat = g.lat, cLon = g.lon, zoom = 13;
    } else if (mapdata::centerLatLon(lat, lon)) {
        cLat = lat, cLon = lon, zoom = 13;
    }
}

void drawTiles() {
    int bh = bodyH();
    double cx = worldX(cLon, zoom), cy = worldY(cLat, zoom);
    double left = cx - W / 2.0, top = cy - bh / 2.0;
    int tx0 = (int)floor(left / 256), tx1 = (int)floor((left + W) / 256);
    int ty0 = (int)floor(top / 256), ty1 = (int)floor((top + bh) / 256);

    struct T {
        int tx, ty;
        double d;
    } list[64];
    int n = 0;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1 && n < 64; tx++) {
            double dx = tx * 256 + 128 - cx, dy = ty * 256 + 128 - cy;
            list[n++] = {tx, ty, dx * dx + dy * dy};
        }
    // Farthest first: the tile fetcher takes the newest request, so the centre loads first.
    std::sort(list, list + n, [](const T& a, const T& b) { return a.d > b.d; });

    cv.setClipRect(0, HEADER_H, W, bh);
    cv.fillRect(0, HEADER_H, W, bh, 0xC618);
    tiles::beginFrame();
    tiles::lock();
    for (int i = 0; i < n; i++) {
        int sx = (int)lround(list[i].tx * 256 - left), sy = (int)lround(list[i].ty * 256 - top) + HEADER_H;
        const uint16_t* px = net::connected() ? tiles::get(zoom, list[i].tx, list[i].ty) : nullptr;
        if (px) cv.pushImage(sx, sy, tiles::SIZE, tiles::SIZE, (const lgfx::swap565_t*)px);
        else cv.drawRect(sx, sy, tiles::SIZE, tiles::SIZE, 0xBDF7);
    }
    tiles::unlock();
}

void drawPicker() {
    positionPicker();
    drawTiles();

    // Maps already on the card.
    cv.setFont(&fonts::FreeSansBold9pt7b);
    cv.setTextDatum(top_left);
    mapdata::FileInfo fi;
    for (int i = 0; mapdata::fileInfo(i, fi); i++) {
        int x0, y0, x1, y1;
        toScreen(fi.north, fi.west, x0, y0);
        toScreen(fi.south, fi.east, x1, y1);
        for (int k = 0; k < 2; k++) cv.drawRect(x0 + k, y0 + k, x1 - x0 - 2 * k, y1 - y0 - 2 * k, 0x0400);
        int lx = std::max(x0, 0) + 6, ly = std::max(y0, HEADER_H) + 6 + i * 22;
        int tw = cv.textWidth(fi.name) + 8;
        cv.fillRect(lx - 4, ly - 2, tw, 20, TFT_WHITE);
        cv.setTextColor(0x0400);
        cv.drawString(fi.name, lx, ly);
    }

    // Position.
    gps::Snapshot g = gps::get();
    if (g.fix) {
        int px, py;
        toScreen(g.lat, g.lon, px, py);
        cv.fillCircle(px, py, 11, TFT_BLACK);
        cv.fillCircle(px, py, 8, TFT_YELLOW);
    }

    // Selection.
    if (haveSel) {
        int x0, y0, x1, y1;
        toScreen(selN, selW, x0, y0);
        toScreen(selS, selE, x1, y1);
        bool ok = selValid();
        uint16_t col = ok ? 0x001F : TFT_RED;
        for (int k = 0; k < 4; k++) cv.drawRect(x0 - k, y0 - k, x1 - x0 + 2 * k, y1 - y0 + 2 * k, col);
        double wKm, hKm;
        areaKm(selS, selW, selN, selE, wKm, hKm);
        char label[48];
        if (ok) snprintf(label, sizeof(label), "%.1f x %.1f km", wKm, hKm);
        else snprintf(label, sizeof(label), "%.1f x %.1f km - max %.0f km", wKm, hKm, mapjob::MAX_EDGE_KM);
        cv.setFont(&fonts::FreeSansBold12pt7b);
        int tw = cv.textWidth(label) + 16;
        int ly = std::max(y0 - 36, HEADER_H + 4);
        cv.fillRoundRect(x0, ly, tw, 30, 6, col);
        cv.setTextColor(TFT_WHITE);
        cv.setTextDatum(middle_left);
        cv.drawString(label, x0 + 8, ly + 15);
    }
    cv.clearClipRect();

    // Controls.
    button(selectBtn(), selectMode ? "DRAG TO DRAW" : haveSel ? "NEW AREA" : "SELECT AREA", true, selectMode);
    if (haveSel) button(downloadBtn(), "DOWNLOAD AREA", selValid() && !mapjob::busy());
    cv.fillRoundRect(zoomInBtn().x, zoomInBtn().y, 64, 64, 10, TFT_WHITE);
    cv.drawRoundRect(zoomInBtn().x, zoomInBtn().y, 64, 64, 10, TFT_DARKGREY);
    cv.fillRoundRect(zoomOutBtn().x, zoomOutBtn().y, 64, 64, 10, TFT_WHITE);
    cv.drawRoundRect(zoomOutBtn().x, zoomOutBtn().y, 64, 64, 10, TFT_DARKGREY);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    cv.setTextColor(TFT_BLACK);
    cv.setTextDatum(middle_center);
    cv.drawString("+", zoomInBtn().x + 32, zoomInBtn().y + 32);
    cv.drawString("-", zoomOutBtn().x + 32, zoomOutBtn().y + 30);

    cv.setFont(&fonts::FreeSans9pt7b);
    const char* attr = "(c) OpenStreetMap contributors";
    int aw = cv.textWidth(attr) + 12;
    cv.fillRect(W - 96 - aw, H - 24, aw, 24, TFT_WHITE);
    cv.setTextColor(TFT_BLACK);
    cv.setTextDatum(middle_right);
    cv.drawString(attr, W - 102, H - 12);

    if (!net::connected()) {
        cv.setFont(&fonts::FreeSansBold12pt7b);
        const char* t = "Connect Wi-Fi (WIFI button) to see the map";
        int tw = cv.textWidth(t) + 40;
        cv.fillRoundRect(W / 2 - tw / 2, HEADER_H + bodyH() / 2 - 30, tw, 60, 10, TFT_WHITE);
        cv.setTextColor(TFT_BLACK);
        cv.setTextDatum(middle_center);
        cv.drawString(t, W / 2, HEADER_H + bodyH() / 2);
    } else if (selectMode) {
        cv.setFont(&fonts::FreeSansBold12pt7b);
        const char* t = "Drag a rectangle over the water you row on";
        int tw = cv.textWidth(t) + 30;
        cv.fillRoundRect(16, HEADER_H + 16, tw, 44, 8, TFT_WHITE);
        cv.setTextColor(TFT_BLACK);
        cv.setTextDatum(middle_left);
        cv.drawString(t, 31, HEADER_H + 38);
    }
}

void setZoom(int z) {
    zoom = std::max(MIN_ZOOM, std::min(MAX_ZOOM, z));
    dirty = true;
}

void tapPicker(int x, int y) {
    if (selectBtn().hit(x, y)) {
        selectMode = !selectMode;
        if (selectMode) haveSel = false;
    } else if (haveSel && downloadBtn().hit(x, y)) {
        if (!selValid()) {
            message("area too large or too small");
        } else {
            char name[32];
            snprintf(name, sizeof(name), "Area %.3f %.3f", (selS + selN) / 2, (selW + selE) / 2);
            startJob(name, selS, selW, selN, selE);
        }
    } else if (zoomInBtn().hit(x, y)) {
        setZoom(zoom + 1);
    } else if (zoomOutBtn().hit(x, y)) {
        setZoom(zoom - 1);
    }
    dirty = true;
}

uint32_t signature() {
    mapjob::Status js = mapjob::status();
    uint32_t s = (uint32_t)cur * 2654435761u;
    s ^= (uint32_t)net::state() << 3 ^ (uint32_t)net::setupActive() << 6 ^ (uint32_t)js.phase << 8;
    s ^= js.bytes / 65536 * 7919 ^ js.elapsedMs / 1000 * 104729;
    s ^= mapdata::fileCount() * 31337 ^ (millis() < msgUntil) << 20 ^ (confirmIdx + 1) << 22;
    s ^= (uint32_t)editing << 28;
    if (cur == View::Draw) s ^= tiles::version() * 15485863u ^ (uint32_t)gps::get().fix << 24;
    return s;
}

}  // namespace

void begin(int x, int y, int w, int h) {
    X0 = x, Y0 = y, W = w, H = h;
    cv.setColorDepth(16);
    cv.setPsram(true);
    cv.createSprite(W, H);
    tiles::begin();
}

void setColors(const Colors& colors) {
    c = colors;
    dirty = true;
}

void enter() {
    dirty = true;
    scroll = 0;
    confirmIdx = -1;
    editing = false;
    drag = Drag::None;
}

void leave() {
    selectMode = false;
    editing = false;
    tiles::clear();
}

View view() { return cur; }

void setView(View v) {
    if (editing) {
        editing = false;
        dirty = true;
    }
    if (v == cur) return;
    cur = v;
    scroll = 0;
    confirmIdx = -1;
    selectMode = false;
    if (v != View::Draw) tiles::clear();
    dirty = true;
}

bool takeShown() {
    bool v = showRequested;
    showRequested = false;
    return v;
}

void message(const char* text) {
    strlcpy(msg, text, sizeof(msg));
    msgUntil = millis() + 4000;
    dirty = true;
}

void draw(bool force) {
    uint32_t sig = signature();
    if (!force && !dirty && sig == lastSig) return;
    if (!force && millis() - lastDraw < FRAME_MS) return;
    lastDraw = millis();
    lastSig = sig;
    dirty = false;

    drawHeader();
    if (net::setupActive()) drawSetup();
    else if (editing) osk::draw(cv, HEADER_H, c);
    else if (cur == View::Library) drawLibrary();
    else if (cur == View::Draw) drawPicker();
    else drawOnCard();
    fb::push(cv, X0, Y0);
}

void tap(int x, int y) {
    x -= X0, y -= Y0;
    if (net::setupActive()) return;
    if (editing) tapKeyboard(x, y);
    else if (cur == View::Library) tapLibrary(x, y);
    else if (cur == View::Draw) tapPicker(x, y);
    else tapOnCard(x, y);
}

void dragStart(int x, int y) {
    x -= X0, y -= Y0;
    dragX0 = x, dragY0 = y;
    if (net::setupActive() || editing || y < HEADER_H) {
        drag = Drag::None;
    } else if (cur != View::Draw) {
        drag = Drag::Scroll;
        scrollStart = scroll;
    } else if (selectMode) {
        drag = Drag::Select;
        toLatLon(x, y, dragLat0, dragLon0);
        haveSel = false;
    } else {
        drag = Drag::Pan;
        dragCx0 = worldX(cLon, zoom);
        dragCy0 = worldY(cLat, zoom);
    }
}

void dragMove(int x, int y) {
    x -= X0, y -= Y0;
    switch (drag) {
        case Drag::Scroll: {
            int rows = cur == View::Library ? maplib::COUNT : cardRows();
            scroll = std::max(0, std::min(maxScroll(rows), scrollStart - (y - dragY0)));
            break;
        }
        case Drag::Pan:
            cLon = lonOf(dragCx0 - (x - dragX0), zoom);
            cLat = std::max(-85.0, std::min(85.0, latOf(dragCy0 - (y - dragY0), zoom)));
            break;
        case Drag::Select: {
            double lat, lon;
            toLatLon(x, std::max(y, HEADER_H), lat, lon);
            selS = std::min(lat, dragLat0), selN = std::max(lat, dragLat0);
            selW = std::min(lon, dragLon0), selE = std::max(lon, dragLon0);
            haveSel = true;
            break;
        }
        case Drag::None: return;
    }
    dirty = true;
}

void dragEnd(int x, int y) {
    dragMove(x, y);
    if (drag == Drag::Select && haveSel) selectMode = false;
    drag = Drag::None;
    dirty = true;
}

}  // namespace mapsui
