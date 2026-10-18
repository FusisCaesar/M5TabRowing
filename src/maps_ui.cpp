#include "maps_ui.h"

#include <M5Unified.h>
#include <math.h>

#include <algorithm>

#include "display_fb.h"
#include "gps_nav.h"
#include "map_data.h"
#include "map_job.h"
#include "map_library.h"
#include "map_view.h"
#include "net.h"
#include "recordings.h"
#include "review.h"
#include "tiles.h"

namespace mapsui {
namespace {

constexpr int HEADER_H = 52;
constexpr int ROW_H = 64;
constexpr int ROW_BTN_W = 196;
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

// Lists
int scroll = 0, scrollStart = 0;
int confirmIdx = -1;
uint32_t confirmUntil = 0;

// Renaming with the on-screen keyboard
enum class Edit { None, Map, Recording } edit = Edit::None;
int editTarget = 0;  // map index or recording number
char editText[40] = "";
int editMax = 0;
bool caps = true;

int reviewNumber = -1;  // recording shown in the session review, or -1

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
enum class Drag { None, Pan, Select, Scroll, ReviewPan } drag = Drag::None;

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
    cv.fillRoundRect(r.x, r.y, r.w, r.h, 12, bg);
    cv.drawRoundRect(r.x, r.y, r.w, r.h, 12, c.dim);
    cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextSize(1);
    cv.setTextDatum(middle_center);
    cv.setTextColor(lit || fill ? c.bg : enabled ? c.btnText : c.dim);
    cv.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
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
    const char* title = net::setupActive()           ? "WI-FI SETUP"
                        : edit == Edit::Map         ? "RENAME MAP"
                        : edit == Edit::Recording   ? "RENAME RECORDING"
                        : reviewNumber >= 0         ? "SESSION"
                        : cur == View::Library      ? "MAP LIBRARY"
                        : cur == View::Draw         ? "DRAW AN AREA"
                                                    : "ON CARD";
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

Rect rowButton(int row) { return {W - ROW_BTN_W - 20, HEADER_H + row * ROW_H - scroll + 8, ROW_BTN_W, ROW_H - 16}; }

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
        cv.setTextDatum(middle_left);
        cv.setFont(&fonts::FreeSansBold12pt7b);
        cv.setTextColor(c.fg);
        cv.drawString(e.name, 24, y + ROW_H / 2);
        cv.setFont(&fonts::FreeSans12pt7b);
        cv.setTextColor(c.label);
        cv.drawString(e.region, 430, y + ROW_H / 2);
        char size[24];
        snprintf(size, sizeof(size), "%.0f x %.0f km", e.radiusKm * 2, e.radiusKm * 2);
        cv.drawString(size, 760, y + ROW_H / 2);

