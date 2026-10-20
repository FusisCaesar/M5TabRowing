#include "ui.h"

#include <M5Unified.h>

#include <atomic>

#include "board.h"
#include "camera.h"
#include "display_fb.h"
#include "config.h"
#include "efficiency.h"
#include "gps_nav.h"
#include "hr.h"
#include "icons.h"
#include "imu_stroke.h"
#include "logger.h"
#include "map_data.h"
#include "map_job.h"
#include "map_view.h"
#include "maps_ui.h"
#include "net.h"
#include "metrics.h"
#include "rtclock.h"
#include "sessions_ui.h"
#include "settings.h"
#include "track.h"

namespace ui {

namespace {

// ---- layout (landscape 1280 x 720) ---------------------------------------
constexpr int W = 1280;
constexpr int H = 720;
constexpr int BAR_H = 56;
constexpr int BIG_W = 640, BIG_H = 232;
constexpr int SMALL_Y = BAR_H + 2 * BIG_H;
constexpr int SMALL_W = 320, SMALL_H = 100;
constexpr int BTN_Y = SMALL_Y + SMALL_H;
constexpr int BTN_COUNT = 5;
constexpr int BTN_W = W / BTN_COUNT, BTN_H = H - BTN_Y;
// Camera picture-in-picture: left half, metrics stacked on the right.
constexpr int PIP_W = 640, PIP_H = 360;
constexpr int PIP_INFO_Y = BAR_H + PIP_H + 4;
// Camera full screen: one row of small metric tiles above the image.
constexpr int FULL_CAM_Y = BAR_H + SMALL_H;
constexpr int FULL_CAM_H = BTN_Y - FULL_CAM_Y;
constexpr int CONTENT_H = BTN_Y - BAR_H;
constexpr int PLOT_H = 430;
constexpr int ROW_H = 67;
constexpr int PLOT_POINTS = 600;   // 6 s at 100 Hz
constexpr float PLOT_RANGE_G = 0.6f;

// The NEXT button cycles through the first CYCLE screens; Maps opens from the Map screen.
// Screens from Sessions on draw their own content area (sessions_ui, maps_ui).
enum class Screen { Main, Map, Efficiency, Plot, Settings, Sessions, Maps };
constexpr int CYCLE = 6;
enum PipMode : uint8_t { PipOff = 0, PipCamera = 1, PipMap = 2 };
// Efficiency screen: chart on the left, score and technique on the right, coaching hint below.
constexpr int EFF_H = BTN_Y - ROW_H - BAR_H;
constexpr int EFF_CHART_W = 800, EFF_PANEL_W = W - EFF_CHART_W;

struct Theme {
    uint16_t bg, fg, label, accent, dim, btn, btnText, warn, ok, caution;
};
const Theme NIGHT = {TFT_BLACK, TFT_WHITE, 0xAD55, TFT_YELLOW, 0x39E7, 0x2124, TFT_WHITE, TFT_RED, TFT_GREEN, TFT_ORANGE};
const Theme DAY = {TFT_WHITE, TFT_BLACK, 0x31A6, 0x001F, 0xC618, 0xDEFB, TFT_BLACK, 0xC000, 0x0400, 0xCB20};

M5Canvas bigC(&M5.Display);
M5Canvas smallC(&M5.Display);
M5Canvas barC(&M5.Display);
M5Canvas rowC(&M5.Display);
M5Canvas plotC(&M5.Display);
M5Canvas btnC(&M5.Display);
M5Canvas setC(&M5.Display);
M5Canvas setTabC(&M5.Display);
M5Canvas mapC(&M5.Display);     // full-size map area (Map screen or full-on-Main)
M5Canvas mapPipC(&M5.Display);  // picture-in-picture map
M5Canvas effC(&M5.Display);     // efficiency chart
M5Canvas effP(&M5.Display);     // efficiency score and technique panel

Screen screen = Screen::Main;
const Theme* th = &NIGHT;
bool forceRedraw = true;
camera::Status camShown = camera::Status::Off;
uint32_t lastMap = 0;
uint32_t lastMapScan = 0;
bool dragging = false;
// On-map button that opens the Maps screen (Map screen, no session running).
constexpr int MAPS_BTN_X = 16, MAPS_BTN_Y = 16, MAPS_BTN_W = 170, MAPS_BTN_H = 64;

struct Cache {
    char text[96];
};
Cache bigCache[4], smallCache[6], btnCache[BTN_COUNT], barCache, rowCache[12];

char hint[48] = "";
uint32_t hintUntil = 0;
uint32_t lastFrame = 0;
uint32_t lastPlot = 0;
uint32_t lastEff = 0, effVersion = 0;
uint32_t lastBattery = 0;

imu::PlotPoint plotBuf[PLOT_POINTS];

// ---- formatting ------------------------------------------------------------

void fmtSplit(char* out, size_t n, float s) {
    if (s <= 0 || s >= 600) {
        snprintf(out, n, "-:--.-");
        return;
    }
    int tenths = (int)lroundf(s * 10);
    snprintf(out, n, "%d:%02d.%d", tenths / 600, (tenths / 10) % 60, tenths % 10);
}

void fmtTime(char* out, size_t n, float s) {
    if (s < 3600) {
        int tenths = (int)(s * 10);
        snprintf(out, n, "%d:%02d.%d", tenths / 600, (tenths / 10) % 60, tenths % 10);
    } else {
        int sec = (int)s;
        snprintf(out, n, "%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
    }
}

uint16_t adviceColor(const efficiency::RatioCheck& rc) {
    switch (rc.advice) {
        case efficiency::Advice::None: return th->fg;
        case efficiency::Advice::Good: return th->ok;
        default: return rc.far ? th->warn : th->caution;
    }
}

// Green near the rower's usual value, orange above it, red well above it (lower is better).
uint16_t usualColor(float value, float usual) {
    if (usual <= 0 || value <= 0) return th->fg;
    return value <= 1.1f * usual ? th->ok : value <= 1.25f * usual ? th->caution : th->warn;
}

uint16_t scoreColor(int score) {
    return score <= 0 ? th->fg : score >= 93 ? th->ok : score >= 85 ? th->caution : th->warn;
}

bool changed(Cache& c, const char* text) {
    if (!forceRedraw && strcmp(c.text, text) == 0) return false;
    strlcpy(c.text, text, sizeof(c.text));
    return true;
}

void showHint(const char* text) {
    strlcpy(hint, text, sizeof(hint));
    hintUntil = millis() + 2500;
}

// ---- drawing primitives ----------------------------------------------------

void drawValue(M5Canvas& c, const char* value, int top, float maxScale, uint16_t color) {
    c.setFont(&fonts::Font7);
    c.setTextSize(1);
    int tw = c.textWidth(value);
    int fh = c.fontHeight();
    float s = maxScale;
    if (tw > 0) s = fminf(s, (c.width() - 40) / (float)tw);
    s = fminf(s, (c.height() - top - 10) / (float)fh);
    c.setTextSize(s);
    c.setTextColor(color);
    c.setTextDatum(middle_center);
    c.drawString(value, c.width() / 2, top + (c.height() - top) / 2);
}

void drawTile(M5Canvas& c, int x, int y, const char* label, const char* unit, const char* value,
              bool big, uint16_t color, int unitColor = -1) {
    c.fillSprite(th->bg);
    c.drawRoundRect(4, 4, c.width() - 8, c.height() - 8, 14, th->dim);
    c.setFont(big ? &fonts::FreeSansBold18pt7b : &fonts::FreeSansBold12pt7b);
    c.setTextSize(1);
    c.setTextColor(th->label);
    c.setTextDatum(top_left);
    c.drawString(label, 20, big ? 16 : 12);
    if (unitColor >= 0) c.setTextColor((uint16_t)unitColor);
    c.setTextDatum(top_right);
    c.drawString(unit, c.width() - 20, big ? 16 : 12);
    drawValue(c, value, big ? 56 : 38, big ? 3.6f : 1.15f, color);
    fb::push(c, x, y);
}

void drawButton(int idx, const char* label, bool enabled, bool lit = false) {
    char key[64];
    snprintf(key, sizeof(key), "%s|%d|%d", label, enabled, lit);
    if (!changed(btnCache[idx], key)) return;
    M5Canvas& c = btnC;
    c.fillSprite(th->bg);
    c.fillRoundRect(8, 8, BTN_W - 16, BTN_H - 16, 18, lit ? th->accent : enabled ? th->btn : th->bg);
    c.drawRoundRect(8, 8, BTN_W - 16, BTN_H - 16, 18, th->dim);
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextSize(1);
    if (c.textWidth(label) > BTN_W - 32) c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextColor(lit ? th->bg : enabled ? th->btnText : th->dim);
    c.setTextDatum(middle_center);
    c.drawString(label, BTN_W / 2, BTN_H / 2);
    fb::push(c, idx * BTN_W, BTN_Y);
}

void drawRow(int idx, int y, const char* left, const char* right, uint16_t color) {
    char key[96];
    snprintf(key, sizeof(key), "%s|%s", left, right);
    if (!changed(rowCache[idx], key)) return;
    rowC.fillSprite(th->bg);
    rowC.setFont(&fonts::FreeSans12pt7b);
    rowC.setTextSize(1);
    rowC.setTextColor(color);
    rowC.setTextDatum(middle_left);
    rowC.drawString(left, 24, ROW_H / 2);
    rowC.setTextDatum(middle_right);
    rowC.drawString(right, W - 24, ROW_H / 2);
    fb::push(rowC, 0, y);
}

// ---- top bar and buttons ---------------------------------------------------

void drawTopBar(const metrics::View& v) {
    char left[32], mid[80], right[48];
    switch (v.state) {
        case metrics::State::Ready: snprintf(left, sizeof(left), "READY"); break;
        case metrics::State::Running:
            if (v.session >= 0) snprintf(left, sizeof(left), "REC  S%04d", v.session);
            else snprintf(left, sizeof(left), "RUNNING (no SD)");
            break;
        case metrics::State::Stopped: snprintf(left, sizeof(left), "STOPPED"); break;
    }

    if (millis() < hintUntil) {
        snprintf(mid, sizeof(mid), "%s", hint);
    } else if (v.hasLap && millis() - v.lapShownMs < 6000) {
        char split[16], t[16];
        fmtSplit(split, sizeof(split), v.lastLap.avgSplit);
        fmtTime(t, sizeof(t), v.lastLap.time);
        int n = snprintf(mid, sizeof(mid), "LAP %d  %s  %.0f m  %s/500  %.0f spm", v.lastLap.number, t,
                         v.lastLap.distance, split, v.lastLap.avgRate);
        if (v.lastLap.avgHr > 0 && n > 0 && n < (int)sizeof(mid))
            snprintf(mid + n, sizeof(mid) - n, "  %.0f bpm", v.lastLap.avgHr);
    } else if (!v.gpsConnected) {
        snprintf(mid, sizeof(mid), "GPS not connected");
    } else if (!v.gpsFix) {
        snprintf(mid, sizeof(mid), "GPS searching  %d sats", v.sats);
    } else {
        snprintf(mid, sizeof(mid), "GPS %.0f Hz  %d sats  HDOP %.1f", v.gpsHz, v.sats, v.hdop);
    }

#if SIM_MODE
    const char* sim = "SIM  ";
#else
    const char* sim = "";
#endif
    const char* wifi = "";
    switch (net::state()) {
        case net::State::Connecting: wifi = "WIFI...  "; break;
        case net::State::Connected: wifi = "WIFI  "; break;
        case net::State::Failed: wifi = "NO WIFI  "; break;
        case net::State::Off: break;
    }
    const char* hrText = "";
    if (hr::pairing()) hrText = "HR...  ";
    else if (hr::hasSaved()) {
        switch (v.hrState) {
            case hr::State::Connected: hrText = v.hr > 0 ? "HR  " : "HR...  "; break;
            case hr::State::Scanning:
            case hr::State::Connecting: hrText = "HR...  "; break;
            default: hrText = "NO HR  "; break;
        }
    }
    char clock[16] = "";
    if (rtclock::valid()) {
        time_t t = time(nullptr);
        tm l;
        localtime_r(&t, &l);
        strftime(clock, sizeof(clock), "%H:%M  ", &l);
    }
    if (v.battery >= 0) snprintf(right, sizeof(right), "%s%s%s%sBAT %d%%", hrText, wifi, sim, clock, v.battery);
    else snprintf(right, sizeof(right), "%s%s%s%s", hrText, wifi, sim, clock);

    char key[160];
    snprintf(key, sizeof(key), "%s|%s|%s", left, mid, right);
    if (!changed(barCache, key)) return;

    barC.fillSprite(th->bg);
    barC.drawFastHLine(0, BAR_H - 1, W, th->dim);
    barC.setFont(&fonts::FreeSansBold12pt7b);
    barC.setTextSize(1);
    barC.setTextDatum(middle_left);
    barC.setTextColor(v.state == metrics::State::Running ? th->warn : th->label);
    barC.drawString(left, 20, BAR_H / 2);
    barC.setTextDatum(middle_center);
    barC.setTextColor(v.gpsFix || millis() < hintUntil ? th->fg : th->warn);
    barC.drawString(mid, W / 2, BAR_H / 2);
    barC.setTextDatum(middle_right);
    barC.setTextColor(th->label);
    barC.drawString(right, W - 20, BAR_H / 2);
    fb::push(barC, 0, 0);
}

const char* wifiButtonLabel() {
    if (net::setupActive()) return "CANCEL SETUP";
    switch (net::state()) {
        case net::State::Connected: return "WIFI: ON";
        case net::State::Connecting: return "WIFI: ...";
        default: return net::hasStoredNetwork() ? "WIFI: OFF" : "WIFI SETUP";
    }
}

void drawMapsButtons() {
    mapsui::View mv = mapsui::view();
    drawButton(0, "LIBRARY", true, mv == mapsui::View::Library);
    drawButton(1, "DRAW AREA", true, mv == mapsui::View::Draw);
    drawButton(2, "ON CARD", true, mv == mapsui::View::OnCard);
    drawButton(3, wifiButtonLabel(), true, net::connected() || net::setupActive());
    drawButton(4, "BACK", true);
}

void drawButtons(const metrics::View& v) {
    using metrics::State;
    if (screen == Screen::Maps) {
        drawMapsButtons();
        return;
    }
    const Settings& s = settings::get();
    const char* primary = v.state == State::Running ? "HOLD: STOP"
                          : v.state == State::Stopped ? "NEW" : "START";
    drawButton(0, primary, true);
    drawButton(1, "LAP", v.state == State::Running);
    drawButton(2, "HOLD: RESET", v.state == State::Stopped);

    // Button 3 depends on the screen: picture-in-picture mode on Main, map orientation on Map.
    if (screen == Screen::Main) {
        const char* pip = s.pipMode == PipCamera ? "PIP: CAM" : s.pipMode == PipMap ? "PIP: MAP" : "PIP: OFF";
        drawButton(3, pip, true, s.pipMode != PipOff);
    } else if (screen == Screen::Map) {
        drawButton(3, mapview::orientationName(s.mapOrientation), true,
                   s.mapOrientation != mapview::NorthUp);
    } else if (screen == Screen::Efficiency) {
        drawButton(3, "HOLD: CLEAR", efficiency::strokeCount() > 0);
    } else if (screen == Screen::Sessions) {
        bool inner = sessionsui::reviewing() || sessionsui::editing();
        drawButton(3, inner ? "BACK" : "", inner);
    } else {
        drawButton(3, "", false);
    }

    const char* next = screen == Screen::Main         ? "MAP"
                       : screen == Screen::Map        ? "EFFICIENCY"
                       : screen == Screen::Efficiency ? "PLOT"
                       : screen == Screen::Plot       ? "SETTINGS"
                       : screen == Screen::Settings   ? "SESSIONS"
                                                      : "METRICS";
    drawButton(4, next, true);
}

// ---- main metrics screen ---------------------------------------------------

void drawSmallRow(const metrics::View& v);

// The four key numbers in one row across the top (used by full-width PIP and the Map screen).
void drawTopTiles(const metrics::View& v) {
    char split[16], rate[8], dist[16], elapsed[16];
    fmtSplit(split, sizeof(split), v.split);
    if (v.rate > 0) snprintf(rate, sizeof(rate), "%d", (int)lroundf(v.rate));
    else snprintf(rate, sizeof(rate), "--");
    snprintf(dist, sizeof(dist), "%d", (int)v.distance);
    fmtTime(elapsed, sizeof(elapsed), v.elapsed);
    if (changed(smallCache[0], split)) drawTile(smallC, 0, BAR_H, "SPLIT", "/500m", split, false, th->fg);
    if (changed(smallCache[1], rate)) drawTile(smallC, SMALL_W, BAR_H, "RATE", "spm", rate, false, th->accent);
    if (changed(smallCache[2], dist)) drawTile(smallC, 2 * SMALL_W, BAR_H, "DISTANCE", "m", dist, false, th->fg);
    // Heart rate rides along in the TIME tile's unit corner; there is no room for a fifth tile.
    char hrUnit[16] = "";
    if (hr::hasSaved()) {
        if (v.hr > 0) snprintf(hrUnit, sizeof(hrUnit), "%d bpm", v.hr);
        else snprintf(hrUnit, sizeof(hrUnit), "-- bpm");
    }
    char timeKey[32];
    snprintf(timeKey, sizeof(timeKey), "%s|%s", elapsed, hrUnit);
    if (changed(smallCache[3], timeKey))
        drawTile(smallC, 3 * SMALL_W, BAR_H, "TIME", hrUnit, elapsed, false, th->fg, v.hr > 0 ? th->fg : th->warn);
}

void drawMain(const metrics::View& v) {
    char split[16], rate[8], dist[16], elapsed[16];
    fmtSplit(split, sizeof(split), v.split);
    if (v.rate > 0) snprintf(rate, sizeof(rate), "%d", (int)lroundf(v.rate));
    else snprintf(rate, sizeof(rate), "--");
    snprintf(dist, sizeof(dist), "%d", (int)v.distance);
    fmtTime(elapsed, sizeof(elapsed), v.elapsed);

    const Settings& s = settings::get();
    bool hasPip = s.pipMode != PipOff;

    if (hasPip && s.camFull) {
        // Full-width content: everything that matters in one row above it.
        drawTopTiles(v);
        return;
    }
    if (hasPip) {
        // Picture-in-picture: content top left, split and rate stay big on the right.
        if (changed(bigCache[0], split)) drawTile(bigC, BIG_W, BAR_H, "SPLIT", "/500m", split, true, th->fg);
        if (changed(bigCache[1], rate)) drawTile(bigC, BIG_W, BAR_H + BIG_H, "RATE", "spm", rate, true, th->accent);
        if (changed(smallCache[4], dist)) drawTile(smallC, 0, PIP_INFO_Y, "DISTANCE", "m", dist, false, th->fg);
        if (changed(smallCache[5], elapsed)) drawTile(smallC, SMALL_W, PIP_INFO_Y, "TIME", "", elapsed, false, th->fg);
        drawSmallRow(v);
        return;
    }

    if (changed(bigCache[0], split)) drawTile(bigC, 0, BAR_H, "SPLIT", "/500m", split, true, th->fg);
    if (changed(bigCache[1], rate)) drawTile(bigC, BIG_W, BAR_H, "RATE", "spm", rate, true, th->accent);
    if (changed(bigCache[2], dist)) drawTile(bigC, 0, BAR_H + BIG_H, "DISTANCE", "m", dist, true, th->fg);
    if (changed(bigCache[3], elapsed)) drawTile(bigC, BIG_W, BAR_H + BIG_H, "TIME", "", elapsed, true, th->fg);
    drawSmallRow(v);
}

void drawSmallRow(const metrics::View& v) {
    char buf[24];
    if (v.dps > 0) snprintf(buf, sizeof(buf), "%.1f", v.dps);
    else snprintf(buf, sizeof(buf), "--");
    if (changed(smallCache[0], buf))
        drawTile(smallC, 0, SMALL_Y, "DIST/STROKE", v.dpsFused ? "m*" : "m", buf, false, th->fg);

    fmtSplit(buf, sizeof(buf), v.avgSplit);
    if (changed(smallCache[1], buf)) drawTile(smallC, SMALL_W, SMALL_Y, "AVG SPLIT", "/500m", buf, false, th->fg);

    if (hr::hasSaved()) {
        if (v.hr > 0) snprintf(buf, sizeof(buf), "%d", v.hr);
        else snprintf(buf, sizeof(buf), "--");
        char key[32];
        snprintf(key, sizeof(key), "hr|%s", buf);
        if (changed(smallCache[2], key))
            drawTile(smallC, 2 * SMALL_W, SMALL_Y, "HEART RATE", "bpm", buf, false, v.hr > 0 ? th->fg : th->warn);
    } else {
        snprintf(buf, sizeof(buf), "%d", v.strokes);
        if (changed(smallCache[2], buf)) drawTile(smallC, 2 * SMALL_W, SMALL_Y, "STROKES", "", buf, false, th->fg);
    }

    efficiency::RatioCheck rc = efficiency::checkRatio(v.ratio, v.rate);
    const char* word = efficiency::adviceWord(rc.advice);
    if (v.ratio > 0) snprintf(buf, sizeof(buf), "%.1f", v.ratio);
    else snprintf(buf, sizeof(buf), "--");
    char key[32];
    snprintf(key, sizeof(key), "%s|%s", buf, word);
    uint16_t col = adviceColor(rc);
    if (changed(smallCache[3], key)) drawTile(smallC, 3 * SMALL_W, SMALL_Y, "REC:DRIVE", word, buf, false, col, col);
}

// ---- efficiency screen -----------------------------------------------------

constexpr int EFF_RATE_LO = 16, EFF_RATE_HI = 40;

void drawEffChart(const efficiency::Summary& s) {
    M5Canvas& c = effC;
    c.fillSprite(th->bg);
    const int L = 64, R = EFF_CHART_W - 24, T = 44, B = EFF_H - 46;

    int n = efficiency::strokeCount();
    int first = max(0, n - 400);
    float lo = 1e9f, hi = 0;
    for (int i = first; i < n; i++) {
        float d = efficiency::stroke(i).dps;
        if (d > 0) lo = fminf(lo, d), hi = fmaxf(hi, d);
    }
    if (hi <= 0) {
        lo = 4, hi = 12;
    } else {
        lo = fmaxf(0, floorf(lo) - 1);
        hi = fmaxf(ceilf(hi) + 1, lo + 4);
    }
    auto X = [&](float r) { return (int)lroundf(L + (r - EFF_RATE_LO) * (R - L) / (EFF_RATE_HI - EFF_RATE_LO)); };
    auto Y = [&](float d) { return (int)lroundf(B - (d - lo) * (B - T) / (hi - lo)); };

    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextSize(1);
    c.setTextColor(th->label);
    c.setTextDatum(top_center);
    for (int r = EFF_RATE_LO; r <= EFF_RATE_HI; r += 2) {
        c.drawFastVLine(X(r), T, B - T, th->dim);
        char t[8];
        snprintf(t, sizeof(t), "%d", r);
        c.drawString(t, X(r), B + 6);
    }
    c.setTextDatum(middle_right);
    int step = hi - lo > 10 ? 2 : 1;
    for (int d = (int)ceilf(lo); d <= (int)hi; d += step) {
        c.drawFastHLine(L, Y(d), R - L, th->dim);
        char t[8];
        snprintf(t, sizeof(t), "%d", d);
        c.drawString(t, L - 8, Y(d));
    }

    c.setClipRect(L, T, R - L + 1, B - T + 1);
    // Lines of equal split: the same boat speed at a lower rate means longer, more efficient strokes.
    float split = s.dps > 0 && s.rate > 0 ? 500.0f / (s.dps * s.rate / 60.0f) : 130.0f;
    int base = (int)lroundf(split / 10.0f) * 10;
    c.setTextDatum(bottom_right);
    for (int k = -2; k <= 2; k++) {
        float sp = base + 10.0f * k;
        if (sp < 60) continue;
        float v = 500.0f / sp;
        int px = -1, py = -1, lx = -1, ly = -1;
        for (float r = EFF_RATE_LO; r <= EFF_RATE_HI; r += 0.25f) {
            int x = X(r), y = Y(v * 60.0f / r);
            if (px >= 0) c.drawLine(px, py, x, y, th->label);
            if (y >= T + 18 && y <= B) lx = x, ly = y;
            px = x, py = y;
        }
        if (lx >= 0) {
            char t[12];
            int sec = (int)sp;
            snprintf(t, sizeof(t), "%d:%02d", sec / 60, sec % 60);
            c.drawString(t, lx - 2, ly - 4);
        }
    }

    for (int i = first; i < n; i++) {
        const efficiency::Stroke& st = efficiency::stroke(i);
        if (st.dps <= 0) continue;
        c.fillCircle(X(st.rate), Y(st.dps), 3, i >= n - 30 ? th->fg : th->dim);
    }

    int px = -1, py = -1;
    for (int r = efficiency::MIN_RATE; r <= efficiency::MAX_RATE; r++) {
        float b = efficiency::bestAt(r);
        if (b <= 0) {
            px = -1;
            continue;
        }
        int x = X(r), y = Y(b);
        if (px >= 0)
            for (int o = -1; o <= 1; o++) c.drawLine(px, py + o, x, y + o, th->accent);
        c.fillCircle(x, y, 5, th->accent);
        px = x, py = y;
    }

    if (s.dps > 0) {
        int x = X(s.rate), y = Y(s.dps);
        c.fillCircle(x, y, 13, th->fg);
        c.fillCircle(x, y, 10, scoreColor(s.score));
    }
    c.clearClipRect();

    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(top_left);
    c.setTextColor(th->label);
    c.drawString("METRES PER STROKE", 16, 10);
    c.setTextDatum(top_right);
    c.setTextColor(th->accent);
    c.drawString("your best", R - 150, 10);
    c.setTextColor(th->label);
    c.drawString("equal split", R, 10);
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextDatum(bottom_right);
    c.drawString("stroke rate (spm)", R, EFF_H - 4);
    fb::push(c, 0, BAR_H);
}

// A horizontal gauge; `frac` is 0..1 along the track.
void effBar(M5Canvas& c, int y, float frac, uint16_t color) {
    const int x = 24, w = EFF_PANEL_W - 48, h = 22;
    c.drawRoundRect(x, y, w, h, 6, th->dim);
    int fw = (int)(fmaxf(0, fminf(1, frac)) * (w - 4));
    if (fw > 0) c.fillRoundRect(x + 2, y + 2, fw, h - 4, 4, color);
}

void effSection(M5Canvas& c, int y, const char* label, const char* value, uint16_t valueColor) {
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(top_left);
    c.setTextColor(th->label);
    c.drawString(label, 24, y);
    c.setTextDatum(top_right);
    c.setTextColor(valueColor);
    c.drawString(value, EFF_PANEL_W - 24, y);
}

void drawEffPanel(const efficiency::Summary& s) {
    M5Canvas& c = effP;
    c.fillSprite(th->bg);
    c.drawFastVLine(0, 0, EFF_H, th->dim);
    char a[48], b[64];

    effSection(c, 12, "EFFICIENCY", "", th->fg);
    uint16_t sc = scoreColor(s.score);
    if (s.score > 0) snprintf(a, sizeof(a), "%d", s.score);
    else snprintf(a, sizeof(a), "--");
    c.setFont(&fonts::Font7);
    c.setTextSize(2);
    c.setTextColor(sc);
    c.setTextDatum(middle_right);
    c.drawString(a, EFF_PANEL_W / 2 + 90, 100);
    c.setFont(&fonts::FreeSansBold24pt7b);
    c.setTextSize(1);
    c.setTextDatum(middle_left);
    c.drawString("%", EFF_PANEL_W / 2 + 100, 112);
    c.setFont(&fonts::FreeSans12pt7b);
    c.setTextDatum(top_center);
    c.setTextColor(th->label);
    if (s.score > 0) snprintf(b, sizeof(b), "of your best %.1f m at %d spm", s.best, (int)lroundf(s.rate));
    else if (s.rate > 0) snprintf(b, sizeof(b), "learning your best at %d spm", (int)lroundf(s.rate));
    else snprintf(b, sizeof(b), "distance per stroke vs your best");
    c.drawString(b, EFF_PANEL_W / 2, 152);
    c.drawFastHLine(16, 188, EFF_PANEL_W - 32, th->dim);

    // Recovery:drive against the target band.
    const efficiency::RatioCheck& rc = s.ratioCheck;
    uint16_t rcol = adviceColor(rc);
    if (rc.target > 0) snprintf(a, sizeof(a), "%.1f  aim %.1f", s.ratio, rc.target);
    else snprintf(a, sizeof(a), "--");
    effSection(c, 200, "REC:DRIVE", a, rcol);
    const float RLO = 0.5f, RHI = 3.5f;
    const int bx = 24, bw = EFF_PANEL_W - 48, by = 236;
    auto rx = [&](float r) { return bx + (int)((fmaxf(RLO, fminf(RHI, r)) - RLO) / (RHI - RLO) * bw); };
    c.drawRoundRect(bx, by, bw, 22, 6, th->dim);
    if (rc.target > 0) {
        int b0 = rx(rc.target * (1 - efficiency::RATIO_TOLERANCE)), b1 = rx(rc.target * (1 + efficiency::RATIO_TOLERANCE));
        c.fillRect(b0, by + 2, b1 - b0, 18, th->dim);
        c.drawRect(b0, by + 2, b1 - b0, 18, th->ok);
        int mx = rx(s.ratio);
        c.fillRect(mx - 4, by - 6, 8, 34, rcol);
    }
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextDatum(top_center);
    c.setTextColor(rcol);
    c.drawString(efficiency::adviceWord(rc.advice), EFF_PANEL_W / 2, 270);
    c.drawFastHLine(16, 318, EFF_PANEL_W - 32, th->dim);

    // Check and bounce relative to this session's usual values (lower is better).
    auto usualSection = [&](int y, const char* label, float v, float usual) {
        if (v <= 0) snprintf(a, sizeof(a), "--");
        else if (usual > 0) snprintf(a, sizeof(a), "%.0f mg  usual %.0f", v, usual);
        else snprintf(a, sizeof(a), "%.0f mg", v);
        uint16_t col = usualColor(v, usual);
        effSection(c, y, label, a, col);
        float scale = usual > 0 ? 2 * usual : fmaxf(v, 1) * 1.5f;
        effBar(c, y + 36, v / scale, col);
    };
    usualSection(332, "CHECK", s.checkMg, s.checkUsualMg);
    usualSection(414, "BOUNCE", s.bounceMg, s.bounceUsualMg);
    fb::push(c, EFF_CHART_W, BAR_H);
}

void drawEfficiency(const metrics::View& v) {
    uint32_t ver = efficiency::version();
    if (!forceRedraw && ver == effVersion && millis() - lastEff < 1000) return;
    effVersion = ver;
    lastEff = millis();
    efficiency::Summary s = efficiency::summary(v.rate);
    drawEffChart(s);
    drawEffPanel(s);
    char right[32];
    snprintf(right, sizeof(right), "%d strokes", efficiency::strokeCount());
    drawRow(0, BTN_Y - ROW_H, s.hint, right, s.hintWarn ? th->caution : th->fg);
}

// ---- live plot screen ------------------------------------------------------

int plotY(float g) {
    float y = PLOT_H / 2 - g * (PLOT_H / 2 - 10) / PLOT_RANGE_G;
    return (int)fmaxf(0, fminf(PLOT_H - 1, y));
}

void drawPlot(const metrics::View& v) {
    if (millis() - lastPlot >= 80 || forceRedraw) {
        lastPlot = millis();
        int n = imu::copyPlot(plotBuf, PLOT_POINTS);
        plotC.fillSprite(th->bg);
        plotC.setFont(&fonts::FreeSans9pt7b);
        plotC.setTextSize(1);
        plotC.setTextDatum(middle_left);
        plotC.setTextColor(th->label);
        for (float g : {-0.5f, -0.25f, 0.0f, 0.25f, 0.5f}) {
            int y = plotY(g);
            plotC.drawFastHLine(0, y, W, g == 0 ? th->label : th->dim);
            char lbl[12];
            snprintf(lbl, sizeof(lbl), "%+.2f g", g);
            plotC.drawString(lbl, 6, y - 10);
        }
        for (int i = 0; i < n; i++) {
            int x = i * (W - 1) / (PLOT_POINTS - 1);
            if (plotBuf[i].flags & 1) plotC.drawFastVLine(x, 0, PLOT_H, th->ok);
            if (plotBuf[i].flags & 2) plotC.drawFastVLine(x, PLOT_H / 2 - 40, 80, TFT_CYAN);
        }
        for (int i = 1; i < n; i++) {
            int x0 = (i - 1) * (W - 1) / (PLOT_POINTS - 1), x1 = i * (W - 1) / (PLOT_POINTS - 1);
            plotC.drawLine(x0, plotY(plotBuf[i - 1].threshold), x1, plotY(plotBuf[i].threshold), th->warn);
            int y0 = plotY(plotBuf[i - 1].surge), y1 = plotY(plotBuf[i].surge);
            plotC.drawLine(x0, y0, x1, y1, th->accent);
            plotC.drawLine(x0, y0 + 1, x1, y1 + 1, th->accent);
        }
        plotC.setFont(&fonts::FreeSansBold12pt7b);
        plotC.setTextDatum(top_right);
        int lx = W - 12;
        const std::pair<const char*, uint16_t> legend[] = {
            {"finish", TFT_CYAN}, {"catch   ", th->ok}, {"threshold   ", th->warn}, {"surge (g)   ", th->accent}};
        for (const auto& item : legend) {
            plotC.setTextColor(item.second);
            plotC.drawString(item.first, lx, 8);
            lx -= plotC.textWidth(item.first);
        }
        char rec[64];
        plotC.setTextDatum(top_left);
        if (v.state == metrics::State::Running && v.session >= 0) {
            snprintf(rec, sizeof(rec), "REC S%04d%s", v.session, logger::rawActive() ? "   raw IMU 100 Hz" : "");
            plotC.setTextColor(th->warn);
        } else if (v.state == metrics::State::Running) {
            snprintf(rec, sizeof(rec), "NOT RECORDING: no SD card");
            plotC.setTextColor(th->warn);
        } else {
            snprintf(rec, sizeof(rec), "Press START to record");
            plotC.setTextColor(th->label);
        }
        plotC.drawString(rec, 12, 8);
        fb::push(plotC, 0, BAR_H);
    }

    imu::Live l = imu::live();
    char a[96], b[96];
    snprintf(a, sizeof(a), "IMU %.1f Hz  overruns %lu   axis (%+.2f %+.2f %+.2f) sign %+d", l.sampleHz,
             (unsigned long)l.overruns, l.axis[0], l.axis[1], l.axis[2], l.sign);
    snprintf(b, sizeof(b), "rms %.3f g   threshold %.3f g   skew %+.2f", l.rms, l.threshold, l.skew);
    drawRow(0, BAR_H + PLOT_H, a, b, th->fg);
    snprintf(a, sizeof(a), "rate %.1f spm   strokes %lu   check %.0f mg   bounce %.0f mg", l.rate,
             (unsigned long)l.strokes, v.checkMg, v.bounceMg);
    snprintf(b, sizeof(b), "GPS %.2f m/s   fused %s%.2f m/s   log drops %lu", v.speed,
             l.fusionActive ? "" : "(off) ", l.fusedSpeed, (unsigned long)logger::dropped());
    drawRow(1, BAR_H + PLOT_H + ROW_H, a, b, th->fg);
}

// ---- settings screen -------------------------------------------------------

enum SettingRow { S_K, S_FLOOR, S_MINIMA, S_RAW, S_FUSION, S_RATIO, S_CAM_MIRROR, S_THEME, S_FLIP, S_BRIGHT, S_HR, S_COUNT };
constexpr int SET_TAB_H = 72, SET_ROW_H = 90, SET_ROWS = 4;
constexpr int SET_ICON = 74, SET_PLUS_X = W - 20 - SET_ICON, SET_MINUS_X = SET_PLUS_X - 16 - SET_ICON;
constexpr int SET_SWITCH_W = 130, SET_SWITCH_H = 60, SET_TEXT_BTN_W = 190;

struct SettingsTab {
    const char* name;
    int rows[SET_ROWS];  // -1 = empty slot
};
constexpr SettingsTab SET_TABS[] = {
    {"STROKE", {S_K, S_FLOOR, S_MINIMA, S_RATIO}},
    {"DISPLAY", {S_THEME, S_BRIGHT, S_FLIP, S_CAM_MIRROR}},
    {"SENSORS", {S_HR, S_FUSION, S_RAW, -1}},
};
constexpr int SET_TAB_COUNT = sizeof(SET_TABS) / sizeof(SET_TABS[0]);
int settingsTab = 0;
Cache setTabCache, setRowCache[SET_ROWS];

bool isNumeric(int row) { return row == S_K || row == S_FLOOR || row == S_RATIO || row == S_BRIGHT; }

// On/off rows whose button shows the state itself (lit when on).
int switchState(int row) {
    const Settings& s = settings::get();
    switch (row) {
        case S_MINIMA: return s.useMinimaThreshold;
        case S_RAW: return s.rawLog;
        case S_FUSION: return s.fusion;
        case S_CAM_MIRROR: return s.camMirror;
        case S_FLIP: return s.flip;
        default: return -1;
    }
}

void settingText(int row, char* label, size_t ln, char* value, size_t vn) {
    const Settings& s = settings::get();
    switch (row) {
        case S_K: snprintf(label, ln, "Catch threshold factor (x RMS)"); snprintf(value, vn, "%.1f", s.thresholdK); break;
        case S_FLOOR: snprintf(label, ln, "Catch threshold floor"); snprintf(value, vn, "%.2f g", s.thresholdFloorG); break;
        case S_MINIMA: snprintf(label, ln, "Threshold from last 5 catch depths"); snprintf(value, vn, "%s", s.useMinimaThreshold ? "ON" : "OFF"); break;
        case S_RAW: snprintf(label, ln, "Raw IMU logging (100 Hz)"); snprintf(value, vn, "%s", s.rawLog ? "ON" : "OFF"); break;
        case S_FUSION: snprintf(label, ln, "GPS+IMU fusion for distance/stroke"); snprintf(value, vn, "%s", s.fusion ? "ON" : "OFF"); break;
        case S_RATIO: snprintf(label, ln, "Rec:drive target shift (2.5 at 20 spm)"); snprintf(value, vn, "%+.1f", s.ratioShift); break;
        case S_CAM_MIRROR: snprintf(label, ln, "Camera mirror image (like a mirror)"); snprintf(value, vn, "%s", s.camMirror ? "ON" : "OFF"); break;
        case S_THEME: snprintf(label, ln, "Theme"); snprintf(value, vn, "%s", s.dayTheme ? "DAY" : "NIGHT"); break;
        case S_FLIP: snprintf(label, ln, "Rotate screen 180"); snprintf(value, vn, "%s", s.flip ? "ON" : "OFF"); break;
        case S_BRIGHT: snprintf(label, ln, "Brightness"); snprintf(value, vn, "%d%%", s.brightness * 100 / 255); break;
        case S_HR:
            snprintf(label, ln, "Heart rate sensor (Bluetooth)");
            if (hr::pairing()) snprintf(value, vn, "SCAN...");
            else if (hr::hasSaved()) snprintf(value, vn, "%s", hr::get().name);
            else snprintf(value, vn, "NONE");
            break;
    }
}

const char* settingButton(int row) {
    if (row == S_HR) {
        if (hr::pairing()) return "...";
        return hr::hasSaved() ? "FORGET" : "PAIR";
    }
    if (row == S_THEME) return settings::get().dayTheme ? "DAY" : "NIGHT";
    return switchState(row) == 1 ? "ON" : "OFF";
}

void drawSettingsTabs() {
    char key[16];
    snprintf(key, sizeof(key), "tab%d", settingsTab);
    if (!changed(setTabCache, key)) return;
    M5Canvas& c = setTabC;
    c.fillSprite(th->bg);
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextSize(1);
    c.setTextDatum(middle_center);
    int tw = W / SET_TAB_COUNT;
    for (int i = 0; i < SET_TAB_COUNT; i++) {
        bool on = i == settingsTab;
        c.fillRoundRect(i * tw + 8, 8, tw - 16, SET_TAB_H - 16, 14, on ? th->accent : th->btn);
        c.setTextColor(on ? th->bg : th->btnText);
        c.drawString(SET_TABS[i].name, i * tw + tw / 2, SET_TAB_H / 2);
    }
    c.drawFastHLine(0, SET_TAB_H - 1, W, th->dim);
    fb::push(c, 0, BAR_H);
}

void drawSettingRow(int slot, int r) {
    char label[48], value[24], key[96];
    if (r < 0) {
        label[0] = value[0] = 0;
        snprintf(key, sizeof(key), "empty");
    } else {
        settingText(r, label, sizeof(label), value, sizeof(value));
        snprintf(key, sizeof(key), "%d|%s|%s|%s", r, label, value, settingButton(r));
    }
    if (!changed(setRowCache[slot], key)) return;
    M5Canvas& c = setC;
    c.fillSprite(th->bg);
    if (r >= 0) {
        int mid = SET_ROW_H / 2, by = (SET_ROW_H - SET_ICON) / 2;
        c.drawFastHLine(0, SET_ROW_H - 1, W, th->dim);
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextSize(1);
        c.setTextDatum(middle_left);
        c.setTextColor(th->fg);
        c.drawString(label, 24, mid);
        c.setTextDatum(middle_right);
        c.setTextColor(th->accent);
        auto iconButton = [&](int x, icons::Icon icon) {
            c.fillSmoothRoundRect(x, by, SET_ICON, SET_ICON, 16, th->btn);
            icons::draw(c, icon, x + SET_ICON / 2, by + SET_ICON / 2, 34, th->btnText, th->btn);
        };
        if (isNumeric(r)) {
            c.drawString(value, SET_MINUS_X - 24, mid);
            iconButton(SET_MINUS_X, icons::Icon::Minus);
            iconButton(SET_PLUS_X, icons::Icon::Plus);
        } else if (r == S_THEME) {
            bool day = settings::get().dayTheme;
            c.drawString(day ? "Day" : "Night", SET_PLUS_X - 24, mid);
            iconButton(SET_PLUS_X, day ? icons::Icon::Sun : icons::Icon::Moon);
        } else if (r == S_HR) {
            int bx = W - 20 - SET_TEXT_BTN_W;
            c.drawString(value, bx - 24, mid);
            c.fillSmoothRoundRect(bx, by, SET_TEXT_BTN_W, SET_ICON, 16, th->btn);
            c.setTextDatum(middle_center);
            c.setTextColor(th->btnText);
            c.drawString(settingButton(r), bx + SET_TEXT_BTN_W / 2, mid);
        } else {
            icons::drawSwitch(c, W - 20 - SET_SWITCH_W, (SET_ROW_H - SET_SWITCH_H) / 2, SET_SWITCH_W, SET_SWITCH_H,
                              switchState(r) == 1, th->accent, th->btn, th->bg);
        }
    }
    fb::push(c, 0, BAR_H + SET_TAB_H + slot * SET_ROW_H);
}

void drawSettings() {
    drawSettingsTabs();
    for (int i = 0; i < SET_ROWS; i++) drawSettingRow(i, SET_TABS[settingsTab].rows[i]);

    char info[96];
    snprintf(info, sizeof(info), "SD card: %s   log drops: %lu", logger::sdReady() ? "OK" : "not found",
             (unsigned long)logger::dropped());
    drawRow(S_COUNT, BTN_Y - ROW_H, info,
#if SIM_MODE
            "build: SIMULATION",
#else
            "build: hardware",
#endif
            th->label);
}

void applySettings(bool full);

void onSettingsTap(int x, int y) {
    if (y >= BAR_H && y < BAR_H + SET_TAB_H) {
        settingsTab = min(SET_TAB_COUNT - 1, x / (W / SET_TAB_COUNT));
        return;
    }
    int slot = (y - BAR_H - SET_TAB_H) / SET_ROW_H;
    if (y < BAR_H + SET_TAB_H || slot >= SET_ROWS) return;
    int row = SET_TABS[settingsTab].rows[slot];
    if (row < 0) return;
    bool minus = x >= SET_MINUS_X - 8 && x < SET_MINUS_X + SET_ICON + 8;
    bool plus = x >= SET_PLUS_X - 8;
    bool toggle = x >= W / 2;  // the whole right half of a switch row
    Settings& s = settings::get();
    bool full = false;
    switch (row) {
        case S_K:
            if (minus) s.thresholdK = fmaxf(1.0f, s.thresholdK - 0.1f);
            if (plus) s.thresholdK = fminf(3.5f, s.thresholdK + 0.1f);
            break;
        case S_FLOOR:
            if (minus) s.thresholdFloorG = fmaxf(0.04f, s.thresholdFloorG - 0.01f);
            if (plus) s.thresholdFloorG = fminf(0.40f, s.thresholdFloorG + 0.01f);
            break;
        case S_RATIO:
            if (minus) s.ratioShift = fmaxf(-0.8f, roundf((s.ratioShift - 0.1f) * 10) / 10);
            if (plus) s.ratioShift = fminf(0.8f, roundf((s.ratioShift + 0.1f) * 10) / 10);
            break;
        case S_BRIGHT:
            if (minus) s.brightness = (uint8_t)max(20, s.brightness - 25);
            if (plus) s.brightness = (uint8_t)min(255, s.brightness + 25);
            break;
        case S_MINIMA: if (toggle) s.useMinimaThreshold = !s.useMinimaThreshold; break;
        case S_RAW: if (toggle) s.rawLog = !s.rawLog; break;
        case S_FUSION: if (toggle) s.fusion = !s.fusion; break;
        case S_CAM_MIRROR: if (toggle) s.camMirror = !s.camMirror; break;
        case S_THEME: if (toggle) { s.dayTheme = !s.dayTheme; full = true; } break;
        case S_FLIP: if (toggle) { s.flip = !s.flip; full = true; } break;
        case S_HR:
            if (toggle && !hr::pairing()) {
                if (hr::hasSaved()) hr::forget();
                else hr::startPairing();
            }
            return;
    }
    if (minus || plus || toggle) {
        settings::save();
        applySettings(full);
    }
}

// ---- camera & map content --------------------------------------------------

void clearScreen();

PipMode pipMode() { return (PipMode)settings::get().pipMode; }

bool cameraVisible() { return screen == Screen::Main && pipMode() == PipCamera; }
bool mapOnMain() { return screen == Screen::Main && pipMode() == PipMap; }

// Rectangle of the Main-screen corner content (camera or PIP map).
void contentRect(int& x, int& y, int& w, int& h) {
    if (settings::get().camFull) {
        x = 0, y = FULL_CAM_Y, w = W, h = FULL_CAM_H;
    } else {
        x = 0, y = BAR_H, w = PIP_W, h = PIP_H;
    }
}

bool inContentRect(int px, int py) {
    if (!(cameraVisible() || mapOnMain())) return false;
    int x, y, w, h;
    contentRect(x, y, w, h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

void updateCamera() {
    camera::setActive(cameraVisible());
    camShown = camera::Status::Off;
}

void drawCamera() {
    if (!cameraVisible()) return;
    int x, y, w, h;
    contentRect(x, y, w, h);
    camera::Status st = camera::status();
    if (st != camera::Status::Running && (st != camShown || forceRedraw)) {
        M5.Display.fillRect(x, y, w, h, TFT_BLACK);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextSize(1);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setTextColor(st == camera::Status::Failed ? TFT_RED : TFT_WHITE, TFT_BLACK);
        M5.Display.drawString(st == camera::Status::Failed ? "Camera not available" : "Camera starting...",
                              x + w / 2, y + h / 2);
    }
    camShown = st;
    const Settings& s = settings::get();
    // The sensor is fixed to the case, so a flipped display needs a flipped image.
    camera::draw(x, y, w, h, s.camMirror, s.flip != cfg::CAM_ROTATE_180);
}

// The map canvas and its screen position for the current layout, or nullptr.
M5Canvas* mapCanvasFor(int& sx, int& sy, bool& full) {
    if (screen == Screen::Map) {
        sx = 0, sy = FULL_CAM_Y, full = true;
        return &mapC;
    }
    if (mapOnMain()) {
        if (settings::get().camFull) {
            sx = 0, sy = FULL_CAM_Y, full = true;
            return &mapC;
        }
        sx = 0, sy = BAR_H, full = false;
        return &mapPipC;
    }
    return nullptr;
}

bool mapFullShown() { return screen == Screen::Map || (mapOnMain() && settings::get().camFull); }

// Downloads are refused while recording, so the Maps screen is only offered between sessions.
bool mapsButtonShown() { return screen == Screen::Map && metrics::view().state != metrics::State::Running; }

// A full map frame takes longer than a UI frame, so it is drawn by its own task on the other
// core and only pushed from here; the timer and metrics keep updating meanwhile.
enum class MapJob : uint8_t { Idle, Drawing, Done };
std::atomic<MapJob> mapJob{MapJob::Idle};
TaskHandle_t mapTask = nullptr;
M5Canvas* mapJobCanvas = nullptr;
mapview::View mapJobView;
uint32_t mapJobLayout = 0;  // layoutGen when the job was started
uint32_t layoutGen = 0;     // bumped by every full redraw; older frames are dropped
bool mapRedrawPending = false;
bool mapFollow = true;
double mapCenterLat = 0, mapCenterLon = 0;
double mapPanLat0 = 0, mapPanLon0 = 0;
int mapPanX0 = 0, mapPanY0 = 0;
float mapPanMpp = 1, mapPanCourse = 0;
uint8_t mapPanOrientation = mapview::NorthUp;
enum class DragKind : uint8_t { None, Lists, Map } dragKind = DragKind::None;

void mapTaskRun(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        mapview::render(*mapJobCanvas, mapJobView);
        mapJob = MapJob::Done;
    }
}

void startMapTask() {
    xTaskCreatePinnedToCore(mapTaskRun, "map", 8192, nullptr, 1, &mapTask, 0);
}

void pushMap(M5Canvas* cv, int sx, int sy) {
    if (mapsButtonShown()) {
        cv->fillRoundRect(MAPS_BTN_X, MAPS_BTN_Y, MAPS_BTN_W, MAPS_BTN_H, 10, TFT_WHITE);
        cv->drawRoundRect(MAPS_BTN_X, MAPS_BTN_Y, MAPS_BTN_W, MAPS_BTN_H, 10, TFT_DARKGREY);
        cv->setFont(&fonts::FreeSansBold12pt7b);
        cv->setTextSize(1);
        cv->setTextColor(TFT_BLACK);
        cv->setTextDatum(middle_center);
        cv->drawString("MAPS", MAPS_BTN_X + MAPS_BTN_W / 2, MAPS_BTN_Y + MAPS_BTN_H / 2);
    }
    fb::push(*cv, sx, sy);
}

void drawMapContent() {
    if (forceRedraw) {
        layoutGen++;
        mapRedrawPending = true;
    }
    int sx, sy;
    bool full;
    M5Canvas* cv = mapCanvasFor(sx, sy, full);

    if (mapJob == MapJob::Done) {
        bool current = cv && cv == mapJobCanvas && mapJobLayout == layoutGen;
        mapJob = MapJob::Idle;
        // The gap before the next frame starts now, so the map task never hogs its core.
        lastMap = millis();
        if (current) pushMap(cv, sx, sy);
    }
    if (!cv || mapJob != MapJob::Idle) return;
    if (!mapRedrawPending && millis() - lastMap < cfg::MAP_FRAME_MS) return;
    mapRedrawPending = false;

    const Settings& s = settings::get();
    gps::Snapshot g = gps::get();
    mapview::View mv;
    mv.lat = g.lat;
    mv.lon = g.lon;
    mv.course = g.course;
    mv.haveFix = g.fix;
    mv.orientation = s.mapOrientation;
    mv.showControls = full;
    mv.largeControls = full && metrics::view().state == metrics::State::Running;
    if (!full) mapFollow = true;
    mv.followBoat = mapFollow;
    mv.centerLat = mapCenterLat;
    mv.centerLon = mapCenterLon;
    mv.showRecenter = full && !mapFollow;
    int zi = s.mapZoom < cfg::MAP_ZOOM_COUNT ? s.mapZoom : cfg::MAP_ZOOM_DEFAULT;
    mv.metersPerPixel = cfg::MAP_ZOOM_M[zi] / (float)cv->width();
    mapJobCanvas = cv;
    mapJobView = mv;
    mapJobLayout = layoutGen;
    mapJob = MapJob::Drawing;
    xTaskNotifyGive(mapTask);
}

void cyclePip() {
    Settings& s = settings::get();
    s.pipMode = (uint8_t)((s.pipMode + 1) % 3);
    settings::save();
    if (s.pipMode != PipMap || !s.camFull) mapFollow = true;
    clearScreen();
    updateCamera();
}

void togglePipFull() {
    Settings& s = settings::get();
    s.camFull = !s.camFull;
    settings::save();
    if (!s.camFull) mapFollow = true;
    clearScreen();
    updateCamera();
}

void recenterMap() {
    mapFollow = true;
    mapRedrawPending = true;
    forceRedraw = true;
}

void boatLatLon(double& lat, double& lon) {
    gps::Snapshot g = gps::get();
    if (g.fix) {
        lat = g.lat;
        lon = g.lon;
        return;
    }
    if (!mapdata::centerLatLon(lat, lon)) lat = lon = 0;
}

bool mapControlHit(int px, int py) {
    if (!mapFullShown()) return false;
    bool large = metrics::view().state == metrics::State::Running;
    int bx, by, bw, bh;
    auto hit = [&](int x, int y, int w, int h) {
        return px >= x && px < x + w && py >= FULL_CAM_Y + y && py < FULL_CAM_Y + y + h;
    };
    if (mapview::zoomInRect(mapC, bx, by, bw, bh, large) && hit(bx, by, bw, bh)) return true;
    if (mapview::zoomOutRect(mapC, bx, by, bw, bh, large) && hit(bx, by, bw, bh)) return true;
    if (mapview::orientationRect(mapC, bx, by, bw, bh, large) && hit(bx, by, bw, bh)) return true;
    if (!mapFollow && mapview::recenterRect(mapC, bx, by, bw, bh, large) && hit(bx, by, bw, bh)) return true;
    if (mapsButtonShown() && px >= MAPS_BTN_X && px < MAPS_BTN_X + MAPS_BTN_W &&
        py >= FULL_CAM_Y + MAPS_BTN_Y && py < FULL_CAM_Y + MAPS_BTN_Y + MAPS_BTN_H)
        return true;
    return false;
}

void cycleOrientation() {
    Settings& s = settings::get();
    s.mapOrientation = (uint8_t)((s.mapOrientation + 1) % mapview::ORIENTATION_COUNT);
    settings::save();
    forceRedraw = true;
}

void adjustZoom(int delta) {
    Settings& s = settings::get();
    int z = (int)s.mapZoom + delta;
    if (z < 0) z = 0;
    if (z >= cfg::MAP_ZOOM_COUNT) z = cfg::MAP_ZOOM_COUNT - 1;
    if (z != s.mapZoom) {
        s.mapZoom = (uint8_t)z;
        settings::save();
        forceRedraw = true;
    }
}

// If the tap hit an on-map zoom or orientation button, handle it. Returns true if consumed.
bool handleMapControlTap(int px, int py) {
    if (!mapFullShown()) return false;
    bool large = metrics::view().state == metrics::State::Running;
    int bx, by, bw, bh;
    auto hit = [&](int x, int y, int w, int h) {
        return px >= x && px < x + w && py >= FULL_CAM_Y + y && py < FULL_CAM_Y + y + h;
    };
    if (mapview::zoomInRect(mapC, bx, by, bw, bh, large) && hit(bx, by, bw, bh)) {
        adjustZoom(-1);  // smaller span = closer in
        return true;
    }
    if (mapview::zoomOutRect(mapC, bx, by, bw, bh, large) && hit(bx, by, bw, bh)) {
        adjustZoom(+1);  // larger span = further out
        return true;
    }
    if (mapview::orientationRect(mapC, bx, by, bw, bh, large) && hit(bx, by, bw, bh)) {
        cycleOrientation();
        return true;
    }
    if (!mapFollow && mapview::recenterRect(mapC, bx, by, bw, bh, large) && hit(bx, by, bw, bh)) {
        recenterMap();
        return true;
    }
    return false;
}

// ---- input -----------------------------------------------------------------

void clearScreen() {
    M5.Display.fillScreen(th->bg);
    forceRedraw = true;
}

void setScreen(Screen next) {
    if (next == screen) return;
    if (screen == Screen::Maps) {
        mapsui::leave();
        // Wi-Fi is only for downloading maps; keep it while a download or the setup page runs.
        if (!mapjob::busy() && !net::setupActive()) net::disconnect();
    }
    if (screen == Screen::Sessions) sessionsui::leave();
    screen = next;
    if (screen == Screen::Sessions) sessionsui::enter();
    if (screen == Screen::Maps) {
        mapsui::enter();
        if (net::state() == net::State::Off && net::hasStoredNetwork()) net::connect(nullptr, nullptr, Serial);
    }
    dragging = false;
    if (next != Screen::Map) mapFollow = true;
    clearScreen();
    updateCamera();
}

void nextScreen() {
    setScreen(screen == Screen::Maps ? Screen::Map : (Screen)(((int)screen + 1) % CYCLE));
}

void onWifiButton() {
    if (net::setupActive()) {
        net::stopSetup();
    } else if (net::connected() || net::state() == net::State::Connecting) {
        net::disconnect();
    } else if (net::hasStoredNetwork()) {
        net::connect(nullptr, nullptr, Serial);
    } else {
        net::startSetup(Serial);
    }
    forceRedraw = true;
}

void onMapsTap(int x, int y) {
    if (y < BTN_Y) {
        mapsui::tap(x, y);
        return;
    }
    switch (x / BTN_W) {
        case 0: mapsui::setView(mapsui::View::Library); break;
        case 1: mapsui::setView(mapsui::View::Draw); break;
        case 2: mapsui::setView(mapsui::View::OnCard); break;
        case 3: onWifiButton(); break;
        case 4: nextScreen(); break;
    }
    forceRedraw = true;
}

void onTap(int x, int y) {
    using metrics::State;
    if (screen == Screen::Maps) {
        onMapsTap(x, y);
        return;
    }
    if (screen == Screen::Sessions && y < BTN_Y) {
        sessionsui::tap(x, y);
        forceRedraw = true;
        return;
    }
    metrics::View v = metrics::view();
    if (screen == Screen::Map && mapsButtonShown() && x >= MAPS_BTN_X && x < MAPS_BTN_X + MAPS_BTN_W &&
        y >= FULL_CAM_Y + MAPS_BTN_Y && y < FULL_CAM_Y + MAPS_BTN_Y + MAPS_BTN_H) {
        setScreen(Screen::Maps);
        return;
    }
    if (y >= BTN_Y) {
        switch (x / BTN_W) {
            case 0:
                if (v.state == State::Running) showHint("Hold to stop");
                else metrics::start();
                break;
            case 1: metrics::lap(); break;
            case 2:
                if (v.state == State::Stopped) showHint("Hold to reset");
                break;
            case 3:
                if (screen == Screen::Main) cyclePip();
                else if (screen == Screen::Map) cycleOrientation();
                else if (screen == Screen::Efficiency) showHint("Hold to clear the chart and your best line");
                else if (screen == Screen::Sessions) {
                    sessionsui::back();
                    forceRedraw = true;
                }
                break;
            case 4: nextScreen(); break;
        }
        return;
    }
    if (handleMapControlTap(x, y)) return;
    if (screen == Screen::Map) return;  // full map: controls and drag, not a tap action
    if (inContentRect(x, y)) togglePipFull();
    if (screen == Screen::Settings) onSettingsTap(x, y);
}

void onHold(int x, int y) {
    if (y < BTN_Y) return;
    int idx = x / BTN_W;
    if (screen == Screen::Maps) {
        // Hold WIFI to set up a different network.
        if (idx == 3 && !net::setupActive()) {
            net::startSetup(Serial);
            forceRedraw = true;
        }
        return;
    }
    if (idx == 0) metrics::stop();
    if (idx == 2) metrics::reset();
    if (idx == 3 && screen == Screen::Efficiency) {
        efficiency::reset();
        forceRedraw = true;
    }
}

void applySettings(bool full) {
    const Settings& s = settings::get();
    imu::setParams(s.strokeParams());
    imu::setFusionEnabled(s.fusion);
    logger::setRawEnabled(s.rawLog);
    {
        board::I2CLock lock;
        M5.Display.setBrightness(s.brightness);
    }
    th = s.dayTheme ? &DAY : &NIGHT;
    Palette pal{th->bg, th->fg, th->label, th->accent, th->dim, th->btn, th->btnText, th->warn, th->ok};
    mapsui::setColors(pal);
    sessionsui::setColors(pal);
    if (full) {
        M5.Display.setRotation(s.flip ? 1 : 3);
        clearScreen();
    }
    updateCamera();
}

void dragStart(int x, int y) {
    dragKind = DragKind::None;
    if (screen == Screen::Maps) {
        dragKind = DragKind::Lists;
        mapsui::dragStart(x, y);
    } else if (screen == Screen::Sessions) {
        dragKind = DragKind::Lists;
        sessionsui::dragStart(x, y);
    } else if (mapFullShown() && y >= FULL_CAM_Y && y < BTN_Y && !mapControlHit(x, y)) {
        dragKind = DragKind::Map;
        mapPanX0 = x;
        mapPanY0 = y;
        if (mapFollow) boatLatLon(mapPanLat0, mapPanLon0);
        else {
            mapPanLat0 = mapCenterLat;
            mapPanLon0 = mapCenterLon;
        }
        const Settings& s = settings::get();
        M5Canvas* cv = &mapC;
        int zi = s.mapZoom < cfg::MAP_ZOOM_COUNT ? s.mapZoom : cfg::MAP_ZOOM_DEFAULT;
        mapPanMpp = cfg::MAP_ZOOM_M[zi] / (float)cv->width();
        mapPanOrientation = s.mapOrientation;
        mapPanCourse = gps::get().course;
    }
}
void dragMove(int x, int y) {
    if (dragKind == DragKind::Lists) {
        if (screen == Screen::Maps) mapsui::dragMove(x, y);
        else sessionsui::dragMove(x, y);
        return;
    }
    if (dragKind != DragKind::Map) return;
    mapFollow = false;
    mapCenterLat = mapPanLat0;
    mapCenterLon = mapPanLon0;
    mapview::panCenter(mapCenterLat, mapCenterLon, (float)(x - mapPanX0), (float)(y - mapPanY0), mapPanMpp,
                       mapPanOrientation, mapPanCourse);
    mapRedrawPending = true;
    lastMap = 0;
}
void dragEnd(int x, int y) {
    if (dragKind == DragKind::Lists) {
        if (screen == Screen::Maps) mapsui::dragEnd(x, y);
        else sessionsui::dragEnd(x, y);
    } else if (dragKind == DragKind::Map) {
        dragMove(x, y);
    }
    dragKind = DragKind::None;
}

void handleDrag(const m5::touch_detail_t& t) {
    if (screen != Screen::Maps && screen != Screen::Sessions && !mapFullShown()) return;
    if (t.isPressed()) {
        if (!dragging && (abs(t.distanceX()) > 12 || abs(t.distanceY()) > 12)) {
            dragging = true;
            dragStart(t.base_x, t.base_y);
        }
        if (dragging) dragMove(t.x, t.y);
    } else if (dragging) {
        dragging = false;
        dragEnd(t.x, t.y);
    }
}

}  // namespace

void begin() {
    const Settings& s = settings::get();
    th = s.dayTheme ? &DAY : &NIGHT;
    M5.Display.setRotation(s.flip ? 1 : 3);
    Serial.printf("[ui] display %d x %d\n", M5.Display.width(), M5.Display.height());
    M5.Touch.setHoldThresh(cfg::TOUCH_HOLD_MS);

    for (M5Canvas* c : {&bigC, &smallC, &barC, &rowC, &plotC, &btnC, &setC, &setTabC, &mapC, &mapPipC, &effC, &effP}) {
        c->setColorDepth(16);
        c->setPsram(true);
    }
    bigC.createSprite(BIG_W, BIG_H);
    smallC.createSprite(SMALL_W, SMALL_H);
    barC.createSprite(W, BAR_H);
    rowC.createSprite(W, ROW_H);
    plotC.createSprite(W, PLOT_H);
    btnC.createSprite(BTN_W, BTN_H);
    setC.createSprite(W, SET_ROW_H);
    setTabC.createSprite(W, SET_TAB_H);
    mapC.createSprite(W, FULL_CAM_H);
    mapPipC.createSprite(PIP_W, PIP_H);
    effC.createSprite(EFF_CHART_W, EFF_H);
    effP.createSprite(EFF_PANEL_W, EFF_H);

    camera::begin();
    mapview::begin();
    startMapTask();
    mapsui::begin(0, BAR_H, W, BTN_Y - BAR_H);
    sessionsui::begin(0, BAR_H, W, BTN_Y - BAR_H);
    applySettings(false);
    clearScreen();
}

void tap(int x, int y) { onTap(x, y); }
void hold(int x, int y) { onHold(x, y); }

void drag(int x0, int y0, int x1, int y1) {
    if (screen != Screen::Maps && screen != Screen::Sessions && !mapFullShown()) return;
    dragStart(x0, y0);
    for (int i = 1; i <= 8; i++) dragMove(x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8);
    dragEnd(x1, y1);
}

void command(char c) {
    switch (c) {
        case 'N': nextScreen(); break;
        case 'M':
            if (metrics::view().state != metrics::State::Running) setScreen(Screen::Maps);
            break;
        case 'C':
            if (screen == Screen::Main) cyclePip();
            break;
        case 'V':
            if (cameraVisible() || mapOnMain()) togglePipFull();
            break;
        case 'H':
            if (mapFullShown() || mapOnMain()) cycleOrientation();
            break;
        case '+':
            if (mapFullShown() || mapOnMain()) adjustZoom(-1);  // zoom in
            break;
        case '-':
            if (mapFullShown() || mapOnMain()) adjustZoom(+1);  // zoom out
            break;
        case 'G': metrics::start(); break;
        case 'X': metrics::stop(); break;
        case 'L': metrics::lap(); break;
        case 'R': metrics::reset(); break;
    }
}

void loop() {
    static bool wasPairing = false;
    bool pairingNow = hr::pairing();
    if (wasPairing && !pairingNow) {
        char text[48];
        if (hr::hasSaved()) snprintf(text, sizeof(text), "Heart rate: %s", hr::get().name);
        else snprintf(text, sizeof(text), "No heart rate sensor found");
        showHint(text);
    }
    wasPairing = pairingNow;

    auto t = M5.Touch.getDetail();
    handleDrag(t);
    if (t.wasClicked() && !dragging) onTap(t.x, t.y);
    else if (t.wasHold() && !dragging) onHold(t.x, t.y);

    // A finished download: rescan the card and show the new map.
    if (mapjob::takeCompleted()) {
        char path[48];
        mapjob::pathFor(mapjob::status().name, path, sizeof(path));
        mapdata::begin();
        mapdata::show(path);
        forceRedraw = true;
    }

    if (millis() - lastBattery > 5000 || lastBattery == 0) {
        lastBattery = millis();
        int level;
        {
            board::I2CLock lock;
            level = M5.Power.getBatteryLevel();
        }
        metrics::setBattery(level);
    }

    // Keep the loaded map area in sync with position, and record the track.
    if (millis() - lastMapScan > 1000 && !sessionsui::reviewing()) {
        lastMapScan = millis();
        gps::Snapshot g = gps::get();
        mapdata::update(g.lat, g.lon, g.fix);
    }
    track::update();

    drawCamera();
    drawMapContent();
    if (screen == Screen::Maps) mapsui::draw(forceRedraw);
    if (screen == Screen::Sessions) sessionsui::draw(forceRedraw);
    if (millis() - lastFrame < cfg::UI_FRAME_MS && !forceRedraw) return;
    lastFrame = millis();

    metrics::View v = metrics::view();
    drawTopBar(v);
    switch (screen) {
        case Screen::Main: drawMain(v); break;
        case Screen::Map: drawTopTiles(v); break;
        case Screen::Efficiency: drawEfficiency(v); break;
        case Screen::Plot: drawPlot(v); break;
        case Screen::Settings: drawSettings(); break;
        case Screen::Sessions:
        case Screen::Maps: break;
    }
    drawButtons(v);
    forceRedraw = false;
}

}  // namespace ui
