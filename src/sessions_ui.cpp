#include "sessions_ui.h"

#include <M5Unified.h>
#include <math.h>

#include <algorithm>

#include "display_fb.h"
#include "keyboard.h"
#include "map_data.h"
#include "map_view.h"
#include "recordings.h"
#include "review.h"

namespace sessionsui {
namespace {

constexpr int HEADER_H = 52;
// Row buttons as on the Settings screen: 90 px rows, 74 px tall, right-aligned; 190 px wide so
// RENAME and CONFIRM fit in the large font.
constexpr int ROW_H = 90;
constexpr int BTN_W = 190, BTN2_X = 1280 - 20 - BTN_W, BTN1_X = BTN2_X - 20 - BTN_W;
constexpr int REV_MAP_W = 760;
constexpr uint32_t FRAME_MS = 100;

int X0, Y0, W, H;
M5Canvas cv(&M5.Display);
Palette c;
bool dirty = true;
uint32_t lastDraw = 0;
uint32_t lastSig = 0;

char msg[80] = "";
uint32_t msgUntil = 0;

int scroll = 0, scrollStart = 0;
int confirmIdx = -1;
uint32_t confirmUntil = 0;

bool editingName = false;
int editNumber = 0;

int reviewNumber = -1;
const char* revError = nullptr;
M5Canvas revMap(&cv);
double revLat = 0, revLon = 0;
float revMpp = 2;

enum class Drag { None, Scroll, Pan } drag = Drag::None;
int dragX0, dragY0;
double dragLat0, dragLon0;

struct Rect {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

int bodyH() { return H - HEADER_H; }

void message(const char* text) {
    strlcpy(msg, text, sizeof(msg));
    msgUntil = millis() + 4000;
    dirty = true;
}

void button(const Rect& r, const char* label, bool lit = false, uint16_t fill = 0) {
    uint16_t bg = lit ? c.accent : fill ? fill : c.btn;
    int rad = r.h >= 60 ? 16 : 12;
    cv.fillRoundRect(r.x, r.y, r.w, r.h, rad, bg);
    cv.drawRoundRect(r.x, r.y, r.w, r.h, rad, c.dim);
    cv.setTextSize(1);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    if (r.h < 60 || cv.textWidth(label) > r.w - 12) cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextDatum(middle_center);
    cv.setTextColor(lit || fill ? c.bg : c.btnText);
    cv.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
}

void fmtBytes(char* out, size_t n, uint32_t b) {
    if (b >= 1024 * 1024) snprintf(out, n, "%.1f MB", b / 1048576.0);
    else snprintf(out, n, "%u KB", (unsigned)((b + 512) / 1024));
}

void fmtDuration(char* out, size_t n, float s) {
    int t = (int)s;
    if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

void fmtSplit(char* out, size_t n, float s) {
    if (s <= 0) snprintf(out, n, "--");
    else snprintf(out, n, "%d:%04.1f", (int)s / 60, fmodf(s, 60));
}

// ---- header ----------------------------------------------------------------

void drawHeader() {
    cv.fillRect(0, 0, W, HEADER_H, c.bg);
    cv.drawFastHLine(0, HEADER_H - 1, W, c.dim);
    cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextSize(1);
    cv.setTextDatum(middle_left);
    cv.setTextColor(c.label);
    char title[32];
    if (editingName) snprintf(title, sizeof(title), "RENAME RECORDING");
    else if (reviewNumber >= 0) snprintf(title, sizeof(title), "SESSION");
    else snprintf(title, sizeof(title), "RECORDINGS (%d)", recordings::count());
    cv.drawString(title, 20, HEADER_H / 2);

    if (millis() < msgUntil) {
        cv.setTextDatum(middle_center);
        cv.setTextColor(c.warn);
        cv.drawString(msg, W / 2 + 40, HEADER_H / 2);
    }
    if (reviewNumber < 0 && !editingName) {
        cv.setFont(&fonts::FreeSans12pt7b);
        cv.setTextDatum(middle_right);
        cv.setTextColor(c.label);
        cv.drawString("copy: python tools\\fetch.py COM10", W - 20, HEADER_H / 2);
    }
}

// ---- list ------------------------------------------------------------------

int rows() { return std::max(1, recordings::count()); }
int maxScroll() { return std::max(0, rows() * ROW_H - bodyH()); }
Rect deleteBtn(int row) { return {BTN2_X, HEADER_H + row * ROW_H - scroll + 8, BTN_W, ROW_H - 16}; }
Rect renameBtn(int row) { return {BTN1_X, HEADER_H + row * ROW_H - scroll + 8, BTN_W, ROW_H - 16}; }

void drawList() {
    cv.fillRect(0, HEADER_H, W, bodyH(), c.bg);
    if (confirmIdx >= 0 && millis() > confirmUntil) confirmIdx = -1;
    cv.setClipRect(0, HEADER_H, W, bodyH());
    if (!recordings::count()) {
        cv.setFont(&fonts::FreeSans12pt7b);
        cv.setTextColor(c.label);
        cv.setTextDatum(middle_left);
        cv.drawString("No recordings yet. Press START on the main screen to record a session.", 24,
                      HEADER_H + ROW_H / 2);
    }
    for (int r = 0; r < recordings::count(); r++) {
        int y = HEADER_H + r * ROW_H - scroll;
        if (y + ROW_H < HEADER_H || y > H) continue;
        recordings::Info ri;
        if (!recordings::get(r, ri)) continue;
        int cy = y + ROW_H / 2;
        char a[64], b[64];
        cv.setTextDatum(middle_left);
        cv.setFont(&fonts::FreeSansBold12pt7b);
        cv.setTextColor(c.fg);
        if (ri.label[0]) snprintf(a, sizeof(a), "%s", ri.label);
        else snprintf(a, sizeof(a), "Session %d", ri.number);
        cv.drawString(a, 24, cy);
        cv.setFont(&fonts::FreeSans12pt7b);
        cv.setTextColor(c.label);
        snprintf(b, sizeof(b), "S%04d  %s", ri.number, ri.start);
        cv.drawString(b, 390, cy);
        char size[16], dur[16] = "";
        fmtBytes(size, sizeof(size), ri.bytes);
        if (ri.durationS > 0) fmtDuration(dur, sizeof(dur), ri.durationS);
        snprintf(b, sizeof(b), "%s  %s", dur, size);
        cv.setTextDatum(middle_right);
        cv.drawString(b, renameBtn(r).x - 16, cy);
        cv.drawFastHLine(0, y + ROW_H - 1, W, c.dim);
        bool confirm = r == confirmIdx;
        button(renameBtn(r), "RENAME");
        button(deleteBtn(r), confirm ? "CONFIRM" : "DELETE", false, confirm ? c.warn : 0);
    }
    cv.clearClipRect();
    int ms = maxScroll();
    if (ms > 0) {
        int bh = bodyH() * bodyH() / (rows() * ROW_H);
        int by = HEADER_H + (bodyH() - bh) * scroll / ms;
        cv.fillRect(W - 6, by, 4, bh, c.dim);
    }
}

void openReview(int number);

void tapList(int x, int y) {
    if (y < HEADER_H) return;
    for (int r = 0; r < recordings::count(); r++) {
        int ry = HEADER_H + r * ROW_H - scroll;
        if (y < ry || y >= ry + ROW_H) continue;
        recordings::Info ri;
        if (!recordings::get(r, ri)) return;
        if (renameBtn(r).hit(x, y)) {
            editingName = true;
            editNumber = ri.number;
            osk::start(ri.label, recordings::MAX_LABEL);
            confirmIdx = -1;
        } else if (deleteBtn(r).hit(x, y)) {
            if (confirmIdx == r && millis() < confirmUntil) {
                confirmIdx = -1;
                if (!recordings::remove(ri.number)) message("could not delete (recording in progress?)");
                scroll = std::min(scroll, maxScroll());
            } else {
                confirmIdx = r;
                confirmUntil = millis() + 4000;
            }
        } else if (x < renameBtn(r).x) {
            openReview(ri.number);
        }
        dirty = true;
        return;
    }
}

// ---- review ----------------------------------------------------------------

void fitReview() {
    double s, w, n, e;
    if (!review::trackBounds(s, w, n, e)) return;
    revLat = (s + n) / 2;
    revLon = (w + e) / 2;
    double wm = (e - w) * 111320.0 * cos(revLat * M_PI / 180.0), hm = (n - s) * 111320.0;
    revMpp = (float)std::max(0.5, std::max(wm / (REV_MAP_W - 80), hm / (bodyH() - 80)));
}

void openReview(int number) {
    revError = nullptr;
    reviewNumber = number;
    if (!review::load(number)) {
        revError = "This recording has no data.";
    } else if (review::summary().hasGps) {
        fitReview();
        mapdata::update(revLat, revLon, true);  // the map file covering the track, if any
        if (!revMap.getBuffer()) {
            revMap.setColorDepth(16);
            revMap.setPsram(true);
            revMap.createSprite(REV_MAP_W, bodyH());
        }
    }
    dirty = true;
}

void closeReview() {
    reviewNumber = -1;
    review::unload();
    revMap.deleteSprite();
    dirty = true;
}

// Time-series graph: x = session time, y = value; `invert` puts small values at the top (splits).
void drawGraph(int x, int y, int w, int h, const char* title, float lo, float hi, bool invert,
               float (*value)(int), uint16_t color, const char* loText, const char* hiText) {
    cv.drawRect(x, y, w, h, c.dim);
    int n = review::count();
    float t1 = n ? std::max(1.0f, review::elapsed(n - 1)) : 1;
    int px = -1, py = 0;
    float pt = 0;
    for (int i = 0; i < n; i++) {
        float v = value(i);
        float t = review::elapsed(i);
        int sx = x + (int)(t / t1 * (w - 1));
        if (v <= 0) {
            px = -1;
            continue;
        }
        float f = std::max(0.0f, std::min(1.0f, (v - lo) / (hi - lo)));
        int sy = invert ? y + 3 + (int)(f * (h - 7)) : y + h - 4 - (int)(f * (h - 7));
        if (px >= 0 && t - pt <= 5) {
            cv.drawLine(px, py, sx, sy, color);
            cv.drawLine(px, py + 1, sx, sy + 1, color);
        } else {
            cv.fillRect(sx, sy, 2, 2, color);
        }
        px = sx;
        py = sy;
        pt = t;
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
float hrAt(int i) { return review::hr(i); }

// The graphs that have data, stacked in the given box.
void drawGraphs(int x, int y, int w, int h) {
    const review::Summary& s = review::summary();
    bool hasHr = s.avgHr > 0;
    int count = (s.hasGps ? 1 : 0) + 1 + (hasHr ? 1 : 0);
    int gap = 8, gh = (h - (count - 1) * gap) / count;
    if (s.hasGps) {
        drawGraph(x, y, w, gh, "SPLIT /500m", 100, 200, true, splitAt, c.accent, "1:40", "3:20");
        y += gh + gap;
    }
    drawGraph(x, y, w, gh, "RATE spm", 14, 40, false, rateAt, c.ok, "14", "40");
    y += gh + gap;
    if (hasHr) {
        float lo = 1000;
        for (int i = 0; i < review::count(); i++)
            if (review::hr(i) > 0) lo = std::min(lo, review::hr(i));
        int l = std::max(30, ((int)lo - 10) / 10 * 10), hi = (s.maxHr + 19) / 10 * 10;
        char loText[8], hiText[8];
        snprintf(loText, sizeof(loText), "%d", l);
        snprintf(hiText, sizeof(hiText), "%d", hi);
        drawGraph(x, y, w, gh, "HEART RATE bpm", l, hi, false, hrAt, c.warn, loText, hiText);
    }
}

void drawLegend() {
    float slow, fast;
    review::speedScale(slow, fast);
    char a[16], b[16];
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
}

void drawReview() {
    cv.fillRect(0, HEADER_H, W, bodyH(), c.bg);
    if (revError) {
        cv.setFont(&fonts::FreeSansBold12pt7b);
        cv.setTextColor(c.label);
        cv.setTextDatum(middle_center);
        cv.drawString(revError, W / 2, HEADER_H + bodyH() / 2);
        return;
    }
    const review::Summary& s = review::summary();

    // Left: the track on the map, or without GPS the graphs.
    if (s.hasGps) {
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
        drawLegend();
    } else {
        cv.setFont(&fonts::FreeSansBold12pt7b);
        cv.setTextColor(c.warn);
        cv.setTextDatum(top_left);
        cv.drawString("No GPS fix in this session: no track, distance or split.", 16, HEADER_H + 14);
        drawGraphs(16, HEADER_H + 50, REV_MAP_W - 32, bodyH() - 62);
    }

    // Right: title and totals.
    int px = REV_MAP_W + 20;
    char a[48], b[48];
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

    char dist[16], dur[16], avg[16], best[16], rate[16], strokes[16], laps[16], dps[16], vmax[16], hrAvg[16],
        hrMax[16], hrBoth[24];
    snprintf(dist, sizeof(dist), "%.0f m", s.distanceM);
    fmtDuration(dur, sizeof(dur), s.durationS);
    fmtSplit(avg, sizeof(avg), s.avgSplitS);
    fmtSplit(best, sizeof(best), s.best500S);
    if (s.avgRate > 0) snprintf(rate, sizeof(rate), "%.1f", s.avgRate);
    else snprintf(rate, sizeof(rate), "--");
    snprintf(strokes, sizeof(strokes), "%d", s.strokes);
    snprintf(laps, sizeof(laps), "%d", s.laps);
    if (s.avgDps > 0) snprintf(dps, sizeof(dps), "%.1f m", s.avgDps);
    else snprintf(dps, sizeof(dps), "--");
    snprintf(vmax, sizeof(vmax), "%.1f m/s", s.maxSpeed);
    snprintf(hrAvg, sizeof(hrAvg), "%.0f", s.avgHr);
    snprintf(hrMax, sizeof(hrMax), "%d", s.maxHr);
    snprintf(hrBoth, sizeof(hrBoth), "%.0f / %d", s.avgHr, s.maxHr);

    const char* labels[10];
    const char* values[10];
    int n = 0;
    auto add = [&](const char* l, const char* v) {
        labels[n] = l;
        values[n++] = v;
    };
    if (s.hasGps) {
        add("DISTANCE", dist);
        add("TIME", dur);
        add("AVG SPLIT", avg);
        add("BEST 500 M", best);
        add("AVG RATE", rate);
        add("STROKES", strokes);
        add("DIST/STROKE", dps);
        add("MAX SPEED", vmax);
        if (s.avgHr > 0) add("HR AVG / MAX", hrBoth);
    } else {
        add("TIME", dur);
        add("AVG RATE", rate);
        add("STROKES", strokes);
        add("LAPS", laps);
        if (s.avgHr > 0) {
            add("AVG HR", hrAvg);
            add("MAX HR", hrMax);
        }
    }
    for (int i = 0; i < n; i++) {
        int cx = px + (i % 3) * 162, cy = HEADER_H + 86 + (i / 3) * 50;
        cv.setFont(&fonts::FreeSans9pt7b);
        cv.setTextColor(c.label);
        cv.drawString(labels[i], cx, cy);
        cv.setFont(&fonts::FreeSansBold12pt7b);
        cv.setTextColor(c.fg);
        cv.drawString(values[i], cx, cy + 18);
    }

    if (s.hasGps) {
        int gy = HEADER_H + 86 + (n + 2) / 3 * 50 + 6;
        drawGraphs(px, gy, W - px - 16, H - gy - 10);
    }
}

void tapReview(int x, int y) {
    if (revError || !review::summary().hasGps || x >= REV_MAP_W || y < HEADER_H) return;
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
}

// ---- rename ----------------------------------------------------------------

void tapKeyboard(int x, int y) {
    osk::Result r = osk::tap(x, y, W, HEADER_H);
    if (r == osk::Result::Save) {
        bool ok = recordings::rename(editNumber, osk::text());
        message(ok ? "recording renamed" : "could not rename (recording in progress?)");
        editingName = false;
    } else if (r == osk::Result::Cancel) {
        editingName = false;
    }
    dirty = true;
}

uint32_t signature() {
    uint32_t s = recordings::count() * 7907 ^ (uint32_t)editingName << 28 ^ (uint32_t)(reviewNumber + 1) * 92821u;
    s ^= (millis() < msgUntil) << 20 ^ (confirmIdx + 1) << 22;
    return s;
}

}  // namespace

void begin(int x, int y, int w, int h) {
    X0 = x, Y0 = y, W = w, H = h;
    cv.setColorDepth(16);
    cv.setPsram(true);
    cv.createSprite(W, H);
}

void setColors(const Palette& colors) {
    c = colors;
    dirty = true;
}

void enter() {
    recordings::scan();
    scroll = 0;
    confirmIdx = -1;
    editingName = false;
    drag = Drag::None;
    dirty = true;
}

void leave() {
    editingName = false;
    if (reviewNumber >= 0) closeReview();
}

bool reviewing() { return reviewNumber >= 0; }
bool editing() { return editingName; }

void back() {
    if (editingName) editingName = false;
    else if (reviewNumber >= 0) closeReview();
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
    if (editingName) osk::draw(cv, HEADER_H, c);
    else if (reviewNumber >= 0) drawReview();
    else drawList();
    fb::push(cv, X0, Y0);
}

void tap(int x, int y) {
    x -= X0, y -= Y0;
    if (editingName) tapKeyboard(x, y);
    else if (reviewNumber >= 0) tapReview(x, y);
    else tapList(x, y);
}

void dragStart(int x, int y) {
    x -= X0, y -= Y0;
    dragX0 = x, dragY0 = y;
    if (editingName || y < HEADER_H) {
        drag = Drag::None;
    } else if (reviewNumber >= 0) {
        bool map = !revError && review::summary().hasGps && x < REV_MAP_W;
        drag = map ? Drag::Pan : Drag::None;
        dragLat0 = revLat;
        dragLon0 = revLon;
    } else {
        drag = Drag::Scroll;
        scrollStart = scroll;
    }
}

void dragMove(int x, int y) {
    x -= X0, y -= Y0;
    switch (drag) {
        case Drag::Scroll: scroll = std::max(0, std::min(maxScroll(), scrollStart - (y - dragY0))); break;
        case Drag::Pan:
            revLat = dragLat0 + (y - dragY0) * revMpp / 111320.0;
            revLon = dragLon0 - (x - dragX0) * revMpp / (111320.0 * cos(revLat * M_PI / 180.0));
            break;
        case Drag::None: return;
    }
    dirty = true;
}

void dragEnd(int x, int y) {
    dragMove(x, y);
    drag = Drag::None;
    dirty = true;
}

}  // namespace sessionsui