        char path[48];
        mapjob::pathFor(e.name, path, sizeof(path));
        bool have = onCard(path);
        bool thisJob = jobBusy && strcmp(js.name, e.name) == 0;
        if (have) {
            cv.setTextColor(c.ok);
            cv.setTextDatum(middle_right);
            cv.drawString("on card", W - ROW_BTN_W - 40, y + ROW_H / 2);
        }
        button(rowButton(i), thisJob ? "..." : have ? "UPDATE" : "DOWNLOAD", !jobBusy, thisJob);
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

// ---- maps and recordings on card -------------------------------------------

// One scrolling list: a MAPS section, then a RECORDINGS section.
struct CardRow {
    enum Kind { MapsHeader, Map, NoMaps, RecHeader, Rec, NoRecs } kind;
    int idx;
};

int cardRows() {
    return 2 + std::max(1, mapdata::fileCount()) + std::max(1, recordings::count());
}

CardRow cardRow(int r) {
    int maps = mapdata::fileCount();
    int mapRows = std::max(1, maps);
    if (r == 0) return {CardRow::MapsHeader, 0};
    if (r <= mapRows) return maps ? CardRow{CardRow::Map, r - 1} : CardRow{CardRow::NoMaps, 0};
    if (r == mapRows + 1) return {CardRow::RecHeader, 0};
    int i = r - mapRows - 2;
    return recordings::count() ? CardRow{CardRow::Rec, i} : CardRow{CardRow::NoRecs, 0};
}

constexpr int CARD_BTN_W = 150;
Rect deleteBtn(int row) { return {W - CARD_BTN_W - 20, HEADER_H + row * ROW_H - scroll + 8, CARD_BTN_W, ROW_H - 16}; }
Rect renameBtn(int row) { return {W - 2 * CARD_BTN_W - 36, HEADER_H + row * ROW_H - scroll + 8, CARD_BTN_W, ROW_H - 16}; }

void fmtDuration(char* out, size_t n, float s) {
    int t = (int)s;
    if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

void drawOnCard() {
    cv.fillRect(0, HEADER_H, W, bodyH(), c.bg);
    if (confirmIdx >= 0 && millis() > confirmUntil) confirmIdx = -1;
    cv.setClipRect(0, HEADER_H, W, bodyH());
    int rows = cardRows();
    for (int r = 0; r < rows; r++) {
        int y = HEADER_H + r * ROW_H - scroll;
        if (y + ROW_H < HEADER_H || y > H) continue;
        int cy = y + ROW_H / 2;
        CardRow row = cardRow(r);
        cv.setTextDatum(middle_left);
        char a[64], b[64];
        switch (row.kind) {
            case CardRow::MapsHeader:
            case CardRow::RecHeader: {
                bool maps = row.kind == CardRow::MapsHeader;
                cv.fillRect(0, y, W, ROW_H, c.btn);
                cv.setFont(&fonts::FreeSansBold12pt7b);
                cv.setTextColor(c.btnText);
                snprintf(a, sizeof(a), "%s (%d)", maps ? "MAPS" : "RECORDINGS",
                         maps ? mapdata::fileCount() : recordings::count());
                cv.drawString(a, 24, cy);
                cv.setFont(&fonts::FreeSans12pt7b);
                cv.drawString(maps ? "the map covering your position is shown automatically"
                                   : "copy with a card reader or: python tools\\fetch.py COM10",
                              300, cy);
                continue;
            }
            case CardRow::NoMaps:
            case CardRow::NoRecs:
                cv.setFont(&fonts::FreeSans12pt7b);
                cv.setTextColor(c.label);
                cv.drawString(row.kind == CardRow::NoMaps
                                  ? "No maps yet. Download one from LIBRARY, or draw your own area with DRAW AREA."
                                  : "No recordings yet. Press START on the main screen to record a session.",
                              24, cy);
                break;
            case CardRow::Map: {
                mapdata::FileInfo fi;
                if (!mapdata::fileInfo(row.idx, fi)) break;
                cv.setFont(&fonts::FreeSansBold12pt7b);
                cv.setTextColor(c.fg);
                cv.drawString(fi.name, 24, cy);
                cv.setFont(&fonts::FreeSans12pt7b);
                cv.setTextColor(c.label);
                char size[16];
                double wKm, hKm;
                areaKm(fi.south, fi.west, fi.north, fi.east, wKm, hKm);
                fmtBytes(size, sizeof(size), fi.bytes);
                snprintf(a, sizeof(a), "%.1f x %.1f km   %s", wKm, hKm, size);
                cv.drawString(a, 470, cy);
                if (fi.loaded) {
                    cv.setTextColor(c.ok);
                    cv.drawString("showing", 760, cy);
                }
                break;
            }
            case CardRow::Rec: {
                recordings::Info ri;
                if (!recordings::get(row.idx, ri)) break;
                cv.setFont(&fonts::FreeSansBold12pt7b);
                cv.setTextColor(c.fg);
                if (ri.label[0]) snprintf(a, sizeof(a), "%s", ri.label);
                else snprintf(a, sizeof(a), "Session %d", ri.number);
                cv.drawString(a, 24, cy);
                cv.setFont(&fonts::FreeSans12pt7b);
                cv.setTextColor(c.label);
                snprintf(b, sizeof(b), "S%04d  %s", ri.number, ri.start[0] ? ri.start : "");
                cv.drawString(b, 470, cy);
                char size[16], dur[16] = "";
                fmtBytes(size, sizeof(size), ri.bytes);
                if (ri.durationS > 0) fmtDuration(dur, sizeof(dur), ri.durationS);
                snprintf(b, sizeof(b), "%s  %s", dur, size);
                cv.setTextDatum(middle_right);
                cv.drawString(b, renameBtn(r).x - 16, cy);
                break;
            }
        }
        cv.drawFastHLine(0, y + ROW_H - 1, W, c.dim);
        if (row.kind == CardRow::Map || row.kind == CardRow::Rec) {
            bool confirm = r == confirmIdx;
            button(renameBtn(r), "RENAME", true);
            button(deleteBtn(r), confirm ? "CONFIRM" : "DELETE", true, false, confirm ? c.warn : 0);
        }
    }
    cv.clearClipRect();
    int ms = maxScroll(rows);
    if (ms > 0) {
        int bh = bodyH() * bodyH() / (rows * ROW_H);
        int by = HEADER_H + (bodyH() - bh) * scroll / ms;
        cv.fillRect(W - 6, by, 4, bh, c.dim);
    }
}

// ---- session review --------------------------------------------------------

constexpr int REV_MAP_W = 760;
M5Canvas revMap(&cv);
double revLat = 0, revLon = 0;
float revMpp = 2;
const char* revError = nullptr;

int revMapH() { return bodyH(); }

void fitReview() {
    double s, w, n, e;
    if (!review::trackBounds(s, w, n, e)) return;
    revLat = (s + n) / 2;
    revLon = (w + e) / 2;
    double wm = (e - w) * 111320.0 * cos(revLat * M_PI / 180.0), hm = (n - s) * 111320.0;
    revMpp = (float)std::max(0.5, std::max(wm / (REV_MAP_W - 80), hm / (revMapH() - 80)));
}

void openReview(int number) {
    revError = nullptr;
    reviewNumber = number;
    if (!review::load(number)) {
        revError = "This recording has no GPS log.";
    } else if (review::trackCount() < 2) {
        revError = "No GPS positions in this recording (no fix while it was recorded).";
    } else {
        fitReview();
        mapdata::update(revLat, revLon, true);  // the map file covering the track, if any
    }
    if (!revMap.getBuffer()) {
        revMap.setColorDepth(16);
        revMap.setPsram(true);
        revMap.createSprite(REV_MAP_W, revMapH());
    }
    dirty = true;
}

void closeReview() {
    reviewNumber = -1;
    review::unload();
    dirty = true;
}

void fmtSplit(char* out, size_t n, float s) {
    if (s <= 0) snprintf(out, n, "--");
    else snprintf(out, n, "%d:%04.1f", (int)s / 60, fmodf(s, 60));
}

// Time-series graph: x = session time, y = value; `invert` puts small values at the top (splits).
void drawGraph(int x, int y, int w, int h, const char* title, float lo, float hi, bool invert,
               float (*value)(int), uint16_t color, const char* loText, const char* hiText) {
    cv.drawRect(x, y, w, h, c.dim);
    int n = review::count();
    float t1 = n ? std::max(1.0f, review::elapsed(n - 1)) : 1;
    int px = -1, py = 0;
    for (int i = 0; i < n; i++) {
        float v = value(i);
        int sx = x + (int)(review::elapsed(i) / t1 * (w - 1));
        if (v <= 0) {
            px = -1;
            continue;
        }
        float f = std::max(0.0f, std::min(1.0f, (v - lo) / (hi - lo)));
        int sy = invert ? y + 2 + (int)(f * (h - 5)) : y + h - 3 - (int)(f * (h - 5));
        if (px >= 0 && sx - px < 30) cv.drawLine(px, py, sx, sy, color);
        px = sx;
        py = sy;
    }
    cv.setFont(&fonts::FreeSansBold9pt7b);
    cv.setTextColor(c.label);
    cv.setTextDatum(top_left);
    cv.drawString(title, x + 6, y + 4);
    cv.setFont(&fonts::FreeSans9pt7b);
    cv.setTextDatum(top_right);
    cv.drawString(invert ? loText : hiText, x + w - 6, y + 4);
    cv.setTextDatum(bottom_right);
    cv.drawString(invert ? hiText : loText, x + w - 6, y + h - 4);
}

float splitAt(int i) { return review::split(i); }
float rateAt(int i) { return review::rate(i); }

void drawReview() {
    cv.fillRect(0, HEADER_H, W, bodyH(), c.bg);
    const review::Summary& s = review::summary();
    int px = REV_MAP_W + 20;
    char a[48], b[48];

    if (revError) {
        cv.setFont(&fonts::FreeSansBold12pt7b);
        cv.setTextColor(c.label);
        cv.setTextDatum(middle_center);
        cv.drawString(revError, W / 2, HEADER_H + bodyH() / 2);
        return;
    }

    mapview::View v;
    v.lat = revLat;
    v.lon = revLon;
    v.haveFix = true;
    v.metersPerPixel = revMpp;
    v.trackLat = review::trackLat();
    v.trackLon = review::trackLon();
    v.trackColor = review::trackColor();
    v.trackCount = review::trackCount();
    mapview::render(revMap, v);
    revMap.pushSprite(0, HEADER_H);

    // Colour legend on the map.
    float slow, fast;
    review::speedScale(slow, fast);
    int lx = 12, ly = HEADER_H + 12;
    cv.fillRoundRect(lx, ly, 300, 46, 8, TFT_WHITE);
    for (int i = 0; i < 160; i++) {
        float t = i / 159.0f;
        uint8_t r = t < 0.5f ? 230 : (uint8_t)(230 * (1 - t) * 2);
        uint8_t g = t < 0.5f ? (uint8_t)(200 * t * 2) : 200;
        cv.drawFastVLine(lx + 70 + i, ly + 10, 14, lgfx::color565(r, g, 20));
    }
    cv.setFont(&fonts::FreeSans9pt7b);
    cv.setTextColor(TFT_BLACK);
    fmtSplit(a, sizeof(a), slow > 0 ? 500 / slow : 0);
    fmtSplit(b, sizeof(b), fast > 0 ? 500 / fast : 0);
    cv.setTextDatum(middle_left);
    cv.drawString("slow", lx + 10, ly + 17);
    cv.drawString("fast", lx + 236, ly + 17);
    cv.setTextDatum(top_center);
    cv.drawString(a, lx + 70, ly + 27);
    cv.drawString(b, lx + 230, ly + 27);

    // Title.
    cv.setTextDatum(top_left);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    cv.setTextColor(c.fg);
    if (s.label[0]) snprintf(a, sizeof(a), "%s", s.label);
    else snprintf(a, sizeof(a), "Session %d", s.number);
    cv.drawString(a, px, HEADER_H + 10);
    cv.setFont(&fonts::FreeSans12pt7b);
    cv.setTextColor(c.label);
    snprintf(b, sizeof(b), "S%04d   %s", s.number, s.start);
    cv.drawString(b, px, HEADER_H + 50);

    // Totals, two columns.
    char dist[16], dur[16], avg[16], best[16], rate[16], strokes[16], dps[16], vmax[16];
    snprintf(dist, sizeof(dist), "%.0f m", s.distanceM);
    fmtDuration(dur, sizeof(dur), s.durationS);
    fmtSplit(avg, sizeof(avg), s.avgSplitS);
    fmtSplit(best, sizeof(best), s.best500S);
    if (s.avgRate > 0) snprintf(rate, sizeof(rate), "%.1f", s.avgRate);
    else snprintf(rate, sizeof(rate), "--");
    snprintf(strokes, sizeof(strokes), "%d", s.strokes);
    if (s.avgDps > 0) snprintf(dps, sizeof(dps), "%.1f m", s.avgDps);
    else snprintf(dps, sizeof(dps), "--");
    snprintf(vmax, sizeof(vmax), "%.1f m/s", s.maxSpeed);
    const char* labels[] = {"DISTANCE", "TIME", "AVG SPLIT", "BEST 500 M", "AVG RATE", "STROKES", "DIST/STROKE",
                            "MAX SPEED"};
    const char* values[] = {dist, dur, avg, best, rate, strokes, dps, vmax};
    for (int i = 0; i < 8; i++) {
        int cx = px + (i % 2) * 250, cy = HEADER_H + 84 + (i / 2) * 50;
        cv.setFont(&fonts::FreeSans9pt7b);
        cv.setTextColor(c.label);
        cv.drawString(labels[i], cx, cy);
        cv.setFont(&fonts::FreeSansBold12pt7b);
        cv.setTextColor(c.fg);
        cv.drawString(values[i], cx, cy + 18);
    }

    int gw = W - px - 16, gy = HEADER_H + 288;
    drawGraph(px, gy, gw, 106, "SPLIT /500m", 100, 200, true, splitAt, c.accent, "1:40", "3:20");
    drawGraph(px, gy + 114, gw, 106, "RATE spm", 14, 40, false, rateAt, c.ok, "14", "40");
}

bool tapReview(int x, int y) {
    if (revError || x >= REV_MAP_W || y < HEADER_H) return false;
    int bx, by, bw, bh;
    int mx = x, my = y - HEADER_H;
    if (mapview::zoomInRect(revMap, bx, by, bw, bh) && mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
        revMpp = std::max(0.25f, revMpp / 1.6f);
    } else if (mapview::zoomOutRect(revMap, bx, by, bw, bh) && mx >= bx && mx < bx + bw && my >= by &&
               my < by + bh) {
        revMpp = std::min(50.0f, revMpp * 1.6f);
    } else {
        fitReview();
    }
    dirty = true;
    return true;
}

// ---- rename keyboard -------------------------------------------------------

constexpr char K_DEL = '\b', K_CAPS = 1, K_CANCEL = 2, K_SAVE = 3, K_CLEAR = 4;
constexpr int KEY_W = 112, KEY_H = 68, KEY_GAP = 8;
constexpr int FIELD_Y = HEADER_H + 14, FIELD_H = 64;

struct Key {
    Rect r;
    char code;
};

// Fills `keys`, returns the count. Five rows of ten key widths each.
int layoutKeys(Key* keys) {
    static const char* const ROWS[] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm"};
    int x0 = (W - (10 * KEY_W + 9 * KEY_GAP)) / 2;
    int n = 0;
    auto add = [&](int row, int col, int span, char code) {
        int y = FIELD_Y + FIELD_H + 16 + row * (KEY_H + KEY_GAP);
        keys[n++] = {{x0 + col * (KEY_W + KEY_GAP), y, span * KEY_W + (span - 1) * KEY_GAP, KEY_H}, code};
    };
    for (int r = 0; r < 4; r++)
        for (int k = 0; ROWS[r][k]; k++) add(r, k, 1, ROWS[r][k]);
    add(3, 7, 1, '.');
    add(3, 8, 2, K_DEL);
    add(4, 0, 2, K_CAPS);
    add(4, 2, 4, ' ');
    add(4, 6, 1, K_CLEAR);
    add(4, 7, 1, K_CANCEL);
    add(4, 8, 2, K_SAVE);
    return n;
}

void startEdit(Edit kind, int target, const char* current, int maxLen) {
    edit = kind;
    editTarget = target;
    editMax = maxLen;
    strlcpy(editText, current, std::min((int)sizeof(editText), maxLen + 1));
    caps = editText[0] == '\0';
    confirmIdx = -1;
    dirty = true;
}

void drawKeyboard() {
    cv.fillRect(0, HEADER_H, W, bodyH(), c.bg);
    Rect f{40, FIELD_Y, W - 80, FIELD_H};
    cv.fillRoundRect(f.x, f.y, f.w, f.h, 10, TFT_WHITE);
    cv.drawRoundRect(f.x, f.y, f.w, f.h, 10, c.accent);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    cv.setTextColor(TFT_BLACK);
    cv.setTextDatum(middle_left);
    cv.drawString(editText, f.x + 20, f.y + f.h / 2);
    int cx = f.x + 20 + cv.textWidth(editText) + 3;
    cv.fillRect(cx, f.y + 14, 3, f.h - 28, c.accent);
    cv.setFont(&fonts::FreeSans12pt7b);
    cv.setTextColor(TFT_DARKGREY);
    cv.setTextDatum(middle_right);
    char left[24];
    snprintf(left, sizeof(left), "%d / %d", (int)strlen(editText), editMax);
    cv.drawString(left, f.x + f.w - 20, f.y + f.h / 2);

    Key keys[48];
    int n = layoutKeys(keys);
    for (int i = 0; i < n; i++) {
        char label[8];
        char code = keys[i].code;
        switch (code) {
            case K_DEL: strcpy(label, "DELETE"); break;
            case K_CAPS: strcpy(label, caps ? "abc" : "ABC"); break;
            case K_CLEAR: strcpy(label, "CLR"); break;
            case K_CANCEL: strcpy(label, "BACK"); break;
            case K_SAVE: strcpy(label, "SAVE"); break;
            case ' ': strcpy(label, "SPACE"); break;
            default:
                label[0] = caps && isalpha((unsigned char)code) ? toupper((unsigned char)code) : code;
                label[1] = '\0';
        }
        button(keys[i].r, label, true, code == K_SAVE);
    }
}

void saveEdit() {
    bool ok = false;
    // Trim trailing spaces.
    for (int k = (int)strlen(editText) - 1; k >= 0 && editText[k] == ' '; k--) editText[k] = '\0';
    if (edit == Edit::Map) {
        ok = editText[0] && mapdata::renameFile(editTarget, editText);
        message(ok ? "map renamed" : editText[0] ? "could not rename the map" : "a map needs a name");
    } else if (edit == Edit::Recording) {
        ok = recordings::rename(editTarget, editText);
        message(ok ? "recording renamed" : "could not rename the recording");
    }
    edit = Edit::None;
}

void tapKeyboard(int x, int y) {
    Key keys[48];
    int n = layoutKeys(keys);
    for (int i = 0; i < n; i++) {
        if (!keys[i].r.hit(x, y)) continue;
        char code = keys[i].code;
        size_t len = strlen(editText);
        switch (code) {
            case K_DEL:
                if (len) editText[len - 1] = '\0';
                break;
            case K_CAPS: caps = !caps; break;
            case K_CLEAR: editText[0] = '\0'; caps = true; break;
            case K_CANCEL: edit = Edit::None; break;
            case K_SAVE: saveEdit(); break;
            default:
                if ((int)len < editMax && !(code == ' ' && (len == 0 || editText[len - 1] == ' '))) {
                    editText[len] = caps && isalpha((unsigned char)code) ? toupper((unsigned char)code) : code;
                    editText[len + 1] = '\0';
                    // Like a phone keyboard: capitals for the first letter only.
                    if (isalpha((unsigned char)code)) caps = false;
                }
        }
        dirty = true;
        return;
    }
}

void tapOnCard(int x, int y) {
    if (y < HEADER_H) return;
    int rows = cardRows();
    for (int r = 0; r < rows; r++) {
        CardRow row = cardRow(r);
        if (row.kind != CardRow::Map && row.kind != CardRow::Rec) continue;
        bool del = deleteBtn(r).hit(x, y), ren = renameBtn(r).hit(x, y);
        int ry = HEADER_H + r * ROW_H - scroll;
        if (row.kind == CardRow::Rec && !del && !ren && y >= ry && y < ry + ROW_H && x < renameBtn(r).x) {
            recordings::Info ri;
            if (recordings::get(row.idx, ri)) openReview(ri.number);
            return;
        }
        if (!del && !ren) continue;
        if (ren) {
            if (row.kind == CardRow::Map) {
                mapdata::FileInfo fi;
                if (mapdata::fileInfo(row.idx, fi)) startEdit(Edit::Map, row.idx, fi.name, 31);
            } else {
                recordings::Info ri;
                if (recordings::get(row.idx, ri)) startEdit(Edit::Recording, ri.number, ri.label, recordings::MAX_LABEL);
            }
            return;
        }
        if (confirmIdx == r && millis() < confirmUntil) {
            confirmIdx = -1;
            if (row.kind == CardRow::Map) {
                mapdata::removeFile(row.idx);
            } else {
                recordings::Info ri;
                if (recordings::get(row.idx, ri) && !recordings::remove(ri.number))
                    message("could not delete the recording");
            }
            scroll = std::min(scroll, maxScroll(cardRows()));
        } else {
            confirmIdx = r;
            confirmUntil = millis() + 4000;
        }
        dirty = true;
        return;
    }
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
    s ^= recordings::count() * 7907 ^ (uint32_t)edit << 28 ^ (uint32_t)(reviewNumber + 1) * 92821u;
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
    edit = Edit::None;
    drag = Drag::None;
    if (cur == View::OnCard) recordings::scan();
}

void leave() {
    selectMode = false;
    edit = Edit::None;
    if (reviewNumber >= 0) closeReview();
    revMap.deleteSprite();
    tiles::clear();
}

View view() { return cur; }

bool reviewing() { return reviewNumber >= 0; }

void setView(View v) {
    if (edit != Edit::None) {
        edit = Edit::None;
        dirty = true;
    }
    if (reviewNumber >= 0) closeReview();
    if (v == cur) return;
    cur = v;
    scroll = 0;
    confirmIdx = -1;
    selectMode = false;
    if (v != View::Draw) tiles::clear();
    if (v == View::OnCard) recordings::scan();
    dirty = true;
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
    else if (edit != Edit::None) drawKeyboard();
    else if (cur == View::Library) drawLibrary();
    else if (cur == View::Draw) drawPicker();
    else if (reviewNumber >= 0) drawReview();
    else drawOnCard();
    fb::push(cv, X0, Y0);
}

void tap(int x, int y) {
    x -= X0, y -= Y0;
    if (net::setupActive()) return;
    if (edit != Edit::None) tapKeyboard(x, y);
    else if (cur == View::Library) tapLibrary(x, y);
    else if (cur == View::Draw) tapPicker(x, y);
    else if (reviewNumber >= 0) tapReview(x, y);
    else tapOnCard(x, y);
}

void dragStart(int x, int y) {
    x -= X0, y -= Y0;
    dragX0 = x, dragY0 = y;
    if (net::setupActive() || edit != Edit::None || y < HEADER_H) {
        drag = Drag::None;
    } else if (reviewNumber >= 0) {
        drag = x < REV_MAP_W && !revError ? Drag::ReviewPan : Drag::None;
        dragCy0 = revLat;
        dragCx0 = revLon;
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
        case Drag::ReviewPan:
            revLat = dragCy0 + (y - dragY0) * revMpp / 111320.0;
            revLon = dragCx0 - (x - dragX0) * revMpp / (111320.0 * cos(revLat * M_PI / 180.0));
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
