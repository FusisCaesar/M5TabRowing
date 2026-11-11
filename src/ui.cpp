#include "ui.h"

#include <M5Unified.h>

#include <atomic>
#include <cstring>
#include <esp_heap_caps.h>

#include "board.h"
#include "camera.h"
#include "display_fb.h"
#include "config.h"
#include "efficiency.h"
#include "gps_nav.h"
#include "hr.h"
#include "icons.h"
#include "keyboard.h"
#include "imu_stroke.h"
#include "logger.h"
#include "map_data.h"
#include "map_job.h"
#include "map_view.h"
#include "maps_ui.h"
#include "net.h"
#include "metrics.h"
#include "rtclock.h"
#include "segments.h"
#include "sessions_ui.h"
#include "settings.h"
#include "stroke_anatomy.h"
#include "targets.h"
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

// The NEXT button cycles through the first CYCLE screens. Maps opens from the Map
// screen; GPS diagnostics opens from the top bar; the target and segment lists
// open from the map. None of those is part of the cycle.
// Screens from Sessions on draw their own content area (sessions_ui, maps_ui).
enum class Screen { Main, Map, Efficiency, Stroke, Plot, Settings, Sessions, Maps, Gps, Targets, Segments };
constexpr int CYCLE = 7;
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
M5Canvas wifiC(&M5.Display);
M5Canvas mapC(&M5.Display);     // full-size map area (Map screen or full-on-Main)
M5Canvas mapPipC(&M5.Display);  // picture-in-picture map
M5Canvas effC(&M5.Display);     // efficiency chart
M5Canvas effP(&M5.Display);     // efficiency score and technique panel

Screen screen = Screen::Main;
Screen gpsBack = Screen::Main;
Screen tgtBack = Screen::Main;
Screen segBack = Screen::Main;
const Theme* th = &NIGHT;
bool forceRedraw = true;
camera::Status camShown = camera::Status::Off;
uint32_t lastMap = 0;
uint32_t lastMapScan = 0;
bool dragging = false;
// On-map button that opens the Maps screen (Map screen, no session running).
constexpr int MAPS_BTN_X = 16, MAPS_BTN_Y = 16, MAPS_BTN_W = 170, MAPS_BTN_H = 64;

struct Cache {
    char text[120];
};
Cache tileCache[TILE_SLOTS], btnCache[BTN_COUNT], barCache, rowCache[12];

char hint[48] = "";
uint32_t hintUntil = 0;

// A piece started by the start line. The finish line stops it and opens the result.
struct RaceResult {
    bool open = false;
    int segment = -1;
    bool live = false;
    char name[48] = "";
    char saved[16] = "";
    float course = 0;
    float elapsed = 0;
    float distance = 0;
    float avgSplit = 0;
    float avgRate = 0;
    int strokes = 0;
    float avgHr = 0;
    int maxHr = 0;
} race;
uint32_t lastFrame = 0;
uint32_t lastPlot = 0;
uint32_t lastEff = 0, effVersion = 0;
uint32_t lastStroke = 0, strokeVersion = 0;
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
              bool big, uint16_t color, int unitColor = -1);

void fillMetric(TileMetric m, const metrics::View& v, char* label, char* unit, char* value, size_t vn,
                uint16_t& color, int& unitColor) {
    color = th->fg;
    unitColor = -1;
    label[0] = unit[0] = value[0] = '\0';
    switch (m) {
        case TileMetric::Split:
            strcpy(label, "SPLIT");
            strcpy(unit, "/500m");
            fmtSplit(value, vn, v.split);
            break;
        case TileMetric::Rate:
            strcpy(label, "RATE");
            strcpy(unit, "spm");
            color = th->accent;
            if (v.rate > 0) snprintf(value, vn, "%d", (int)lroundf(v.rate));
            else snprintf(value, vn, "--");
            break;
        case TileMetric::Distance:
            strcpy(label, "DISTANCE");
            strcpy(unit, "m");
            snprintf(value, vn, "%d", (int)v.distance);
            break;
        case TileMetric::Time:
            strcpy(label, "TIME");
            fmtTime(value, vn, v.elapsed);
            break;
        case TileMetric::DistStroke:
            strcpy(label, "DIST/STROKE");
            strcpy(unit, v.dpsFused ? "m*" : "m");
            if (v.dps > 0) snprintf(value, vn, "%.1f", v.dps);
            else snprintf(value, vn, "--");
            break;
        case TileMetric::AvgSplit:
            strcpy(label, "AVG SPLIT");
            strcpy(unit, "/500m");
            fmtSplit(value, vn, v.avgSplit);
            break;
        case TileMetric::AvgRate:
            strcpy(label, "AVG RATE");
            strcpy(unit, "spm");
            if (v.avgRate > 0) snprintf(value, vn, "%.1f", v.avgRate);
            else snprintf(value, vn, "--");
            break;
        case TileMetric::Strokes:
            strcpy(label, "STROKES");
            snprintf(value, vn, "%d", v.strokes);
            break;
        case TileMetric::HeartRate:
            strcpy(label, "HEART RATE");
            strcpy(unit, "bpm");
            if (v.hr > 0) snprintf(value, vn, "%d", v.hr);
            else snprintf(value, vn, "--");
            color = v.hr > 0 ? th->fg : th->warn;
            break;
        case TileMetric::AvgHr:
            strcpy(label, "AVG HR");
            strcpy(unit, "bpm");
            if (v.avgHr > 0) snprintf(value, vn, "%.0f", v.avgHr);
            else snprintf(value, vn, "--");
            color = v.avgHr > 0 ? th->fg : th->warn;
            break;
        case TileMetric::RecDrive: {
            strcpy(label, "REC:DRIVE");
            efficiency::RatioCheck rc = efficiency::checkRatio(v.ratio, v.rate);
            strlcpy(unit, efficiency::adviceWord(rc.advice), 16);
            if (v.ratio > 0) snprintf(value, vn, "%.1f", v.ratio);
            else snprintf(value, vn, "--");
            color = adviceColor(rc);
            unitColor = (int)color;
            break;
        }
        case TileMetric::Speed:
            strcpy(label, "SPEED");
            strcpy(unit, "m/s");
            if (v.speed > 0.05f) snprintf(value, vn, "%.1f", v.speed);
            else snprintf(value, vn, "--");
            break;
        case TileMetric::Dtt:
        case TileMetric::Ttt:
        case TileMetric::Finish: {
            // A selected segment counts down to its start line, then along the points to the finish.
            bool course = segments::selected() >= 0;
            bool toFinish = !course || segments::onCourse();
            strcpy(label, m == TileMetric::Dtt ? (course ? (toFinish ? "DTF" : "DTS") : "DTT")
                               : m == TileMetric::Ttt ? (course ? (toFinish ? "TTF" : "TTS") : "TTT")
                                                       : (course && !toFinish ? "START" : "FINISH"));
            const char* name = course ? segments::selectedName() : targets::selectedName();
            if (name[0]) strlcpy(unit, name, 40);
            double blat = 0, blon = 0;
            gps::Snapshot g = gps::get();
            bool have = g.lat != 0 || g.lon != 0;
            if (have) {
                blat = g.lat;
                blon = g.lon;
            }
            float d = -1;
            if (have) {
                if (!course) d = targets::distanceFrom(blat, blon);
                else d = toFinish ? segments::remaining(blat, blon) : segments::distanceToStart(blat, blon);
            }
            if (d < 0) snprintf(value, vn, "--");
            else if (m == TileMetric::Dtt) snprintf(value, vn, "%d", (int)lroundf(d));
            else if (m == TileMetric::Ttt) {
                if (v.speed > cfg::GPS_SPEED_ZERO_MPS) fmtTime(value, vn, d / v.speed);
                else snprintf(value, vn, "--");
            } else if (v.avgSplit > 0) {
                fmtTime(value, vn, d * v.avgSplit / 500.0f);
            } else {
                snprintf(value, vn, "--");
            }
            break;
        }
        case TileMetric::Count:
        default:
            strcpy(label, "SPLIT");
            strcpy(unit, "/500m");
            fmtSplit(value, vn, v.split);
            break;
    }
}

void drawSlot(int slot, int x, int y, bool big, const metrics::View& v) {
    TileMetric m = (TileMetric)settings::get().tiles[slot];
    if ((uint8_t)m >= (uint8_t)TileMetric::Count) m = TileMetric::Split;
    char label[16], unit[40], value[24];
    uint16_t color;
    int unitColor;
    fillMetric(m, v, label, unit, value, sizeof(value), color, unitColor);
    char key[120];
    snprintf(key, sizeof(key), "%d|%s|%s|%s|%u", (int)m, label, unit, value, (unsigned)color);
    if (!changed(tileCache[slot], key)) return;
    drawTile(big ? bigC : smallC, x, y, label, unit, value, big, color, unitColor);
}

void cycleTile(int slot, int dir) {
    if (slot < 0 || slot >= TILE_SLOTS || dir == 0) return;
    int n = (int)TileMetric::Count;
    Settings& s = settings::get();
    s.tiles[slot] = (uint8_t)((s.tiles[slot] + dir + n) % n);
    settings::save();
    forceRedraw = true;
}

// Which of the eight slots is at (x, y), or -1. Same slot numbers in every layout.
// tileLeft / tileW are that slot's rectangle, so a tap can pick previous vs next.
int tileSlotAt(int x, int y, int* tileLeft = nullptr, int* tileW = nullptr) {
    auto hit = [&](int slot, int left, int width) {
        if (tileLeft) *tileLeft = left;
        if (tileW) *tileW = width;
        return slot;
    };
    if (y < BAR_H || y >= BTN_Y) return -1;
    const Settings& s = settings::get();
    bool topStrip = screen == Screen::Map || (screen == Screen::Main && s.pipMode != PipOff && s.camFull);
    if (topStrip) {
        if (y < BAR_H + SMALL_H) return hit(x / SMALL_W, (x / SMALL_W) * SMALL_W, SMALL_W);
        return -1;
    }
    if (screen != Screen::Main) return -1;
    if (s.pipMode != PipOff) {
        if (x < PIP_W && y >= BAR_H && y < BAR_H + PIP_H) return -1;
        if (x >= BIG_W && y >= BAR_H && y < BAR_H + BIG_H) return hit(0, BIG_W, BIG_W);
        if (x >= BIG_W && y >= BAR_H + BIG_H && y < BAR_H + 2 * BIG_H) return hit(1, BIG_W, BIG_W);
        if (y >= PIP_INFO_Y && y < PIP_INFO_Y + SMALL_H && x < PIP_W)
            return hit(2 + x / SMALL_W, (x / SMALL_W) * SMALL_W, SMALL_W);
        if (y >= SMALL_Y && y < SMALL_Y + SMALL_H)
            return hit(4 + x / SMALL_W, (x / SMALL_W) * SMALL_W, SMALL_W);
        return -1;
    }
    if (y < BAR_H + BIG_H) return hit(x / BIG_W, (x / BIG_W) * BIG_W, BIG_W);
    if (y < SMALL_Y) return hit(2 + x / BIG_W, (x / BIG_W) * BIG_W, BIG_W);
    if (y < SMALL_Y + SMALL_H) return hit(4 + x / SMALL_W, (x / SMALL_W) * SMALL_W, SMALL_W);
    return -1;
}

void drawTile(M5Canvas& c, int x, int y, const char* label, const char* unit, const char* value,
              bool big, uint16_t color, int unitColor) {
    c.fillSprite(th->bg);
    c.drawRoundRect(4, 4, c.width() - 8, c.height() - 8, 14, th->dim);
    c.setFont(big ? &fonts::FreeSansBold18pt7b : &fonts::FreeSansBold12pt7b);
    c.setTextSize(1);
    c.setTextColor(th->label);
    c.setTextDatum(top_left);
    c.drawString(label, 20, big ? 16 : 12);
    if (unitColor >= 0) c.setTextColor((uint16_t)unitColor);
    c.setTextDatum(top_right);
    // A target name can be longer than the corner, so it stops before the label.
    int unitMax = c.width() - 48 - c.textWidth(label);
    if (unit[0] && unitMax > 8) {
        c.setClipRect(c.width() - 20 - unitMax, 0, unitMax + 20, big ? 52 : 36);
        c.drawString(unit, c.width() - 20, big ? 16 : 12);
        c.clearClipRect();
    }
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

void drawRow(int idx, int y, const char* left, const char* right, uint16_t color, bool large = false) {
    char key[120];
    snprintf(key, sizeof(key), "%c%s|%s", large ? 'L' : 's', left, right);
    if (!changed(rowCache[idx], key)) return;
    rowC.fillSprite(th->bg);
    rowC.setTextSize(1);
    rowC.setFont(large ? &fonts::FreeSansBold18pt7b : &fonts::FreeSans12pt7b);
    // A long hint stays on one line; the bold 12 pt face still reads larger than the old text.
    if (large && rowC.textWidth(left) + rowC.textWidth(right) > W - 96)
        rowC.setFont(&fonts::FreeSansBold12pt7b);
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
        if (v.sats > 0) snprintf(mid, sizeof(mid), "GPS searching  %d sats", v.sats);
        else snprintf(mid, sizeof(mid), "GPS on, searching");
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
    else if (v.battery == -2) snprintf(right, sizeof(right), "%s%s%s%sUSB", hrText, wifi, sim, clock);
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
    if (race.open) {
        drawButton(0, "", false);
        drawButton(1, "", false);
        drawButton(2, "", false);
        drawButton(3, "", false);
        drawButton(4, "DONE", true);
        return;
    }
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
    } else if (screen == Screen::Stroke) {
        char crew[12];
        snprintf(crew, sizeof(crew), "CREW %d", settings::get().crew);
        drawButton(3, crew, true);
    } else if (screen == Screen::Sessions) {
        bool inner = sessionsui::reviewing() || sessionsui::editing();
        drawButton(3, inner ? "BACK" : "", inner);
    } else {
        drawButton(3, "", false);
    }

    if (screen == Screen::Gps || screen == Screen::Targets || screen == Screen::Segments) {
        drawButton(4, "BACK", true);
        return;
    }
    const char* next = screen == Screen::Main         ? "MAP"
                       : screen == Screen::Map        ? "EFFICIENCY"
                       : screen == Screen::Efficiency ? "STROKE"
                       : screen == Screen::Stroke     ? "PLOT"
                       : screen == Screen::Plot       ? "SETTINGS"
                       : screen == Screen::Settings   ? "SESSIONS"
                                                      : "METRICS";
    drawButton(4, next, true);
}

// ---- main metrics screen ---------------------------------------------------

void drawSmallRow(const metrics::View& v);

// The first four slots in one row across the top (full-width PIP and the Map screen).
void drawTopTiles(const metrics::View& v) {
    for (int i = 0; i < 4; i++) drawSlot(i, i * SMALL_W, BAR_H, false, v);
}

void drawMain(const metrics::View& v) {
    const Settings& s = settings::get();
    bool hasPip = s.pipMode != PipOff;

    if (hasPip && s.camFull) {
        drawTopTiles(v);
        return;
    }
    if (hasPip) {
        drawSlot(0, BIG_W, BAR_H, true, v);
        drawSlot(1, BIG_W, BAR_H + BIG_H, true, v);
        drawSlot(2, 0, PIP_INFO_Y, false, v);
        drawSlot(3, SMALL_W, PIP_INFO_Y, false, v);
        drawSmallRow(v);
        return;
    }

    drawSlot(0, 0, BAR_H, true, v);
    drawSlot(1, BIG_W, BAR_H, true, v);
    drawSlot(2, 0, BAR_H + BIG_H, true, v);
    drawSlot(3, BIG_W, BAR_H + BIG_H, true, v);
    drawSmallRow(v);
}

void drawSmallRow(const metrics::View& v) {
    for (int i = 0; i < 4; i++) drawSlot(4 + i, i * SMALL_W, SMALL_Y, false, v);
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
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextDatum(top_center);
    c.setTextColor(th->label);
    if (s.score > 0) snprintf(b, sizeof(b), "of your best %.1f m at %d spm", s.best, (int)lroundf(s.rate));
    else if (s.rate > 0) snprintf(b, sizeof(b), "learning your best at %d spm", (int)lroundf(s.rate));
    else snprintf(b, sizeof(b), "distance per stroke vs your best");
    if (c.textWidth(b) > EFF_PANEL_W - 32) c.setFont(&fonts::FreeSansBold12pt7b);
    c.drawString(b, EFF_PANEL_W / 2, 148);
    c.drawFastHLine(16, 194, EFF_PANEL_W - 32, th->dim);

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
    drawRow(0, BTN_Y - ROW_H, s.hint, right, s.hintWarn ? th->caution : th->fg, true);
}

// ---- stroke anatomy screen ---------------------------------------------------

uint16_t strokeBand(int phase) {
    // Dark tints at night, pale ones by day, so the trace stays readable on top.
    static const uint16_t night[5] = {0x5042, 0x118E, 0x130C, 0x3306, 0x6202};
    static const uint16_t day[5] = {0xFD1C, 0xC55F, 0xB65F, 0xE61C, 0xFDD4};
    return (th == &NIGHT ? night : day)[phase];
}

// Green within 10% of the reference, orange within 25%, red beyond. `lower` means smaller is better.
uint16_t strokeVs(float now, float ref, bool lower, bool haveRef) {
    if (!haveRef || ref <= 0 || now <= 0) return th->fg;
    float ratio = lower ? now / ref : ref / now;
    return ratio <= 1.10f ? th->ok : ratio <= 1.25f ? th->caution : th->warn;
}

uint16_t strokeWorse(uint16_t a, uint16_t b) {
    auto rank = [&](uint16_t c) { return c == th->warn ? 3 : c == th->caution ? 2 : c == th->ok ? 1 : 0; };
    return rank(a) >= rank(b) ? a : b;
}

void drawStrokeChart(const anatomy::Summary& s) {
    M5Canvas& c = effC;
    c.fillSprite(th->bg);
    c.setTextSize(1);
    const int L = 58, R = EFF_CHART_W - 14;
    const int aT = 34, aB = 242, sT = 268, sB = 474;
    const float tLeft = -anatomy::PREROLL_S;

    if (!s.have) {
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextDatum(middle_center);
        c.setTextColor(th->label);
        c.drawString("STROKE", EFF_CHART_W / 2, EFF_H / 2 - 24);
        c.setFont(&fonts::FreeSans12pt7b);
        c.drawString("The phases appear after a few strokes", EFF_CHART_W / 2, EFF_H / 2 + 16);
        fb::push(c, 0, BAR_H);
        return;
    }

    const float tRight = fmaxf(0.8f, s.driveS + s.recS);
    auto X = [&](float t) { return L + (int)lroundf((t - tLeft) / (tRight - tLeft) * (R - L)); };

    float aLo = 0, aHi = 0;
    auto acc = [&](float v) { aLo = fminf(aLo, v); aHi = fmaxf(aHi, v); };
    for (int i = 0; i < anatomy::PREROLL_N; i++) acc(s.preroll[i]);
    for (int i = 0; i < anatomy::BINS; i++) acc(s.accel[i]);
    if (s.haveRef) {
        for (int i = 0; i < anatomy::PREROLL_N; i++) acc(s.refPreroll[i]);
        for (int i = 0; i < anatomy::BINS; i++) acc(s.refAccel[i]);
    }
    float aPad = 0.10f * (aHi - aLo) + 0.04f;
    aLo -= aPad;
    aHi += aPad;

    float vLo = 1e9f, vHi = -1e9f;
    auto vel = [&](float v) { vLo = fminf(vLo, v); vHi = fmaxf(vHi, v); };
    for (int i = 0; i < anatomy::PREROLL_N + anatomy::BINS; i++) vel(s.speed[i]);
    for (int i = 0; i < s.gpsN; i++) vel(s.gpsV[i]);
    if (vHi < vLo) {
        vLo = -0.4f;
        vHi = 0.4f;
    }
    float vPad = 0.12f * (vHi - vLo) + 0.05f;
    vLo -= vPad;
    vHi += vPad;

    auto Ya = [&](float g) { return (int)lroundf(aB - (g - aLo) / (aHi - aLo) * (aB - aT)); };
    auto Yv = [&](float v) { return (int)lroundf(sB - (v - vLo) / (vHi - vLo) * (sB - sT)); };

    if (s.now.valid) {
        const float edge[6] = {s.bounds.catch0, s.bounds.catch1, s.bounds.finish, s.bounds.finishEnd, s.bounds.approach,
                                s.bounds.interval};
        const char* name[5] = {"CATCH", "DRIVE", "FINISH", "RUN", "APPROACH"};
        c.setFont(&fonts::FreeSans9pt7b);
        c.setTextDatum(top_center);
        for (int p = 0; p < 5; p++) {
            float u0 = fmaxf(tLeft, edge[p]), u1 = fminf(tRight, edge[p + 1]);
            if (u1 - u0 < 0.02f) continue;
            int x0 = X(u0), x1 = X(u1);
            c.fillRect(x0, aT, x1 - x0, sB - aT, strokeBand(p));
            if (x1 - x0 > c.textWidth(name[p]) + 6) {
                c.setTextColor(th->fg);
                c.drawString(name[p], (x0 + x1) / 2, aT + 3);
            }
        }
    }

    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextColor(th->label);
    c.setTextDatum(middle_right);
    for (float g : {aLo, 0.0f, aHi}) {
        if (g < aLo - 0.01f || g > aHi + 0.01f) continue;
        int y = Ya(g);
        c.drawFastHLine(L, y, R - L, g == 0 ? th->label : th->dim);
        char t[12];
        snprintf(t, sizeof(t), "%+.1f", g);
        if (y > aT + 8 && y < aB - 8) c.drawString(t, L - 4, y);
    }
    c.drawFastHLine(L, Yv(vLo), R - L, th->dim);
    c.drawFastHLine(L, Yv(vHi), R - L, th->dim);
    float vMid = (vLo + vHi) * 0.5f;
    c.drawFastHLine(L, Yv(vMid), R - L, th->dim);
    char t[16];
    snprintf(t, sizeof(t), "%.1f", vMid);
    c.drawString(t, L - 4, Yv(vMid));

    int xCatch = X(0), xFinish = X(s.driveS);
    c.drawFastVLine(xCatch, aT, sB - aT, th->ok);
    c.drawFastVLine(xFinish, aT, sB - aT, TFT_CYAN);

    auto trace = [&](const float* preroll, const float* accel, const float* speed, uint16_t color, bool accelTrace) {
        int prevX = -1, prevY = -1;
        for (int i = 0; i < anatomy::PREROLL_N + anatomy::BINS; i++) {
            float time = i < anatomy::PREROLL_N ? anatomy::prerollTime(i)
                                                 : anatomy::binTime(i - anatomy::PREROLL_N, s.driveS, s.recS);
            float value = speed ? speed[i] : (i < anatomy::PREROLL_N ? preroll[i] : accel[i - anatomy::PREROLL_N]);
            int x = X(time), y = accelTrace ? Ya(value) : Yv(value);
            if (prevX >= 0) {
                c.drawLine(prevX, prevY, x, y, color);
                if (color == th->accent) c.drawLine(prevX, prevY + 1, x, y + 1, color);
            }
            prevX = x;
            prevY = y;
        }
    };

    c.setClipRect(L, aT, R - L + 1, aB - aT + 1);
    if (s.haveRef) trace(s.refPreroll, s.refAccel, nullptr, th->label, true);
    trace(s.preroll, s.accel, nullptr, th->accent, true);
    c.clearClipRect();

    c.setClipRect(L, sT, R - L + 1, sB - sT + 1);
    trace(nullptr, nullptr, s.speed, th->accent, false);
    for (int i = 0; i < s.gpsN; i++) {
        int x = X(s.gpsT[i]), y = Yv(s.gpsV[i]);
        if (x >= L && x <= R) c.fillCircle(x, y, 3, th->fg);
    }
    c.clearClipRect();

    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextDatum(top_center);
    c.setTextColor(th->label);
    for (float tick = 0; tick <= tRight + 0.01f; tick += 0.5f) {
        int x = X(tick);
        c.drawFastVLine(x, sB, 4, th->dim);
        snprintf(t, sizeof(t), "%.1f", tick);
        c.drawString(t, x, sB + 4);
    }

    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(top_left);
    c.setTextColor(th->fg);
    c.drawString("STROKE", 12, 6);
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextDatum(top_right);
    int lx = R;
    const char* legend[] = {"gps", "reference", "now", "finish", "catch"};
    const uint16_t legendColor[] = {th->fg, th->label, th->accent, TFT_CYAN, th->ok};
    for (int i = 0; i < 5; i++) {
        if (i == 0 && s.gpsN == 0) continue;
        if (i == 1 && !s.haveRef) continue;
        c.setTextColor(legendColor[i]);
        c.drawString(legend[i], lx, 8);
        lx -= c.textWidth(legend[i]) + 14;
    }
    fb::push(c, 0, BAR_H);
}

void drawStrokePanel(const anatomy::Summary& s) {
    M5Canvas& c = effP;
    c.fillSprite(th->bg);
    c.setTextSize(1);
    c.drawFastVLine(0, 0, EFF_H, th->dim);

    char value[32], caption[64];
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextDatum(top_left);
    c.setTextColor(th->fg);
    if (s.have && s.rate > 0) snprintf(value, sizeof(value), "%d spm", (int)lroundf(s.rate));
    else snprintf(value, sizeof(value), "--");
    c.drawString(value, 24, 8);
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(top_right);
    c.setTextColor(th->label);
    const char* vs = !s.have ? "waiting" : !s.haveRef ? "learning" : s.vsBest ? "vs your best" : "vs your usual";
    c.drawString(vs, EFF_PANEL_W - 20, 14);
    c.setFont(&fonts::FreeSans12pt7b);
    c.setTextDatum(top_left);
    if (s.samples > 0) snprintf(caption, sizeof(caption), "%d-stroke average", s.samples);
    else snprintf(caption, sizeof(caption), "no stroke yet");
    c.drawString(caption, 24, 44);
    c.drawFastHLine(16, 70, EFF_PANEL_W - 32, th->dim);

    const int rows = s.crew > 1 ? 6 : 5;
    const int rowY0 = 78;
    const int pitch = (EFF_H - rowY0 - 4) / rows;
    int rowY = rowY0;

    auto gauge = [&](const char* label, int phase, const char* val, const char* cap, uint16_t color, float frac,
                      float refFrac) {
        if (phase >= 0) c.fillRoundRect(20, rowY + 4, 12, 12, 2, strokeBand(phase));
        c.setFont(&fonts::FreeSansBold12pt7b);
        c.setTextDatum(top_left);
        c.setTextColor(th->label);
        c.drawString(label, phase >= 0 ? 40 : 20, rowY);
        c.setTextDatum(top_right);
        c.setTextColor(color);
        c.drawString(val, EFF_PANEL_W - 20, rowY);
        const int bx = 20, bw = EFF_PANEL_W - 40, bh = 14;
        int by = rowY + 26;
        c.drawRoundRect(bx, by, bw, bh, 4, th->dim);
        int fw = (int)(fmaxf(0, fminf(1.0f, frac)) * (bw - 4));
        if (fw > 0) c.fillRoundRect(bx + 2, by + 2, fw, bh - 4, 3, color);
        if (refFrac > 0.02f && refFrac < 1.15f) {
            int rx = bx + (int)(fminf(1.0f, refFrac) * (bw - 4));
            c.drawFastVLine(rx, by - 3, bh + 6, th->fg);
        }
        c.setFont(&fonts::FreeSans12pt7b);
        c.setTextDatum(top_left);
        c.setTextColor(th->label);
        c.drawString(cap, 20, rowY + 44);
        rowY += pitch;
    };

    const anatomy::Features& now = s.now;
    const anatomy::Features& ref = s.ref;
    bool ok = s.now.valid;
    auto scaleOf = [&](float refV, float fallback) { return s.haveRef && refV > 0 ? refV * 1.8f : fallback; };

    if (!ok) snprintf(value, sizeof(value), "--"), snprintf(caption, sizeof(caption), "");
    else snprintf(value, sizeof(value), "%d ms", (int)lroundf(now.catchWidthS * 1000));
    if (ok) snprintf(caption, sizeof(caption), "%.2f g deep, loses %.2f m/s", now.catchDepthG, now.catchLoss);
    float catchScale = scaleOf(ref.catchWidthS, 0.30f);
    gauge("CATCH", 0, value, caption, strokeVs(now.catchWidthS, ref.catchWidthS, true, s.haveRef && ok),
          ok ? now.catchWidthS / catchScale : 0, s.haveRef ? ref.catchWidthS / catchScale : 0);

    uint16_t driveColor = th->fg;
    if (ok) {
        uint16_t gain = strokeVs(now.driveGain, ref.driveGain, false, s.haveRef);
        uint16_t peak = th->fg;
        if (s.haveRef) {
            float d = fabsf(now.drivePeakPct - ref.drivePeakPct);
            peak = d <= 0.10f ? th->ok : d <= 0.20f ? th->caution : th->warn;
        }
        driveColor = strokeWorse(gain, peak);
        snprintf(value, sizeof(value), "+%.2f m/s", now.driveGain);
        snprintf(caption, sizeof(caption), "peak %d%%, sag %.2f g", (int)lroundf(now.drivePeakPct * 100), now.driveSagG);
    } else {
        snprintf(value, sizeof(value), "--");
        caption[0] = 0;
    }
    float driveScale = scaleOf(ref.driveGain, 1.2f);
    gauge("DRIVE", 1, value, caption, driveColor, ok ? now.driveGain / driveScale : 0,
          s.haveRef ? ref.driveGain / driveScale : 0);

    uint16_t finishColor = th->fg;
    if (ok) {
        finishColor = strokeWorse(strokeVs(now.fallTimeS, ref.fallTimeS, true, s.haveRef),
                                   strokeVs(now.releaseDipG, ref.releaseDipG, true, s.haveRef));
        snprintf(value, sizeof(value), "%d ms", (int)lroundf(now.fallTimeS * 1000));
        snprintf(caption, sizeof(caption), "release dip %.2f g", now.releaseDipG);
    } else {
        snprintf(value, sizeof(value), "--");
        caption[0] = 0;
    }
    float fallScale = scaleOf(ref.fallTimeS, 0.30f);
    gauge("FINISH", 2, value, caption, finishColor, ok ? now.fallTimeS / fallScale : 0,
          s.haveRef ? ref.fallTimeS / fallScale : 0);

    if (ok) {
        snprintf(value, sizeof(value), "%d mg", (int)lroundf(now.runDecelMg));
        snprintf(caption, sizeof(caption), "speed peaks at %.1f s", now.speedPeakS);
    } else {
        snprintf(value, sizeof(value), "--");
        caption[0] = 0;
    }
    float runScale = scaleOf(ref.runDecelMg, 80.0f);
    gauge("RUN", 3, value, caption, strokeVs(now.runDecelMg, ref.runDecelMg, true, s.haveRef && ok),
          ok ? now.runDecelMg / runScale : 0, s.haveRef ? ref.runDecelMg / runScale : 0);

    if (ok) {
        snprintf(value, sizeof(value), "%.2f s", now.approachLeadS);
        snprintf(caption, sizeof(caption), "loses %.2f m/s before the blade is in", now.approachLoss);
    } else {
        snprintf(value, sizeof(value), "--");
        caption[0] = 0;
    }
    float leadScale = scaleOf(ref.approachLeadS, 0.50f);
    gauge("APPROACH", 4, value, caption, strokeVs(now.approachLeadS, ref.approachLeadS, true, s.haveRef && ok),
          ok ? now.approachLeadS / leadScale : 0, s.haveRef ? ref.approachLeadS / leadScale : 0);

    if (s.crew > 1) {
        uint16_t col = th->fg;
        if (ok) {
            uint16_t notches = th->fg;
            if (s.haveRef && now.notches >= ref.notches + 0.8f) notches = th->warn;
            else if (s.haveRef) notches = th->ok;
            col = strokeWorse(strokeVs(now.catchWidthS, ref.catchWidthS, true, s.haveRef), notches);
            snprintf(value, sizeof(value), "%d ms", (int)lroundf(now.catchWidthS * 1000));
            if (now.notches < 1.5f) snprintf(caption, sizeof(caption), "1 notch in the catch");
            else snprintf(caption, sizeof(caption), "%.0f notches in the catch", now.notches);
        } else {
            snprintf(value, sizeof(value), "--");
            caption[0] = 0;
        }
        gauge("TOGETHER", -1, value, caption, col, ok ? now.catchWidthS / catchScale : 0,
              s.haveRef ? ref.catchWidthS / catchScale : 0);
    }
    fb::push(c, EFF_CHART_W, BAR_H);
}

void drawStroke(const metrics::View& v) {
    uint32_t ver = anatomy::version();
    if (!forceRedraw && ver == strokeVersion && millis() - lastStroke < 1000) return;
    strokeVersion = ver;
    lastStroke = millis();
    const anatomy::Summary& s = anatomy::summary(v.rate);
    drawStrokeChart(s);
    drawStrokePanel(s);
    char right[24];
    snprintf(right, sizeof(right), "%d stroke%s", s.samples, s.samples == 1 ? "" : "s");
    drawRow(0, BTN_Y - ROW_H, s.hint, right, s.hintWarn ? th->caution : th->fg, true);
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

enum SettingRow { S_K, S_FLOOR, S_MINIMA, S_RAW, S_FUSION, S_RATIO, S_CAM_MIRROR, S_THEME, S_FLIP, S_BRIGHT, S_HR, S_GPS, S_COUNT };
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
    {"SENSORS", {S_HR, S_FUSION, S_RAW, S_GPS}},
    {"WIFI", {-1, -1, -1, -1}},
};
constexpr int SET_TAB_COUNT = sizeof(SET_TABS) / sizeof(SET_TABS[0]);
constexpr int TAB_WIFI = 3;
constexpr int WIFI_ROW = 68;
constexpr int WIFI_HEAD = 90;
constexpr int WIFI_SUB = 52;
int settingsTab = 0;
Cache setTabCache, setRowCache[SET_ROWS], wifiCache;
bool wifiEditing = false;
char wifiJoin[33] = "";
int wifiScroll = 0;
int wifiDragY = 0;
int wifiScroll0 = 0;

int wifiListTop() { return SET_TAB_H + WIFI_HEAD + WIFI_SUB; }
int wifiMaxScroll() {
    int extra = net::apCount() * WIFI_ROW - ((BTN_Y - BAR_H) - wifiListTop());
    return extra > 0 ? extra : 0;
}

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
        case S_GPS: {
            gps::Snapshot g = gps::get();
            snprintf(label, ln, "GPS");
            if (!g.connected && g.scanning) snprintf(value, vn, "SCANNING");
            else if (!g.connected) snprintf(value, vn, "NO LINK");
            else if (g.fix) snprintf(value, vn, "FIX");
            else if (g.sats > 0) snprintf(value, vn, "%d SATS", g.sats);
            else snprintf(value, vn, "SEARCHING");
            break;
        }
    }
}

const char* settingButton(int row) {
    if (row == S_GPS) return "OPEN";
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
        } else if (r == S_HR || r == S_GPS) {
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

void enterWifiTab() {
    wifiScroll = 0;
    wifiEditing = false;
    net::watchScan(true);
}

void leaveWifiTab() {
    wifiEditing = false;
    net::watchScan(false);
}

void drawWifiTabs(M5Canvas& c) {
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
}

void drawWifi() {
    uint32_t passHash = 0;
    if (wifiEditing)
        for (const char* p = osk::text(); *p; ++p) passHash = passHash * 33 + (uint8_t)*p;
    char key[96];
    snprintf(key, sizeof(key), "%d|%lu|%d|%d|%d|%d|%s|%lu|%d", wifiEditing, (unsigned long)net::scanGeneration(),
             net::apCount(), wifiScroll, (int)net::state(), net::hasStoredNetwork() ? 1 : 0, net::ssid(),
             (unsigned long)passHash, net::scanning() ? 1 : 0);
    if (!changed(wifiCache, key)) return;

    M5Canvas& c = wifiC;
    c.fillSprite(th->bg);
    if (wifiEditing) {
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextSize(1);
        c.setTextDatum(middle_left);
        c.setTextColor(th->fg);
        char title[64];
        snprintf(title, sizeof(title), "Password for %s", wifiJoin);
        c.drawString(title, 24, 28);
        Palette pal{th->bg, th->fg, th->label, th->accent, th->dim, th->btn, th->btnText, th->warn, th->ok};
        osk::draw(c, 48, pal);
        fb::push(c, 0, BAR_H);
        return;
    }

    drawWifiTabs(c);
    int y = SET_TAB_H;
    c.drawFastHLine(0, y + WIFI_HEAD - 1, W, th->dim);
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextSize(1);
    c.setTextDatum(middle_left);
    c.setTextColor(th->fg);
    const char* saved = net::hasStoredNetwork() && net::ssid()[0] ? net::ssid() : "No network saved";
    c.drawString(saved, 24, y + WIFI_HEAD / 2);
    const char* status = "OFF";
    uint16_t statusColor = th->label;
    switch (net::state()) {
        case net::State::Connected: status = "ON"; statusColor = th->ok; break;
        case net::State::Connecting: status = "JOINING"; statusColor = th->caution; break;
        case net::State::Failed: status = "FAILED"; statusColor = th->warn; break;
        case net::State::Off: break;
    }
    int bx = W - 20 - SET_TEXT_BTN_W;
    c.setTextDatum(middle_right);
    c.setTextColor(statusColor);
    if (net::hasStoredNetwork()) {
        c.drawString(status, bx - 24, y + WIFI_HEAD / 2);
        c.fillSmoothRoundRect(bx, y + (WIFI_HEAD - SET_ICON) / 2, SET_TEXT_BTN_W, SET_ICON, 16, th->btn);
        c.setTextDatum(middle_center);
        c.setTextColor(th->btnText);
        c.drawString("FORGET", bx + SET_TEXT_BTN_W / 2, y + WIFI_HEAD / 2);
    } else {
        c.drawString(status, W - 24, y + WIFI_HEAD / 2);
    }

    y += WIFI_HEAD;
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(middle_left);
    c.setTextColor(th->label);
    if (net::scanning() && net::apCount() == 0) c.drawString("Scanning", 24, y + WIFI_SUB / 2);
    else {
        char sub[32];
        snprintf(sub, sizeof(sub), "%d networks", net::apCount());
        c.drawString(sub, 24, y + WIFI_SUB / 2);
    }
    int rx = W - 20 - 170;
    c.fillSmoothRoundRect(rx, y + (WIFI_SUB - 44) / 2, 170, 44, 12, th->btn);
    c.setTextDatum(middle_center);
    c.setTextColor(th->btnText);
    c.drawString(net::scanning() ? "SCAN..." : "RESCAN", rx + 85, y + WIFI_SUB / 2);

    int listTop = wifiListTop();
    int n = net::apCount();
    if (n == 0 && !net::scanning()) {
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextDatum(middle_left);
        c.setTextColor(th->label);
        c.drawString("No networks found", 24, listTop + 36);
    }
    for (int i = 0; i < n; i++) {
        int ry = listTop + i * WIFI_ROW - wifiScroll;
        if (ry < listTop || ry >= c.height()) continue;
        const net::Ap* a = net::ap(i);
        if (!a) continue;
        c.drawFastHLine(0, ry + WIFI_ROW - 1, W, th->dim);
        bool current = net::ssid()[0] && strcmp(a->ssid, net::ssid()) == 0 &&
                       (net::connected() || net::state() == net::State::Connecting);
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextDatum(middle_left);
        c.setTextColor(current ? th->accent : th->fg);
        c.drawString(a->ssid, 24, ry + WIFI_ROW / 2);
        char side[24];
        snprintf(side, sizeof(side), "%d   %s", a->rssi, a->open ? "open" : "lock");
        c.setFont(&fonts::FreeSans12pt7b);
        c.setTextDatum(middle_right);
        c.setTextColor(th->label);
        c.drawString(side, W - 24, ry + WIFI_ROW / 2);
    }
    fb::push(c, 0, BAR_H);
}

void onWifiTap(int x, int y) {
    int cy = y - BAR_H;
    if (wifiEditing) {
        Palette pal{th->bg, th->fg, th->label, th->accent, th->dim, th->btn, th->btnText, th->warn, th->ok};
        osk::Result r = osk::tap(x, cy, W, 48);
        if (r == osk::Result::Save) {
            net::connect(wifiJoin, osk::text(), Serial);
            wifiEditing = false;
        } else if (r == osk::Result::Cancel) {
            wifiEditing = false;
        }
        forceRedraw = true;
        return;
    }
    if (cy >= SET_TAB_H && cy < SET_TAB_H + WIFI_HEAD) {
        int bx = W - 20 - SET_TEXT_BTN_W;
        if (net::hasStoredNetwork() && x >= bx) {
            net::forget();
            wifiScroll = 0;
            net::beginScan();
            forceRedraw = true;
        }
        return;
    }
    if (cy >= SET_TAB_H + WIFI_HEAD && cy < wifiListTop()) {
        if (x >= W - 20 - 170) {
            net::beginScan();
            forceRedraw = true;
        }
        return;
    }
    int rel = cy - wifiListTop() + wifiScroll;
    if (rel < 0) return;
    int i = rel / WIFI_ROW;
    const net::Ap* a = net::ap(i);
    if (!a) return;
    strlcpy(wifiJoin, a->ssid, sizeof(wifiJoin));
    bool saved = net::hasStoredNetwork() && strcmp(wifiJoin, net::ssid()) == 0;
    if (a->open || saved) net::connect(saved ? nullptr : wifiJoin, saved ? nullptr : "", Serial);
    else {
        osk::start("", 63, osk::Kind::Secret);
        wifiEditing = true;
    }
    forceRedraw = true;
}

void drawSettings() {
    if (settingsTab == TAB_WIFI) {
        drawWifi();
        return;
    }
    drawSettingsTabs();
    for (int i = 0; i < SET_ROWS; i++) drawSettingRow(i, SET_TABS[settingsTab].rows[i]);

    char info[96];
    snprintf(info, sizeof(info), "SD card: %s   log drops: %lu", logger::sdReady() ? "OK" : "not found",
             (unsigned long)logger::dropped());
    // Footer cache slot. S_COUNT is not an index: rowCache only has 12 entries.
    drawRow(11, BTN_Y - ROW_H, info,
#if SIM_MODE
            "build: SIMULATION",
#else
            "build: hardware",
#endif
            th->label);
}

void applySettings(bool full);
void openGps();

void onSettingsTap(int x, int y) {
    if (settingsTab == TAB_WIFI && wifiEditing) {
        onWifiTap(x, y);
        return;
    }
    if (y >= BAR_H && y < BAR_H + SET_TAB_H) {
        int tab = min(SET_TAB_COUNT - 1, x / (W / SET_TAB_COUNT));
        if (tab != settingsTab) {
            if (settingsTab == TAB_WIFI) leaveWifiTab();
            settingsTab = tab;
            if (settingsTab == TAB_WIFI) enterWifiTab();
            M5.Display.fillRect(0, BAR_H, W, BTN_Y - BAR_H, th->bg);
            forceRedraw = true;
        }
        return;
    }
    if (settingsTab == TAB_WIFI) {
        onWifiTap(x, y);
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
        case S_GPS:
            openGps();
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
enum class DragKind : uint8_t { None, Lists, Map, Wifi, Targets, Segments, Waypoint } dragKind = DragKind::None;

// A course being dropped on the map: each tap adds a point and joins it to the last.
bool segPlacing = false;
// Looking at a segment leaves its points where they are. Edit mode is what lets a drag move them.
bool segMapEdit = false;
// A segment whose start is within 100 m, offered on the map until it is selected.
constexpr float NEAR_START_M = 100;
int nearIdx = -1;
int nearX = 0, nearY = 0, nearW = 0, nearH = 0;
int segPlacePrev = -1;
int* segDraft = nullptr;
int segDraftN = 0;
int segDraftCap = 0;
int* segMade = nullptr;  // targets created for this draft, dropped if it is discarded
int segMadeN = 0;
int segMadeCap = 0;
int wpDrag = -1;
// The Tab5 panel is 5 inches at 1280x720, about 116 px per centimetre. A dragged
// waypoint is placed this far above the finger so the point stays visible.
constexpr int WP_LIFT_PX = 116;
// One map frame, with the moving sight and its course left out. Further finger
// movement paints on top of this copy instead of redrawing the water.
uint8_t* wpHold = nullptr;
size_t wpHoldBytes = 0;
bool wpBasePending = false;
bool wpCaptureThis = false;
bool wpHoldReady = false;
bool wpSightMoved = false;
mapview::View wpView;

mapview::View::Pin* pinSnap = nullptr;
int pinSnapCap = 0;
double* routeLatSnap = nullptr;
double* routeLonSnap = nullptr;
int routeSnapCap = 0;

bool growInts(int*& buf, int& cap, int need) {
    if (need <= cap) return true;
    int ncap = cap < 8 ? 8 : cap;
    while (ncap < need) ncap *= 2;
    int* p = (int*)heap_caps_realloc(buf, ncap * sizeof(int), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (int*)heap_caps_realloc(buf, ncap * sizeof(int), MALLOC_CAP_8BIT);
    if (!p) return false;
    buf = p;
    cap = ncap;
    return true;
}

bool ensureMapSnap(int pins, int route) {
    if (pins > pinSnapCap) {
        int ncap = pinSnapCap < 8 ? 8 : pinSnapCap;
        while (ncap < pins) ncap *= 2;
        auto* p = (mapview::View::Pin*)heap_caps_realloc(pinSnap, ncap * sizeof(mapview::View::Pin),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p)
            p = (mapview::View::Pin*)heap_caps_realloc(pinSnap, ncap * sizeof(mapview::View::Pin), MALLOC_CAP_8BIT);
        if (!p) return false;
        pinSnap = p;
        pinSnapCap = ncap;
    }
    if (route > routeSnapCap) {
        int ncap = routeSnapCap < 8 ? 8 : routeSnapCap;
        while (ncap < route) ncap *= 2;
        double* lat = (double*)heap_caps_realloc(routeLatSnap, ncap * sizeof(double), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!lat) lat = (double*)heap_caps_realloc(routeLatSnap, ncap * sizeof(double), MALLOC_CAP_8BIT);
        if (!lat) return false;
        routeLatSnap = lat;
        double* lon = (double*)heap_caps_realloc(routeLonSnap, ncap * sizeof(double), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!lon) lon = (double*)heap_caps_realloc(routeLonSnap, ncap * sizeof(double), MALLOC_CAP_8BIT);
        if (!lon) return false;
        routeLonSnap = lon;
        routeSnapCap = ncap;
    }
    return true;
}

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

// Canvas coordinates. The pair sits under MAPS between sessions, and in that corner while recording.
void targetsButtonRect(int& x, int& y, int& w, int& h) {
    x = MAPS_BTN_X;
    y = mapsButtonShown() ? MAPS_BTN_Y + MAPS_BTN_H + 12 : MAPS_BTN_Y;
    w = 190;
    h = MAPS_BTN_H;
}

void segmentsButtonRect(int& x, int& y, int& w, int& h) {
    int tx, ty, tw, th;
    targetsButtonRect(tx, ty, tw, th);
    x = tx + tw + 12;
    y = ty;
    w = 210;
    h = th;
}

void editButtonRect(int& x, int& y, int& w, int& h) {
    int sx, sy, sw, sh;
    segmentsButtonRect(sx, sy, sw, sh);
    x = sx + sw + 12;
    y = sy;
    w = 150;
    h = sh;
}

bool mapCornerHit(int px, int py, void (*rect)(int&, int&, int&, int&)) {
    if (screen != Screen::Map) return false;
    int x, y, w, h;
    rect(x, y, w, h);
    return px >= x && px < x + w && py >= FULL_CAM_Y + y && py < FULL_CAM_Y + y + h;
}

bool targetsButtonHit(int px, int py) { return mapCornerHit(px, py, targetsButtonRect); }
bool segmentsButtonHit(int px, int py) { return mapCornerHit(px, py, segmentsButtonRect); }
bool editButtonHit(int px, int py) {
    if (segPlacing || segments::selected() < 0) return false;
    return mapCornerHit(px, py, editButtonRect);
}

bool nearStartHit(int px, int py) {
    if (nearIdx < 0 || segPlacing || screen != Screen::Map) return false;
    return px >= nearX && px < nearX + nearW && py >= FULL_CAM_Y + nearY && py < FULL_CAM_Y + nearY + nearH;
}

void placeUndoRect(int& x, int& y, int& w, int& h) {
    int sx, sy, sw, sh;
    segmentsButtonRect(sx, sy, sw, sh);
    x = MAPS_BTN_X;
    y = sy + sh + 12;
    w = 150;
    h = MAPS_BTN_H;
}

void placeSaveRect(int& x, int& y, int& w, int& h) {
    placeUndoRect(x, y, w, h);
    x += w + 12;
}

bool placeButtonHit(int px, int py, bool save) {
    if (!segPlacing || screen != Screen::Map) return false;
    int x, y, w, h;
    if (save) placeSaveRect(x, y, w, h);
    else placeUndoRect(x, y, w, h);
    return px >= x && px < x + w && py >= FULL_CAM_Y + y && py < FULL_CAM_Y + y + h;
}

void pushMap(M5Canvas* cv, int sx, int sy) {
    if (race.open) return;
    cv->setFont(&fonts::FreeSansBold12pt7b);
    cv->setTextSize(1);
    cv->setTextDatum(middle_center);
    if (mapsButtonShown()) {
        cv->fillRoundRect(MAPS_BTN_X, MAPS_BTN_Y, MAPS_BTN_W, MAPS_BTN_H, 10, th->btn);
        cv->drawRoundRect(MAPS_BTN_X, MAPS_BTN_Y, MAPS_BTN_W, MAPS_BTN_H, 10, th->dim);
        cv->setTextColor(th->btnText);
        cv->drawString("MAPS", MAPS_BTN_X + MAPS_BTN_W / 2, MAPS_BTN_Y + MAPS_BTN_H / 2);
    }
    if (screen == Screen::Map) {
        int x, y, w, h;
        targetsButtonRect(x, y, w, h);
        bool lit = targets::selected() >= 0;
        cv->fillRoundRect(x, y, w, h, 10, lit ? th->accent : th->btn);
        cv->drawRoundRect(x, y, w, h, 10, th->dim);
        cv->setTextColor(lit ? th->bg : th->btnText);
        cv->drawString("TARGETS", x + w / 2, y + h / 2);
        segmentsButtonRect(x, y, w, h);
        lit = segments::selected() >= 0 || segPlacing;
        cv->fillRoundRect(x, y, w, h, 10, lit ? th->accent : th->btn);
        cv->drawRoundRect(x, y, w, h, 10, th->dim);
        cv->setTextColor(lit ? th->bg : th->btnText);
        cv->drawString("SEGMENTS", x + w / 2, y + h / 2);
        const char* segName = segments::selectedName();
        if (segName[0] && !segPlacing) {
            editButtonRect(x, y, w, h);
            cv->fillRoundRect(x, y, w, h, 10, segMapEdit ? th->accent : th->btn);
            cv->drawRoundRect(x, y, w, h, 10, th->dim);
            cv->setTextColor(segMapEdit ? th->bg : th->btnText);
            cv->drawString(segMapEdit ? "EDITING" : "EDIT", x + w / 2, y + h / 2);
            int nx = x + w + 16;
            int nw = cv->width() - nx - 56;
            if (nw > 80) {
                cv->setFont(&fonts::FreeSansBold18pt7b);
                int tw = cv->textWidth(segName);
                int boxW = tw + 48;
                if (boxW > nw) boxW = nw;
                cv->fillRoundRect(nx, y, boxW, h, 10, th->bg);
                cv->drawRoundRect(nx, y, boxW, h, 10, th->accent);
                cv->setTextDatum(middle_left);
                cv->setTextColor(th->accent);
                cv->setClipRect(nx + 8, y, boxW - 16, h);
                cv->drawString(segName, nx + 24, y + h / 2);
                cv->clearClipRect();
                cv->setTextDatum(middle_center);
                cv->setFont(&fonts::FreeSansBold12pt7b);
            }
        }
        nearIdx = -1;
        if (!segPlacing) {
            gps::Snapshot g = gps::get();
            int offer = (g.lat != 0 || g.lon != 0) ? segments::nearestStart(g.lat, g.lon, NEAR_START_M) : -1;
            const segments::Segment* near = segments::get(offer);
            if (near && near->name[0] && segments::selected() < 0) {
                int sx, sy, sw, sh;
                segmentsButtonRect(sx, sy, sw, sh);
                int x = sx + sw + 12;
                int y = sy;
                int h = sh;
                int maxW = cv->width() - x - 56;
                if (maxW >= 80) {
                    cv->setFont(&fonts::FreeSansBold18pt7b);
                    int boxW = cv->textWidth(near->name) + 48;
                    if (boxW > maxW) boxW = maxW;
                    cv->fillRoundRect(x, y, boxW, h, 10, th->btn);
                    cv->drawRoundRect(x, y, boxW, h, 10, th->accent);
                    cv->setTextDatum(middle_left);
                    cv->setTextColor(th->accent);
                    cv->setClipRect(x + 8, y, boxW - 16, h);
                    cv->drawString(near->name, x + 24, y + h / 2);
                    cv->clearClipRect();
                    cv->setTextDatum(middle_center);
                    cv->setFont(&fonts::FreeSansBold12pt7b);
                    nearIdx = offer;
                    nearX = x;
                    nearY = y;
                    nearW = boxW;
                    nearH = h;
                }
            }
        }
        if (segPlacing) {
            int ux, uy, uw, uh, vx, vy, vw, vh;
            placeUndoRect(ux, uy, uw, uh);
            placeSaveRect(vx, vy, vw, vh);
            bool ready = segDraftN >= 2;
            cv->fillRoundRect(ux, uy, uw, uh, 10, th->btn);
            cv->drawRoundRect(ux, uy, uw, uh, 10, th->dim);
            cv->setTextColor(segDraftN > 0 ? th->btnText : th->dim);
            cv->drawString("UNDO", ux + uw / 2, uy + uh / 2);
            cv->fillRoundRect(vx, vy, vw, vh, 10, ready ? th->accent : th->btn);
            cv->drawRoundRect(vx, vy, vw, vh, 10, th->dim);
            cv->setTextColor(ready ? th->bg : th->dim);
            cv->drawString("SAVE", vx + vw / 2, vy + vh / 2);
            cv->setTextDatum(middle_left);
            cv->setTextColor(th->label);
            char cap[40];
            if (segDraftN >= 2) {
                float len = segments::lengthAlong(segDraft, segDraftN);
                snprintf(cap, sizeof(cap), "%d m along", len < 0 ? 0 : (int)lroundf(len));
            } else if (segDraftN == 1) {
                snprintf(cap, sizeof(cap), "Tap the next point");
            } else {
                snprintf(cap, sizeof(cap), "Tap the map to add a point");
            }
            cv->setClipRect(vx + vw + 16, vy, cv->width() - (vx + vw + 28), uh);
            cv->drawString(cap, vx + vw + 16, vy + uh / 2);
            cv->clearClipRect();
        }
    }
    fb::push(*cv, sx, sy);
}

void fillLiveMapView(mapview::View& mv, bool full, int canvasW) {
    const Settings& s = settings::get();
    gps::Snapshot g = gps::get();
    mv.lat = g.lat;
    mv.lon = g.lon;
    mv.course = g.course;
    mv.haveFix = g.fix;
    mv.orientation = s.mapOrientation;
    mv.showControls = full;
    mv.largeControls = full && metrics::view().state == metrics::State::Running;
    mv.followBoat = mapFollow;
    mv.centerLat = mapCenterLat;
    mv.centerLon = mapCenterLon;
    mv.showRecenter = full && !mapFollow;
    mv.day = s.dayTheme;
    int zi = s.mapZoom < cfg::MAP_ZOOM_COUNT ? s.mapZoom : cfg::MAP_ZOOM_DEFAULT;
    mv.metersPerPixel = cfg::MAP_ZOOM_M[zi] / (float)canvasW;
    int pinN = targets::count();
    int routeN = 0;
    const segments::Segment* liveSeg = nullptr;
    if (segPlacing) routeN = segDraftN;
    else if (segments::selected() >= 0) {
        liveSeg = segments::get(segments::selected());
        if (liveSeg) routeN = liveSeg->count;
    }
    mv.pins = nullptr;
    mv.pinCount = 0;
    mv.routeLat = nullptr;
    mv.routeLon = nullptr;
    mv.routeCount = 0;
    mv.routeNext = 0;
    mv.showStartLine = false;
    mv.showFinishLine = false;
    if (!ensureMapSnap(pinN, routeN)) return;
    int sel = targets::selected();
    for (int i = 0; i < pinN; i++) {
        const targets::Target* t = targets::get(i);
        if (!t) continue;
        pinSnap[i].lat = t->lat;
        pinSnap[i].lon = t->lon;
        bool inDraft = false;
        bool onRoute = false;
        if (segPlacing) {
            for (int k = 0; k < segDraftN; k++)
                if (segDraft[k] == i) inDraft = true;
        } else if (liveSeg) {
            for (int k = 0; k < liveSeg->count; k++)
                if (liveSeg->point[k] == i) onRoute = true;
        }
        pinSnap[i].selected = inDraft || i == sel;
        pinSnap[i].editing = inDraft || (onRoute && segMapEdit);
        strlcpy(pinSnap[i].name, t->name, sizeof(pinSnap[i].name));
    }
    mv.pins = pinSnap;
    mv.pinCount = pinN;
    if (segPlacing) {
        for (int i = 0; i < routeN; i++) {
            const targets::Target* t = targets::get(segDraft[i]);
            if (!t) {
                routeN = i;
                break;
            }
            routeLatSnap[i] = t->lat;
            routeLonSnap[i] = t->lon;
        }
        mv.routeLat = routeLatSnap;
        mv.routeLon = routeLonSnap;
        mv.routeCount = routeN;
        mv.routeNext = -1;
        if (routeN >= 2) {
            mv.showStartLine = segments::startLineBetween(routeLatSnap[0], routeLonSnap[0], routeLatSnap[1],
                                                           routeLonSnap[1], mv.lineLat[0], mv.lineLon[0],
                                                           mv.lineLat[1], mv.lineLon[1]);
            mv.showFinishLine = segments::finishLineBetween(
                routeLatSnap[routeN - 2], routeLonSnap[routeN - 2], routeLatSnap[routeN - 1], routeLonSnap[routeN - 1],
                mv.finishLat[0], mv.finishLon[0], mv.finishLat[1], mv.finishLon[1]);
        }
    } else if (liveSeg) {
        if (g.lat != 0 || g.lon != 0) segments::track(g.lat, g.lon);
        for (int i = 0; i < routeN; i++) {
            const targets::Target* t = targets::get(liveSeg->point[i]);
            if (!t) {
                routeN = i;
                break;
            }
            routeLatSnap[i] = t->lat;
            routeLonSnap[i] = t->lon;
        }
        mv.routeLat = routeLatSnap;
        mv.routeLon = routeLonSnap;
        mv.routeCount = routeN;
        int nx = segments::nextPoint();
        mv.routeNext = nx < 0 ? 0 : nx;
        mv.showStartLine = segments::startLine(mv.lineLat[0], mv.lineLon[0], mv.lineLat[1], mv.lineLon[1]);
        mv.showFinishLine = segments::finishLine(mv.finishLat[0], mv.finishLon[0], mv.finishLat[1], mv.finishLon[1]);
    }
    mv.omitPin = wpBasePending && wpDrag >= 0 ? wpDrag : -1;
    mv.omitRoute = wpBasePending && wpDrag >= 0;
}

void refreshDragCourse() {
    if (wpDrag >= 0 && wpDrag < pinSnapCap) {
        const targets::Target* moved = targets::get(wpDrag);
        if (moved) {
            pinSnap[wpDrag].lat = moved->lat;
            pinSnap[wpDrag].lon = moved->lon;
        }
    }
    int routeN = 0;
    if (segPlacing) {
        routeN = segDraftN < routeSnapCap ? segDraftN : routeSnapCap;
        for (int i = 0; i < routeN; i++) {
            const targets::Target* t = targets::get(segDraft[i]);
            if (!t) {
                routeN = i;
                break;
            }
            routeLatSnap[i] = t->lat;
            routeLonSnap[i] = t->lon;
        }
        wpView.routeNext = -1;
        wpView.showStartLine = false;
        wpView.showFinishLine = false;
        if (routeN >= 2) {
            wpView.showStartLine = segments::startLineBetween(
                routeLatSnap[0], routeLonSnap[0], routeLatSnap[1], routeLonSnap[1], wpView.lineLat[0], wpView.lineLon[0],
                wpView.lineLat[1], wpView.lineLon[1]);
            wpView.showFinishLine = segments::finishLineBetween(
                routeLatSnap[routeN - 2], routeLonSnap[routeN - 2], routeLatSnap[routeN - 1], routeLonSnap[routeN - 1],
                wpView.finishLat[0], wpView.finishLon[0], wpView.finishLat[1], wpView.finishLon[1]);
        }
    } else {
        wpView.showStartLine = false;
        wpView.showFinishLine = false;
        const segments::Segment* live = segments::selected() >= 0 ? segments::get(segments::selected()) : nullptr;
        if (live) {
            routeN = live->count < routeSnapCap ? live->count : routeSnapCap;
            for (int i = 0; i < routeN; i++) {
                const targets::Target* t = targets::get(live->point[i]);
                if (!t) {
                    routeN = i;
                    break;
                }
                routeLatSnap[i] = t->lat;
                routeLonSnap[i] = t->lon;
            }
            int nx = segments::nextPoint();
            wpView.routeNext = nx < 0 ? 0 : nx;
            wpView.showStartLine = segments::startLine(wpView.lineLat[0], wpView.lineLon[0], wpView.lineLat[1],
                                                        wpView.lineLon[1]);
            wpView.showFinishLine =
                segments::finishLine(wpView.finishLat[0], wpView.finishLon[0], wpView.finishLat[1], wpView.finishLon[1]);
        }
    }
    wpView.routeLat = routeLatSnap;
    wpView.routeLon = routeLonSnap;
    wpView.routeCount = routeN;
    wpView.pins = pinSnap;
    wpView.omitPin = wpDrag;
    wpView.omitRoute = true;
}

void paintFrozenSight(M5Canvas* cv, int sx, int sy) {
    if (!cv || !wpHold || !cv->getBuffer()) return;
    memcpy(cv->getBuffer(), wpHold, wpHoldBytes);
    refreshDragCourse();
    mapview::paintCourse(*cv, wpView);
    pushMap(cv, sx, sy);
}

void drawMapContent() {
    if (forceRedraw && !wpHoldReady && !wpBasePending) {
        layoutGen++;
        mapRedrawPending = true;
    }
    int sx, sy;
    bool full;
    M5Canvas* cv = mapCanvasFor(sx, sy, full);

    if (mapJob == MapJob::Done) {
        bool current = cv && cv == mapJobCanvas && mapJobLayout == layoutGen;
        bool take = current && wpCaptureThis && wpHold;
        mapJob = MapJob::Idle;
        // The gap before the next frame starts now, so the map task never hogs its core.
        lastMap = millis();
        if (take) {
            memcpy(wpHold, cv->getBuffer(), wpHoldBytes);
            wpHoldReady = true;
            wpBasePending = false;
            wpCaptureThis = false;
            wpSightMoved = true;
        } else if (current) {
            pushMap(cv, sx, sy);
        }
        if (!take && wpCaptureThis) {
            wpCaptureThis = false;
            mapRedrawPending = true;
            lastMap = 0;
        }
    }
    if (wpHoldReady) {
        if (wpSightMoved && cv) {
            paintFrozenSight(cv, sx, sy);
            wpSightMoved = false;
        }
        return;
    }
    if (!cv || mapJob != MapJob::Idle) return;
    if (!mapRedrawPending && millis() - lastMap < cfg::MAP_FRAME_MS) return;
    mapRedrawPending = false;

    if (!full && !mapdata::held()) mapFollow = true;
    mapview::View mv;
    fillLiveMapView(mv, full, cv->width());
    if (wpBasePending) {
        wpView = mv;
        wpCaptureThis = true;
    }
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
    if ((s.pipMode != PipMap || !s.camFull) && !mapdata::held()) mapFollow = true;
    clearScreen();
    updateCamera();
}

void togglePipFull() {
    Settings& s = settings::get();
    s.camFull = !s.camFull;
    settings::save();
    if (!s.camFull && !mapdata::held()) mapFollow = true;
    clearScreen();
    updateCamera();
}

void recenterMap() {
    mapdata::release();
    mapFollow = true;
    mapRedrawPending = true;
    forceRedraw = true;
}

void addTargetAt(int cx, int cy) {
    mapview::View mv;
    fillLiveMapView(mv, true, mapC.width());
    double lat = 0, lon = 0;
    if (!mapview::latLonAt(mv, mapC.width(), mapC.height(), (float)cx, (float)cy, lat, lon)) {
        showHint("No map to mark");
        return;
    }
    if (!targets::add(lat, lon)) {
        showHint("Could not save that point");
        return;
    }
    char msg[32];
    snprintf(msg, sizeof(msg), "Target %s", targets::selectedName());
    showHint(msg);
    mapRedrawPending = true;
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
    if (targetsButtonHit(px, py)) return true;
    if (segmentsButtonHit(px, py)) return true;
    if (editButtonHit(px, py)) return true;
    if (nearStartHit(px, py)) return true;
    if (placeButtonHit(px, py, false) || placeButtonHit(px, py, true)) return true;
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

// ---- GPS screen ------------------------------------------------------------
// Same tile grid as the main screen: four large tiles, then four small ones.

void drawGpsTile(int slot, int x, int y, bool big, const char* label, const char* unit, const char* value,
                 uint16_t color, bool numeric, int unitColor = -1) {
    char key[96];
    snprintf(key, sizeof(key), "g|%s|%s|%s|%u|%d", label, unit, value, (unsigned)color, unitColor);
    if (!changed(tileCache[slot], key)) return;
    M5Canvas& c = big ? bigC : smallC;
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
    if (numeric) {
        drawValue(c, value, big ? 56 : 38, big ? 3.6f : 1.15f, color);
    } else {
        int top = big ? 64 : 34;
        c.setTextColor(color);
        c.setTextDatum(middle_center);
        c.setFont(&fonts::FreeSansBold24pt7b);
        float s = 1;
        int tw = c.textWidth(value);
        int fh = c.fontHeight();
        if (tw > 0 && fh > 0) {
            s = fminf((c.width() - 48) / (float)tw, (c.height() - top - 16) / (float)fh);
            s = fminf(s, big ? 2.2f : 1.15f);
        }
        c.setTextSize(s);
        c.drawString(value, c.width() / 2, top + (c.height() - top) / 2);
    }
    fb::push(c, x, y);
}

void drawGps() {
    gps::Snapshot g = gps::get();
    char unit[24], value[28];

    const char* state = "NO LINK";
    uint16_t stateColor = th->warn;
    if (!g.connected && g.scanning) {
        state = "SCANNING";
        stateColor = th->caution;
    } else if (g.connected && g.baud == 0) {
        state = "SIM";
        stateColor = th->fg;
    } else if (g.fix) {
        state = "FIX";
        stateColor = th->ok;
    } else if (g.connected) {
        state = "SEARCHING";
        stateColor = th->warn;
    }
    if (g.rxPin && g.baud) snprintf(unit, sizeof(unit), "G%d   %lu", g.rxPin, (unsigned long)g.baud);
    else if (g.baud) snprintf(unit, sizeof(unit), "%lu", (unsigned long)g.baud);
    else unit[0] = '\0';
    drawGpsTile(0, 0, BAR_H, true, "GPS", unit, state, stateColor, false);

    snprintf(value, sizeof(value), "%d", g.sats);
    uint16_t sky = g.fix ? th->ok : g.sats > 0 ? th->caution : g.connected ? th->warn : th->fg;
    drawGpsTile(1, BIG_W, BAR_H, true, "SATELLITES", "used", value, sky, true);

    if (g.hdop < 90) snprintf(value, sizeof(value), "%.1f", g.hdop);
    else snprintf(value, sizeof(value), "--");
    uint16_t hdopColor = g.hdop <= cfg::GPS_MAX_HDOP ? th->ok : g.connected ? th->warn : th->fg;
    snprintf(unit, sizeof(unit), "max %.0f", cfg::GPS_MAX_HDOP);
    drawGpsTile(2, 0, BAR_H + BIG_H, true, "HDOP", unit, value, hdopColor, true);

    if (g.fix) snprintf(value, sizeof(value), "%.1f", g.speed);
    else snprintf(value, sizeof(value), "--");
    drawGpsTile(3, BIG_W, BAR_H + BIG_H, true, "SPEED", "m/s", value, th->fg, true);

    if (g.fix) snprintf(value, sizeof(value), "%.0f", g.course);
    else snprintf(value, sizeof(value), "--");
    drawGpsTile(4, 0, SMALL_Y, false, "COURSE", "deg", value, th->fg, true);

    if (g.timeValid) snprintf(value, sizeof(value), "%02u:%02u:%02u", g.hour, g.minute, g.second);
    else snprintf(value, sizeof(value), "--");
    drawGpsTile(5, SMALL_W, SMALL_Y, false, "TIME", "UTC", value, th->fg, true);

    if (g.updateHz > 0) snprintf(value, sizeof(value), "%.1f", g.updateHz);
    else snprintf(value, sizeof(value), "--");
    int signalUnit = g.failed ? (int)th->caution : -1;
    drawGpsTile(6, 2 * SMALL_W, SMALL_Y, false, "SIGNAL", g.failed ? "bad" : "Hz", value,
                g.failed ? th->caution : th->fg, true, signalUnit);

    if (g.lat != 0 || g.lon != 0) snprintf(value, sizeof(value), "%.4f, %.4f", g.lat, g.lon);
    else snprintf(value, sizeof(value), "--");
    drawGpsTile(7, 3 * SMALL_W, SMALL_Y, false, "POSITION", "", value, th->fg, g.lat == 0 && g.lon == 0);
}

// ---- input -----------------------------------------------------------------

void clearScreen() {
    M5.Display.fillScreen(th->bg);
    forceRedraw = true;
}

void cancelPlaceSegment();

void setScreen(Screen next) {
    if (next == screen) return;
    if (segPlacing && next != Screen::Map) cancelPlaceSegment();
    if (screen == Screen::Maps) {
        mapsui::leave();
        // Wi-Fi is only for downloading maps; keep it while a download or the setup page runs.
        if (!mapjob::busy() && !net::setupActive()) net::disconnect();
    }
    if (screen == Screen::Sessions) sessionsui::leave();
    if (screen == Screen::Settings) {
        leaveWifiTab();
        if (next != Screen::Maps && !mapjob::busy() && !net::setupActive()) net::disconnect();
    }
    screen = next;
    if (screen == Screen::Settings && settingsTab == TAB_WIFI) enterWifiTab();
    if (screen == Screen::Sessions) sessionsui::enter();
    if (screen == Screen::Maps) {
        mapsui::enter();
        if (net::state() == net::State::Off && net::hasStoredNetwork()) net::connect(nullptr, nullptr, Serial);
    }
    dragging = false;
    if (next != Screen::Map && next != Screen::Targets && next != Screen::Segments && !mapdata::held())
        mapFollow = true;
    clearScreen();
    updateCamera();
}

void openGps() {
    if (screen == Screen::Gps) return;
    gpsBack = screen;
    setScreen(Screen::Gps);
}

void closeGps() {
    setScreen(gpsBack == Screen::Gps ? Screen::Main : gpsBack);
}

constexpr int TGT_HEAD = 80;
constexpr int TGT_ROW = 84;
constexpr int TGT_BTN_W = 150;
int tgtScroll = 0;
int tgtDragY = 0;
int tgtScroll0 = 0;
int tgtEdit = -1;  // row whose name is on the keyboard, or -1
int tgtConfirm = -1;
uint32_t tgtConfirmUntil = 0;

int tgtDeleteX() { return W - 20 - TGT_BTN_W; }
int tgtRenameX() { return tgtDeleteX() - 12 - TGT_BTN_W; }

int tgtMaxScroll() {
    int extra = targets::count() * TGT_ROW - ((BTN_Y - BAR_H) - TGT_HEAD);
    return extra > 0 ? extra : 0;
}

void openTargets() {
    if (screen == Screen::Targets) return;
    tgtBack = screen;
    tgtScroll = 0;
    setScreen(Screen::Targets);
}

void closeTargets() {
    if (tgtConfirm >= 0) {
        tgtConfirm = -1;
        forceRedraw = true;
        return;
    }
    tgtEdit = -1;
    setScreen(tgtBack == Screen::Targets ? Screen::Map : tgtBack);
}

void drawTargets() {
    if (tgtEdit >= 0) {
        uint32_t pass = 0;
        for (const char* p = osk::text(); *p; ++p) pass = pass * 33 + (uint8_t)*p;
        char ekey[24];
        snprintf(ekey, sizeof(ekey), "%d|%lu", tgtEdit, (unsigned long)pass);
        static char eshown[24];
        if (!forceRedraw && strcmp(eshown, ekey) == 0) return;
        strlcpy(eshown, ekey, sizeof(eshown));
        M5Canvas& c = wifiC;
        c.fillSprite(th->bg);
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextSize(1);
        c.setTextDatum(middle_left);
        c.setTextColor(th->fg);
        const targets::Target* t = targets::get(tgtEdit);
        char title[56];
        snprintf(title, sizeof(title), "Rename %s", t ? t->name : "");
        c.drawString(title, 24, 28);
        Palette pal{th->bg, th->fg, th->label, th->accent, th->dim, th->btn, th->btnText, th->warn, th->ok};
        osk::draw(c, 48, pal);
        fb::push(c, 0, BAR_H);
        return;
    }
    if (tgtConfirm >= 0 && millis() > tgtConfirmUntil) tgtConfirm = -1;
    if (tgtScroll > tgtMaxScroll()) tgtScroll = tgtMaxScroll();
    gps::Snapshot g = gps::get();
    bool have = g.lat != 0 || g.lon != 0;
    static char key[768];
    int pos = snprintf(key, sizeof(key), "%d|%d|%d|%d|", targets::selected(), tgtScroll, targets::count(),
                       tgtConfirm);
    for (int i = 0; i < targets::count() && pos < (int)sizeof(key) - 16; i++) {
        const targets::Target* t = targets::get(i);
        int metres = have && t ? (int)lroundf(targets::metresBetween(g.lat, g.lon, t->lat, t->lon)) : -1;
        pos += snprintf(key + pos, sizeof(key) - pos, "%s,%d;", t ? t->name : "", metres);
    }
    static char shown[768];
    if (!forceRedraw && strcmp(shown, key) == 0) return;
    strlcpy(shown, key, sizeof(shown));

    M5Canvas& c = wifiC;
    c.fillSprite(th->bg);
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextSize(1);
    c.setTextDatum(middle_left);
    c.setTextColor(th->fg);
    c.drawString("TARGETS", 24, 36);
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(middle_right);
    c.setTextColor(th->label);
    c.drawString("Hold the map to add", W - 24, 36);
    c.drawFastHLine(0, TGT_HEAD - 1, W, th->dim);

    int n = targets::count();
    if (n == 0) {
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextDatum(middle_left);
        c.setTextColor(th->label);
        c.drawString("No targets yet", 24, TGT_HEAD + 48);
    }
    for (int i = 0; i < n; i++) {
        int ry = TGT_HEAD + i * TGT_ROW - tgtScroll;
        if (ry < TGT_HEAD || ry >= c.height()) continue;
        const targets::Target* t = targets::get(i);
        if (!t) continue;
        c.drawFastHLine(0, ry + TGT_ROW - 1, W, th->dim);
        bool on = i == targets::selected();
        char dist[24] = "--";
        if (have) snprintf(dist, sizeof(dist), "%d m",
                           (int)lroundf(targets::metresBetween(g.lat, g.lon, t->lat, t->lon)));
        int rx = tgtRenameX();
        int dx = tgtDeleteX();
        int nameW = rx - 120;
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextDatum(middle_left);
        if (c.textWidth(t->name) > nameW) c.setFont(&fonts::FreeSansBold12pt7b);
        c.setTextColor(on ? th->accent : th->fg);
        c.setClipRect(16, ry, nameW, TGT_ROW);
        c.drawString(t->name, 24, ry + TGT_ROW / 2);
        c.clearClipRect();
        c.setFont(&fonts::FreeSansBold12pt7b);
        c.setTextDatum(middle_right);
        c.setTextColor(th->label);
        c.drawString(dist, rx - 16, ry + TGT_ROW / 2);
        auto textBtn = [&](int x, const char* label, bool armed) {
            c.fillSmoothRoundRect(x, ry + (TGT_ROW - 52) / 2, TGT_BTN_W, 52, 12, armed ? th->warn : th->btn);
            c.setTextDatum(middle_center);
            c.setTextColor(armed ? TFT_WHITE : th->btnText);
            c.drawString(label, x + TGT_BTN_W / 2, ry + TGT_ROW / 2);
        };
        textBtn(rx, "RENAME", false);
        textBtn(dx, "DELETE", i == tgtConfirm);
    }
    fb::push(c, 0, BAR_H);
}

void onTargetsTap(int x, int y) {
    int cy = y - BAR_H;
    if (tgtEdit >= 0) {
        Palette pal{th->bg, th->fg, th->label, th->accent, th->dim, th->btn, th->btnText, th->warn, th->ok};
        osk::Result r = osk::tap(x, cy, W, 48);
        if (r == osk::Result::Save) {
            if (targets::rename(tgtEdit, osk::text())) tgtEdit = -1;
            else showHint("A target needs a name");
        } else if (r == osk::Result::Cancel) {
            tgtEdit = -1;
        }
        forceRedraw = true;
        return;
    }
    if (cy < TGT_HEAD) return;
    int i = (cy - TGT_HEAD + tgtScroll) / TGT_ROW;
    if (!targets::get(i)) return;
    if (x >= tgtDeleteX()) {
        if (tgtConfirm == i && millis() <= tgtConfirmUntil) {
            tgtConfirm = -1;
            if (tgtEdit == i) tgtEdit = -1;
            targets::remove(i);
            if (tgtScroll > tgtMaxScroll()) tgtScroll = tgtMaxScroll();
            mapRedrawPending = true;
        } else {
            tgtConfirm = i;
            tgtConfirmUntil = millis() + 4000;
            showHint("Tap DELETE again to remove it");
        }
        forceRedraw = true;
        return;
    }
    tgtConfirm = -1;
    if (x >= tgtRenameX()) {
        const targets::Target* t = targets::get(i);
        tgtEdit = i;
        osk::start(t ? t->name : "", targets::NAME_LEN - 1);
        forceRedraw = true;
        return;
    }
    targets::select(targets::selected() == i ? -1 : i);
    if (targets::selected() >= 0) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Target %s", targets::selectedName());
        showHint(msg);
    }
    mapRedrawPending = true;
}

constexpr int SEG_HEAD = 80;
constexpr int SEG_BUILD_HEAD = 108;
constexpr int SEG_ROW = 90;
constexpr int SEG_ICON = 74;
int segScroll = 0;
int segDragY = 0;
int segScroll0 = 0;
int segRename = -1;   // segment whose name is on the keyboard, or -1
int segEdit = -1;     // segment being rebuilt, or -1 for a new one
int segConfirm = -1;
uint32_t segConfirmUntil = 0;
int segCopy = -1;  // segment waiting for same-order or reversed copy, or -1
bool segBuilding = false;

int segDeleteX() { return W - 20 - SEG_ICON; }
int segRenameX() { return segDeleteX() - 16 - SEG_ICON; }
int segEditX() { return segRenameX() - 16 - SEG_ICON; }
int segCopyX() { return segEditX() - 16 - SEG_ICON; }
int segActionX() { return W - 16 - 150; }

int segHead() { return segBuilding ? SEG_BUILD_HEAD : SEG_HEAD; }

int segMaxScroll() {
    int rows = segBuilding ? targets::count() : segments::count();
    int extra = rows * SEG_ROW - ((BTN_Y - BAR_H) - segHead());
    return extra > 0 ? extra : 0;
}

void openSegments() {
    if (screen == Screen::Segments) return;
    segBack = screen;
    segScroll = 0;
    segBuilding = false;
    segRename = -1;
    segConfirm = -1;
    segCopy = -1;
    setScreen(Screen::Segments);
}

void closeSegments() {
    if (segCopy >= 0) {
        segCopy = -1;
        forceRedraw = true;
        return;
    }
    if (segConfirm >= 0) {
        segConfirm = -1;
        forceRedraw = true;
        return;
    }
    if (segRename >= 0) {
        segRename = -1;
        forceRedraw = true;
        return;
    }
    if (segBuilding) {
        segBuilding = false;
        segScroll = 0;
        forceRedraw = true;
        return;
    }
    setScreen(segBack == Screen::Segments ? Screen::Map : segBack);
}

void startSegmentBuilder(int editIndex) {
    segBuilding = true;
    segEdit = editIndex;
    segDraftN = 0;
    segScroll = 0;
    if (editIndex >= 0) {
        const segments::Segment* s = segments::get(editIndex);
        if (s && growInts(segDraft, segDraftCap, s->count)) {
            segDraftN = s->count;
            for (int i = 0; i < segDraftN; i++) segDraft[i] = s->point[i];
        }
    }
    forceRedraw = true;
}

int draftOrder(int targetIndex) {
    for (int i = 0; i < segDraftN; i++)
        if (segDraft[i] == targetIndex) return i;
    return -1;
}

bool fingerOnMap(int sx, int sy, double& lat, double& lon, float& mpp) {
    int ox, oy;
    bool full;
    M5Canvas* cv = mapCanvasFor(ox, oy, full);
    if (!cv || !full) return false;
    mapview::View mv;
    const Settings& s = settings::get();
    gps::Snapshot g = gps::get();
    mv.lat = g.lat;
    mv.lon = g.lon;
    mv.course = g.course;
    mv.haveFix = g.fix;
    mv.orientation = s.mapOrientation;
    mv.followBoat = mapFollow;
    mv.centerLat = mapCenterLat;
    mv.centerLon = mapCenterLon;
    int zi = s.mapZoom < cfg::MAP_ZOOM_COUNT ? s.mapZoom : cfg::MAP_ZOOM_DEFAULT;
    mv.metersPerPixel = cfg::MAP_ZOOM_M[zi] / (float)cv->width();
    mpp = mv.metersPerPixel;
    return mapview::latLonAt(mv, cv->width(), cv->height(), (float)(sx - ox), (float)(sy - oy), lat, lon);
}

int nearestPoint(double lat, double lon, const int* ids, int nids, float radiusM) {
    int best = -1;
    float bestD = radiusM;
    for (int i = 0; i < nids; i++) {
        const targets::Target* t = targets::get(ids[i]);
        if (!t) continue;
        float d = targets::metresBetween(lat, lon, t->lat, t->lon);
        if (d <= bestD) {
            bestD = d;
            best = ids[i];
        }
    }
    return best;
}

int waypointUnder(int sx, int sy) {
    if (!segPlacing && !segMapEdit) return -1;
    double lat, lon;
    float mpp;
    if (!fingerOnMap(sx, sy, lat, lon, mpp)) return -1;
    // Editable waypoints are drawn at triple size, so the grab radius grows with them.
    float radius = fmaxf(16.f, 144.f * mpp);
    if (segPlacing) return nearestPoint(lat, lon, segDraft, segDraftN, radius);
    const segments::Segment* s = segments::selected() >= 0 ? segments::get(segments::selected()) : nullptr;
    if (!s || s->count < 1) return -1;
    int best = -1;
    float bestD = radius;
    for (int i = 0; i < s->count; i++) {
        const targets::Target* t = targets::get(s->point[i]);
        if (!t) continue;
        float d = targets::metresBetween(lat, lon, t->lat, t->lon);
        if (d <= bestD) {
            bestD = d;
            best = s->point[i];
        }
    }
    return best;
}

void discardMade() {
    bool again = true;
    while (again) {
        again = false;
        int best = -1, bestAt = -1;
        for (int i = 0; i < segMadeN; i++) {
            int idx = segMade[i];
            if (segments::usesTarget(idx)) continue;
            if (idx > best) {
                best = idx;
                bestAt = i;
            }
        }
        if (best < 0) break;
        targets::remove(best);
        for (int i = bestAt; i < segMadeN - 1; i++) segMade[i] = segMade[i + 1];
        segMadeN--;
        for (int i = 0; i < segMadeN; i++)
            if (segMade[i] > best) segMade[i]--;
        for (int i = 0; i < segDraftN; i++)
            if (segDraft[i] > best) segDraft[i]--;
        again = true;
    }
    segMadeN = 0;
    segDraftN = 0;
}

void cancelPlaceSegment() {
    if (!segPlacing) return;
    segPlacing = false;
    discardMade();
    int prev = segPlacePrev;
    segPlacePrev = -1;
    if (prev >= 0 && prev < segments::count()) segments::select(prev);
    mapRedrawPending = true;
}

void beginPlaceSegment() {
    segMapEdit = false;
    segPlacePrev = segments::selected();
    if (segPlacePrev >= 0) segments::select(-1);
    segPlacing = true;
    segBuilding = false;
    segDraftN = 0;
    segMadeN = 0;
    setScreen(Screen::Map);
    showHint("Tap the map to add a point");
    mapRedrawPending = true;
}

void undoSegmentPoint() {
    if (segDraftN <= 0) return;
    int idx = segDraft[segDraftN - 1];
    segDraftN--;
    int madeAt = -1;
    for (int i = 0; i < segMadeN; i++)
        if (segMade[i] == idx) madeAt = i;
    bool still = draftOrder(idx) >= 0;
    if (madeAt >= 0 && !still && !segments::usesTarget(idx)) {
        for (int i = madeAt; i < segMadeN - 1; i++) segMade[i] = segMade[i + 1];
        segMadeN--;
        targets::remove(idx);
        for (int i = 0; i < segDraftN; i++)
            if (segDraft[i] > idx) segDraft[i]--;
        for (int i = 0; i < segMadeN; i++)
            if (segMade[i] > idx) segMade[i]--;
    }
    mapRedrawPending = true;
    forceRedraw = true;
}

void savePlacedSegment() {
    if (segDraftN < 2) {
        showHint("Add a start and a finish");
        return;
    }
    if (!segments::add(segDraft, segDraftN)) {
        showHint("Could not save that segment");
        return;
    }
    segPlacing = false;
    segMadeN = 0;
    segDraftN = 0;
    segPlacePrev = -1;
    char msg[48];
    snprintf(msg, sizeof(msg), "Segment %s", segments::selectedName());
    showHint(msg);
    mapRedrawPending = true;
    forceRedraw = true;
}

void addSegmentPoint(int sx, int sy) {
    if (!growInts(segDraft, segDraftCap, segDraftN + 1)) {
        showHint("Could not save that point");
        return;
    }
    double lat, lon;
    float mpp;
    if (!fingerOnMap(sx, sy, lat, lon, mpp)) {
        showHint("No map to mark");
        return;
    }
    float radius = fmaxf(16.f, 48.f * mpp);
    int hit = -1;
    float bestD = radius;
    for (int i = 0; i < targets::count(); i++) {
        const targets::Target* t = targets::get(i);
        if (!t) continue;
        float d = targets::metresBetween(lat, lon, t->lat, t->lon);
        if (d <= bestD) {
            bestD = d;
            hit = i;
        }
    }
    if (hit < 0) {
        if (!targets::add(lat, lon)) {
            showHint("Could not save that point");
            return;
        }
        hit = targets::selected();
        if (hit < 0 || !growInts(segMade, segMadeCap, segMadeN + 1)) {
            if (hit >= 0) targets::remove(hit);
            showHint("Could not save that point");
            return;
        }
        segMade[segMadeN++] = hit;
    } else if (draftOrder(hit) >= 0) {
        showHint("Already in this segment");
        return;
    }
    segDraft[segDraftN++] = hit;
    if (segDraftN >= 2) {
        float len = segments::lengthAlong(segDraft, segDraftN);
        char msg[40];
        snprintf(msg, sizeof(msg), "%d points, %d m", segDraftN, len < 0 ? 0 : (int)lroundf(len));
        showHint(msg);
    } else {
        showHint("Tap the next point");
    }
    mapRedrawPending = true;
    forceRedraw = true;
}

void drawSegments() {
    if (segRename >= 0) {
        uint32_t pass = 0;
        for (const char* p = osk::text(); *p; ++p) pass = pass * 33 + (uint8_t)*p;
        char ekey[24];
        snprintf(ekey, sizeof(ekey), "R|%d|%lu", segRename, (unsigned long)pass);
        static char eshown[24];
        if (!forceRedraw && strcmp(eshown, ekey) == 0) return;
        strlcpy(eshown, ekey, sizeof(eshown));
        M5Canvas& c = wifiC;
        c.fillSprite(th->bg);
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextSize(1);
        c.setTextDatum(middle_left);
        c.setTextColor(th->fg);
        const segments::Segment* s = segments::get(segRename);
        char title[56];
        snprintf(title, sizeof(title), "Rename %s", s ? s->name : "");
        c.setClipRect(24, 0, W - 48, 56);
        c.drawString(title, 24, 28);
        c.clearClipRect();
        Palette pal{th->bg, th->fg, th->label, th->accent, th->dim, th->btn, th->btnText, th->warn, th->ok};
        osk::draw(c, 48, pal);
        fb::push(c, 0, BAR_H);
        return;
    }
    if (segCopy >= 0) {
        const segments::Segment* s = segments::get(segCopy);
        if (!s) {
            segCopy = -1;
        } else {
            static int shown = -2;
            if (!forceRedraw && shown == segCopy) return;
            shown = segCopy;
            M5Canvas& c = wifiC;
            c.fillSprite(th->bg);
            c.setTextSize(1);
            c.setFont(&fonts::FreeSansBold18pt7b);
            c.setTextDatum(middle_left);
            c.setTextColor(th->fg);
            char title[56];
            snprintf(title, sizeof(title), "Copy %s", s->name);
            c.setClipRect(24, 0, W - 48, 64);
            c.drawString(title, 24, 36);
            c.clearClipRect();
            c.setFont(&fonts::FreeSansBold12pt7b);
            c.setTextColor(th->label);
            c.drawString("Same order keeps this start and finish.", 24, 120);
            c.drawString("Opposite runs the course back the other way.", 24, 156);
            auto choice = [&](int x, const char* label) {
                c.fillSmoothRoundRect(x, 220, 460, 88, 16, th->btn);
                c.setTextDatum(middle_center);
                c.setFont(&fonts::FreeSansBold18pt7b);
                c.setTextColor(th->btnText);
                c.drawString(label, x + 230, 264);
            };
            int sameX = (W - 460 * 2 - 24) / 2;
            choice(sameX, "SAME ORDER");
            choice(sameX + 460 + 24, "OPPOSITE");
            fb::push(c, 0, BAR_H);
            return;
        }
    }

    if (segConfirm >= 0 && millis() > segConfirmUntil) segConfirm = -1;
    if (segScroll > segMaxScroll()) segScroll = segMaxScroll();
    static char key[1024];
    int pos = snprintf(key, sizeof(key), "%c|%d|%d|%d|%d|", segBuilding ? 'B' : 'L', segScroll,
                       segments::selected(), segDraftN, segConfirm);
    if (segBuilding) {
        float len = segments::lengthAlong(segDraft, segDraftN);
        pos += snprintf(key + pos, sizeof(key) - pos, "%d|", len < 0 ? -1 : (int)lroundf(len));
        for (int i = 0; i < targets::count() && pos < (int)sizeof(key) - 48; i++) {
            const targets::Target* t = targets::get(i);
            pos += snprintf(key + pos, sizeof(key) - pos, "%d:%s;", draftOrder(i), t ? t->name : "");
        }
    } else {
        for (int i = 0; i < segments::count() && pos < (int)sizeof(key) - 56; i++) {
            const segments::Segment* s = segments::get(i);
            float len = segments::length(i);
            pos += snprintf(key + pos, sizeof(key) - pos, "%s,%d;", s ? s->name : "",
                            len < 0 ? -1 : (int)lroundf(len));
        }
    }
    static char shown[1024];
    if (!forceRedraw && strcmp(shown, key) == 0) return;
    strlcpy(shown, key, sizeof(shown));

    M5Canvas& c = wifiC;
    c.fillSprite(th->bg);
    c.setTextSize(1);
    int ax = segActionX();
    if (segBuilding) {
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextDatum(middle_left);
        c.setTextColor(th->fg);
        const segments::Segment* editing = segEdit >= 0 ? segments::get(segEdit) : nullptr;
        const char* title = editing ? editing->name : "New segment";
        c.setClipRect(24, 0, ax - 36, 64);
        c.drawString(title, 24, 32);
        c.clearClipRect();
        float len = segments::lengthAlong(segDraft, segDraftN);
        c.setFont(&fonts::FreeSansBold12pt7b);
        c.setTextColor(th->label);
        if (len >= 0) {
            char sub[40];
            snprintf(sub, sizeof(sub), "%d m along the points", (int)lroundf(len));
            c.drawString(sub, 24, 78);
        } else {
            c.drawString("Tap in order. First is the start, last is the finish.", 24, 78);
        }
        bool ready = segDraftN >= 2;
        c.fillSmoothRoundRect(ax, 12, 150, 52, 12, ready ? th->accent : th->btn);
        c.setTextDatum(middle_center);
        c.setTextColor(ready ? th->bg : th->dim);
        c.drawString("SAVE", ax + 75, 38);
    } else {
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextDatum(middle_left);
        c.setTextColor(th->fg);
        c.drawString("SEGMENTS", 24, 36);
        c.fillSmoothRoundRect(ax, 10, 150, 52, 12, th->btn);
        c.setFont(&fonts::FreeSansBold12pt7b);
        c.setTextDatum(middle_center);
        c.setTextColor(th->btnText);
        c.drawString("NEW", ax + 75, 36);
    }
    c.drawFastHLine(0, segHead() - 1, W, th->dim);

    auto iconBtn = [&](int x, int ry, icons::Icon icon, bool armed) {
        int y = ry + (SEG_ROW - SEG_ICON) / 2;
        uint16_t fill = armed ? th->warn : th->btn;
        c.fillSmoothRoundRect(x, y, SEG_ICON, SEG_ICON, 16, fill);
        icons::draw(c, icon, x + SEG_ICON / 2, y + SEG_ICON / 2, 36, armed ? TFT_WHITE : th->btnText, fill);
    };

    if (segBuilding) {
        int nt = targets::count();
        if (nt == 0) {
            c.setFont(&fonts::FreeSansBold18pt7b);
            c.setTextDatum(middle_left);
            c.setTextColor(th->label);
            c.drawString("Drop targets on the map first", 24, SEG_BUILD_HEAD + 48);
        }
        for (int i = 0; i < nt; i++) {
            int ry = SEG_BUILD_HEAD + i * SEG_ROW - segScroll;
            if (ry + SEG_ROW <= SEG_BUILD_HEAD || ry >= c.height()) continue;
            const targets::Target* t = targets::get(i);
            if (!t) continue;
            c.drawFastHLine(0, ry + SEG_ROW - 1, W, th->dim);
            int order = draftOrder(i);
            const char* role = "";
            if (order == 0) role = "START";
            else if (order > 0 && order == segDraftN - 1) role = "FINISH";
            else if (order > 0) role = "VIA";
            c.setFont(&fonts::FreeSansBold18pt7b);
            c.setTextDatum(middle_left);
            c.setTextColor(order >= 0 ? th->accent : th->fg);
            if (order >= 0) {
                char num[4];
                snprintf(num, sizeof(num), "%d", order + 1);
                c.drawString(num, 24, ry + SEG_ROW / 2);
            }
            int nameW = W - 160;
            if (c.textWidth(t->name) > nameW - 72) c.setFont(&fonts::FreeSansBold12pt7b);
            c.setClipRect(64, ry, nameW - 64, SEG_ROW);
            c.drawString(t->name, 64, ry + SEG_ROW / 2);
            c.clearClipRect();
            if (role[0]) {
                c.setFont(&fonts::FreeSansBold12pt7b);
                c.setTextDatum(middle_right);
                c.setTextColor(th->label);
                c.drawString(role, W - 24, ry + SEG_ROW / 2);
            }
        }
    } else {
        int ns = segments::count();
        if (ns == 0) {
            c.setFont(&fonts::FreeSansBold18pt7b);
            c.setTextDatum(middle_left);
            c.setTextColor(th->label);
            c.drawString("No segments yet", 24, SEG_HEAD + 48);
        }
        for (int i = 0; i < ns; i++) {
            int ry = SEG_HEAD + i * SEG_ROW - segScroll;
            if (ry + SEG_ROW <= SEG_HEAD || ry >= c.height()) continue;
            const segments::Segment* s = segments::get(i);
            if (!s) continue;
            c.drawFastHLine(0, ry + SEG_ROW - 1, W, th->dim);
            bool on = i == segments::selected();
            float len = segments::length(i);
            char dist[24] = "--";
            if (len >= 0) snprintf(dist, sizeof(dist), "%d m along", (int)lroundf(len));
            int ex = segEditX();
            int cxb = segCopyX();
            c.setFont(&fonts::FreeSansBold18pt7b);
            c.setTextDatum(middle_left);
            int nameW = cxb - 200;
            if (s->name[0] && c.textWidth(s->name) > nameW) c.setFont(&fonts::FreeSansBold12pt7b);
            c.setTextColor(on ? th->accent : th->fg);
            c.setClipRect(16, ry, nameW, SEG_ROW);
            c.drawString(s->name, 24, ry + SEG_ROW / 2);
            c.clearClipRect();
            c.setFont(&fonts::FreeSansBold12pt7b);
            c.setTextDatum(middle_right);
            c.setTextColor(th->label);
            c.drawString(dist, cxb - 12, ry + SEG_ROW / 2);
            iconBtn(cxb, ry, icons::Icon::Copy, false);
            iconBtn(ex, ry, icons::Icon::Route, false);
            iconBtn(segRenameX(), ry, icons::Icon::Pencil, false);
            iconBtn(segDeleteX(), ry, icons::Icon::Trash, i == segConfirm);
        }
    }
    fb::push(c, 0, BAR_H);
}

void onSegmentsTap(int x, int y) {
    int cy = y - BAR_H;
    if (segCopy >= 0) {
        int sameX = (W - 460 * 2 - 24) / 2;
        bool same = x >= sameX && x < sameX + 460 && cy >= 220 && cy < 308;
        bool opp = x >= sameX + 484 && x < sameX + 944 && cy >= 220 && cy < 308;
        if (same || opp) {
            int src = segCopy;
            segCopy = -1;
            if (!segments::duplicate(src, opp)) showHint("Could not save that segment");
            else {
                segScroll = segMaxScroll();
                char msg[48];
                snprintf(msg, sizeof(msg), "Segment %s", segments::selectedName());
                showHint(msg);
                mapRedrawPending = true;
            }
        }
        forceRedraw = true;
        return;
    }
    if (segRename >= 0) {
        Palette pal{th->bg, th->fg, th->label, th->accent, th->dim, th->btn, th->btnText, th->warn, th->ok};
        osk::Result r = osk::tap(x, cy, W, 48);
        if (r == osk::Result::Save) {
            if (segments::rename(segRename, osk::text())) segRename = -1;
            else showHint("A segment needs a name");
        } else if (r == osk::Result::Cancel) {
            segRename = -1;
        }
        forceRedraw = true;
        return;
    }
    if (segBuilding) {
        if (cy >= 12 && cy < 64 && x >= segActionX()) {
            if (segDraftN < 2) {
                showHint("Add a start and a finish");
                return;
            }
            bool ok = segEdit >= 0 ? segments::update(segEdit, segDraft, segDraftN)
                                   : segments::add(segDraft, segDraftN);
            if (!ok) {
                showHint(segEdit >= 0 ? "That segment needs two targets" : "Could not save that segment");
                return;
            }
            segBuilding = false;
            segScroll = 0;
            if (segments::selected() >= 0) {
                char msg[48];
                snprintf(msg, sizeof(msg), "Segment %s", segments::selectedName());
                showHint(msg);
            }
            mapRedrawPending = true;
            forceRedraw = true;
            return;
        }
        if (cy < SEG_BUILD_HEAD) return;
        int i = (cy - SEG_BUILD_HEAD + segScroll) / SEG_ROW;
        if (!targets::get(i)) return;
        int order = draftOrder(i);
        if (order >= 0) {
            for (int k = order; k < segDraftN - 1; k++) segDraft[k] = segDraft[k + 1];
            segDraftN--;
        } else if (growInts(segDraft, segDraftCap, segDraftN + 1)) {
            segDraft[segDraftN++] = i;
        }
        forceRedraw = true;
        return;
    }
    if (cy >= 10 && cy < 62 && x >= segActionX()) {
        beginPlaceSegment();
        return;
    }
    if (cy < SEG_HEAD) return;
    int i = (cy - SEG_HEAD + segScroll) / SEG_ROW;
    if (!segments::get(i)) return;
    if (x >= segDeleteX()) {
        if (segConfirm == i && millis() <= segConfirmUntil) {
            segConfirm = -1;
            segments::remove(i);
            if (segScroll > segMaxScroll()) segScroll = segMaxScroll();
            mapRedrawPending = true;
        } else {
            segConfirm = i;
            segConfirmUntil = millis() + 4000;
            showHint("Tap the red button again to remove it");
        }
        forceRedraw = true;
        return;
    }
    segConfirm = -1;
    if (x >= segRenameX()) {
        const segments::Segment* s = segments::get(i);
        segRename = i;
        osk::start(s ? s->name : "", segments::NAME_LEN - 1);
        forceRedraw = true;
        return;
    }
    if (x >= segEditX()) {
        startSegmentBuilder(i);
        return;
    }
    if (x >= segCopyX()) {
        segCopy = i;
        segConfirm = -1;
        forceRedraw = true;
        return;
    }
    segMapEdit = false;
    segments::select(segments::selected() == i ? -1 : i);
    if (segments::selected() >= 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Segment %s", segments::selectedName());
        showHint(msg);
    }
    mapRedrawPending = true;
    forceRedraw = true;
}

void dismissRace() {
    race.open = false;
    mapRedrawPending = true;
    forceRedraw = true;
}

void beginRace() {
    logger::setSessionLabel(segments::selectedName());
    metrics::start();
    race.live = true;
    race.segment = segments::selected();
    strlcpy(race.name, segments::selectedName(), sizeof(race.name));
    race.course = segments::length(race.segment);
    metrics::note("kind", "race");
    metrics::note("segment", race.name);
    char metres[16];
    if (race.course >= 0) {
        snprintf(metres, sizeof(metres), "%.0f", race.course);
        metrics::note("course_m", metres);
    }
    showHint("Started");
}

void finishRace() {
    metrics::update();
    metrics::View v = metrics::view();
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", v.elapsed);
    metrics::note("race_time_s", buf);
    snprintf(buf, sizeof(buf), "%.1f", v.distance);
    metrics::note("race_distance_m", buf);
    int session = v.session;
    metrics::stop();
    race.live = false;
    race.elapsed = v.elapsed;
    race.distance = v.distance;
    race.avgSplit = v.avgSplit;
    race.avgRate = v.avgRate;
    race.strokes = v.strokes;
    race.avgHr = v.avgHr;
    race.maxHr = v.maxHr;
    if (session >= 0) snprintf(race.saved, sizeof(race.saved), "S%04d", session);
    else snprintf(race.saved, sizeof(race.saved), "NO CARD");
    race.open = true;
    forceRedraw = true;
    Serial.printf("[race] finish %s  %.1f s  %.0f m  session %d\n", race.name, race.elapsed, race.distance, session);
}

void drawRaceModal() {
    M5Canvas& c = wifiC;
    c.fillSprite(th->bg);
    c.setTextSize(1);
    c.setTextDatum(top_left);
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextColor(th->accent);
    c.drawString("RACE", 28, 10);
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.setTextColor(th->fg);
    c.setClipRect(28, 34, W - 56, 36);
    c.drawString(race.name[0] ? race.name : "Segment", 28, 34);
    c.clearClipRect();

    c.fillRoundRect(24, 78, W - 48, 132, 18, th->btn);
    c.drawRoundRect(24, 78, W - 48, 132, 18, th->dim);
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextColor(th->label);
    c.drawString("TIME", 48, 94);
    char time[16];
    fmtTime(time, sizeof(time), race.elapsed);
    c.setFont(&fonts::FreeSansBold24pt7b);
    c.setTextColor(th->accent);
    c.setTextDatum(middle_left);
    c.drawString(time, 48, 156);

    auto card = [&](int x, int y, const char* label, const char* value, const char* unit) {
        constexpr int cw = 400, ch = 140;
        c.fillRoundRect(x, y, cw, ch, 18, th->btn);
        c.drawRoundRect(x, y, cw, ch, 18, th->dim);
        c.setFont(&fonts::FreeSansBold12pt7b);
        c.setTextDatum(top_left);
        c.setTextColor(th->label);
        c.drawString(label, x + 20, y + 16);
        if (unit && unit[0]) {
            c.setTextDatum(top_right);
            int uw = c.textWidth(unit);
            int maxW = cw - 48 - c.textWidth(label);
            if (uw > maxW && maxW > 8) c.setClipRect(x + cw - 20 - maxW, y, maxW + 20, 40);
            c.drawString(unit, x + cw - 20, y + 16);
            c.clearClipRect();
        }
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextDatum(middle_left);
        c.setTextColor(th->fg);
        c.drawString(value, x + 20, y + 88);
    };

    char dist[16], split[16], rate[16], strokes[16], hr[16];
    char distUnit[24] = "m", hrUnit[24] = "bpm";
    snprintf(dist, sizeof(dist), "%d", (int)lroundf(race.distance));
    if (race.course >= 0) snprintf(distUnit, sizeof(distUnit), "course %d m", (int)lroundf(race.course));
    fmtSplit(split, sizeof(split), race.avgSplit);
    if (race.avgRate > 0) snprintf(rate, sizeof(rate), "%.1f", race.avgRate);
    else snprintf(rate, sizeof(rate), "--");
    snprintf(strokes, sizeof(strokes), "%d", race.strokes);
    if (race.avgHr > 0) snprintf(hr, sizeof(hr), "%.0f", race.avgHr);
    else snprintf(hr, sizeof(hr), "--");
    if (race.maxHr > 0) snprintf(hrUnit, sizeof(hrUnit), "max %d", race.maxHr);

    constexpr int x0 = 24, x1 = 440, x2 = 856, y0 = 226, y1 = 382;
    card(x0, y0, "DISTANCE", dist, distUnit);
    card(x1, y0, "AVG SPLIT", split, "/500m");
    card(x2, y0, "AVG RATE", rate, "spm");
    card(x0, y1, "STROKES", strokes, "");
    card(x1, y1, "AVG HR", hr, hrUnit);
    card(x2, y1, "SAVED", race.saved, "session");
    fb::push(c, 0, BAR_H);
}

void nextScreen() {
    if (race.open) {
        dismissRace();
        return;
    }
    if (screen == Screen::Gps) {
        closeGps();
        return;
    }
    if (screen == Screen::Targets) {
        closeTargets();
        return;
    }
    if (screen == Screen::Segments) {
        closeSegments();
        return;
    }
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

void showChosenMap() {
    mapFollow = false;
    if (!mapdata::centerLatLon(mapCenterLat, mapCenterLon)) return;
    setScreen(Screen::Map);
    char msg[48];
    snprintf(msg, sizeof(msg), "Map %s", mapdata::current().name);
    showHint(msg);
    mapRedrawPending = true;
}

void onMapsTap(int x, int y) {
    if (y < BTN_Y) {
        mapsui::tap(x, y);
        if (mapsui::takeShown()) showChosenMap();
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
    if (race.open) {
        if (y >= BTN_Y && x / BTN_W == 4) dismissRace();
        return;
    }
    // Centre of the top bar is the GPS status. Tap it for diagnostics.
    if (y < BAR_H && x >= 220 && x < 1000) {
        if (screen == Screen::Gps) closeGps();
        else openGps();
        return;
    }
    if (screen == Screen::Maps) {
        onMapsTap(x, y);
        return;
    }
    if (screen == Screen::Sessions && y < BTN_Y) {
        sessionsui::tap(x, y);
        forceRedraw = true;
        return;
    }
    if (screen == Screen::Targets && y < BTN_Y) {
        onTargetsTap(x, y);
        forceRedraw = true;
        return;
    }
    if (screen == Screen::Segments && y < BTN_Y) {
        onSegmentsTap(x, y);
        forceRedraw = true;
        return;
    }
    metrics::View v = metrics::view();
    if (screen == Screen::Map && mapsButtonShown() && x >= MAPS_BTN_X && x < MAPS_BTN_X + MAPS_BTN_W &&
        y >= FULL_CAM_Y + MAPS_BTN_Y && y < FULL_CAM_Y + MAPS_BTN_Y + MAPS_BTN_H) {
        setScreen(Screen::Maps);
        return;
    }
    if (targetsButtonHit(x, y)) {
        openTargets();
        return;
    }
    if (segmentsButtonHit(x, y)) {
        if (segPlacing) cancelPlaceSegment();
        openSegments();
        return;
    }
    if (editButtonHit(x, y)) {
        segMapEdit = !segMapEdit;
        showHint(segMapEdit ? "Drag a point to move it" : "Looking at the course");
        mapRedrawPending = true;
        forceRedraw = true;
        return;
    }
    if (nearStartHit(x, y)) {
        segMapEdit = false;
        segments::select(nearIdx);
        char msg[48];
        snprintf(msg, sizeof(msg), "Segment %s", segments::selectedName());
        showHint(msg);
        mapRedrawPending = true;
        forceRedraw = true;
        return;
    }
    if (placeButtonHit(x, y, false)) {
        undoSegmentPoint();
        return;
    }
    if (placeButtonHit(x, y, true)) {
        savePlacedSegment();
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
                else if (screen == Screen::Stroke) {
                    Settings& st = settings::get();
                    st.crew = st.crew == 1 ? 2 : st.crew == 2 ? 4 : st.crew == 4 ? 8 : 1;
                    settings::save();
                    char msg[24];
                    snprintf(msg, sizeof(msg), "Crew of %d", st.crew);
                    showHint(msg);
                    forceRedraw = true;
                } else if (screen == Screen::Sessions) {
                    sessionsui::back();
                    forceRedraw = true;
                }
                break;
            case 4: nextScreen(); break;
        }
        return;
    }
    if (handleMapControlTap(x, y)) return;
    if (segPlacing && screen == Screen::Map && y >= FULL_CAM_Y && y < BTN_Y && !mapControlHit(x, y)) {
        addSegmentPoint(x, y);
        return;
    }
    int tileLeft = 0, tileW = 0;
    int slot = tileSlotAt(x, y, &tileLeft, &tileW);
    if (slot >= 0) {
        cycleTile(slot, x < tileLeft + tileW / 2 ? -1 : 1);
        return;
    }
    if (screen == Screen::Map) return;  // full map: controls and drag, not a tap action
    if (inContentRect(x, y)) togglePipFull();
    if (screen == Screen::Settings) onSettingsTap(x, y);
}

void onHold(int x, int y) {
    if (race.open) return;
    if (mapFullShown() && y >= FULL_CAM_Y && y < BTN_Y && !mapControlHit(x, y)) {
        if (segPlacing && screen == Screen::Map) addSegmentPoint(x, y);
        else addTargetAt(x, y - FULL_CAM_Y);
        return;
    }
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
    if (idx == 3 && screen == Screen::Stroke) {
        anatomy::reset();
        showHint("Stroke reference cleared");
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
    } else if (screen == Screen::Settings && settingsTab == TAB_WIFI && !wifiEditing &&
               y >= BAR_H + wifiListTop()) {
        dragKind = DragKind::Wifi;
        wifiDragY = y;
        wifiScroll0 = wifiScroll;
    } else if (screen == Screen::Targets && tgtEdit < 0 && y >= BAR_H + TGT_HEAD) {
        dragKind = DragKind::Targets;
        tgtDragY = y;
        tgtScroll0 = tgtScroll;
    } else if (screen == Screen::Segments && segRename < 0 && y >= BAR_H + segHead()) {
        dragKind = DragKind::Segments;
        segDragY = y;
        segScroll0 = segScroll;
    } else if (mapFullShown() && y >= FULL_CAM_Y && y < BTN_Y && !mapControlHit(x, y)) {
        int wp = waypointUnder(x, y);
        if (wp >= 0) {
            dragKind = DragKind::Waypoint;
            wpDrag = wp;
            wpHoldReady = false;
            wpCaptureThis = false;
            wpSightMoved = false;
            wpBasePending = wpHold != nullptr;
            if (wpBasePending) {
                mapRedrawPending = true;
                lastMap = 0;
            }
            return;
        }
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
    if (dragKind == DragKind::Wifi) {
        int next = wifiScroll0 - (y - wifiDragY);
        if (next < 0) next = 0;
        int cap = wifiMaxScroll();
        wifiScroll = next > cap ? cap : next;
        return;
    }
    if (dragKind == DragKind::Targets) {
        int next = tgtScroll0 - (y - tgtDragY);
        if (next < 0) next = 0;
        int cap = tgtMaxScroll();
        tgtScroll = next > cap ? cap : next;
        return;
    }
    if (dragKind == DragKind::Segments) {
        int next = segScroll0 - (y - segDragY);
        if (next < 0) next = 0;
        int cap = segMaxScroll();
        segScroll = next > cap ? cap : next;
        return;
    }
    if (dragKind == DragKind::Lists) {
        if (screen == Screen::Maps) mapsui::dragMove(x, y);
        else sessionsui::dragMove(x, y);
        return;
    }
    if (dragKind == DragKind::Waypoint) {
        double lat, lon;
        float mpp;
        if (fingerOnMap(x, y - WP_LIFT_PX, lat, lon, mpp)) {
            targets::move(wpDrag, lat, lon, false);
            if (wpHoldReady) wpSightMoved = true;
            else if (!wpBasePending) {
                mapRedrawPending = true;
                lastMap = 0;
            }
        }
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
    } else if (dragKind == DragKind::Waypoint) {
        dragMove(x, y);
        const targets::Target* t = targets::get(wpDrag);
        if (t) targets::move(wpDrag, t->lat, t->lon, true);
        wpDrag = -1;
        wpHoldReady = false;
        wpBasePending = false;
        wpCaptureThis = false;
        wpSightMoved = false;
        mapRedrawPending = true;
        lastMap = 0;
    }
    dragKind = DragKind::None;
}

void handleDrag(const m5::touch_detail_t& t) {
    if (race.open) return;
    bool wifiList = screen == Screen::Settings && settingsTab == TAB_WIFI && !wifiEditing;
    bool tgtList = screen == Screen::Targets;
    bool segList = screen == Screen::Segments;
    if (screen != Screen::Maps && screen != Screen::Sessions && !mapFullShown() && !wifiList && !tgtList && !segList)
        return;
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

    for (M5Canvas* c : {&bigC, &smallC, &barC, &rowC, &plotC, &btnC, &setC, &setTabC, &wifiC, &mapC, &mapPipC, &effC, &effP}) {
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
    wifiC.createSprite(W, BTN_Y - BAR_H);
    mapC.createSprite(W, FULL_CAM_H);
    wpHoldBytes = (size_t)mapC.width() * mapC.height() * 2;
    wpHold = (uint8_t*)heap_caps_malloc(wpHoldBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wpHold) wpHold = (uint8_t*)malloc(wpHoldBytes);
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
    bool wifiList = screen == Screen::Settings && settingsTab == TAB_WIFI && !wifiEditing;
    bool tgtList = screen == Screen::Targets;
    bool segList = screen == Screen::Segments;
    if (screen != Screen::Maps && screen != Screen::Sessions && !mapFullShown() && !wifiList && !tgtList && !segList)
        return;
    dragStart(x0, y0);
    for (int i = 1; i <= 8; i++) dragMove(x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8);
    dragEnd(x1, y1);
}

void command(char c) {
    if (race.open) {
        if (c == 'N') dismissRace();
        return;
    }
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
        case 'g': openGps(); break;
        case 't': openTargets(); break;
        case 's': openSegments(); break;
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
        // Tab5 is a 2S pack, so this is the pack voltage in millivolts. With no
        // pack on USB the node collapses near 0 V and also sits on the charger
        // rail, and the percentage clamps to 0 and 100 in turn.
        int packMv;
        {
            board::I2CLock lock;
            packMv = M5.Power.getBatteryVoltage();
        }
        static int presentRun = 0;
        static bool usbOnly = false;
        if (packMv < 5500) {
            presentRun = 0;
            usbOnly = true;
        } else if (packMv >= 6000 && packMv <= 8800) {
            presentRun++;
            // A missing pack also rests on the 8.4 V rail between collapses.
            // A mid-range voltage is a pack; the rail has to hold.
            int need = (usbOnly && packMv >= 8100) ? 6 : 2;
            if (presentRun >= need) usbOnly = false;
        } else {
            presentRun = 0;
        }
        if (usbOnly) {
            metrics::setBattery(-2);
        } else if (presentRun >= 2) {
            int cell = packMv / 2;
            int level = (int)lroundf((cell - 3300) * 100.0f / (4150 - 3350));
            if (level < 0) level = 0;
            if (level > 100) level = 100;
            metrics::setBattery(level);
        }
        static int batMode = 0;
        int mode = usbOnly ? 1 : (presentRun >= 2 ? 2 : 0);
        if (mode && mode != batMode) {
            Serial.printf("[bat] %s (%d mV)\n", mode == 1 ? "USB" : "pack", packMv);
            batMode = mode;
        }
    }

    // Keep the loaded map area in sync with position, and record the track.
    if (millis() - lastMapScan > 1000 && !sessionsui::reviewing()) {
        lastMapScan = millis();
        gps::Snapshot g = gps::get();
        mapdata::update(g.lat, g.lon, g.fix);
        if (g.lat != 0 || g.lon != 0) segments::track(g.lat, g.lon);
    }
    track::update();

    if (!race.open) drawCamera();
    drawMapContent();
    if (!race.open && screen == Screen::Maps) mapsui::draw(forceRedraw);
    if (!race.open && screen == Screen::Sessions) sessionsui::draw(forceRedraw);

    // A selected segment starts the piece on its start line and stops it on the finish line.
    {
        gps::Snapshot g = gps::get();
        bool pos = g.lat != 0 || g.lon != 0;
        bool crossed = segments::selected() >= 0 && pos &&
                       segments::pollStart(g.lat, g.lon, g.speed, g.course, g.distance, g.fix);
        if (crossed && !race.open && metrics::view().state != metrics::State::Running) beginRace();
        if (race.live && segments::selected() == race.segment && pos &&
            metrics::view().state == metrics::State::Running &&
            segments::pollFinish(g.lat, g.lon, g.speed, g.course, g.distance, g.fix))
            finishRace();
        if (race.live && metrics::view().state != metrics::State::Running) race.live = false;
    }

    if (millis() - lastFrame < cfg::UI_FRAME_MS && !forceRedraw) return;
    lastFrame = millis();

    metrics::View v = metrics::view();
    drawTopBar(v);
    if (!race.open) {
        switch (screen) {
            case Screen::Main: drawMain(v); break;
            case Screen::Map: drawTopTiles(v); break;
            case Screen::Efficiency: drawEfficiency(v); break;
            case Screen::Stroke: drawStroke(v); break;
            case Screen::Plot: drawPlot(v); break;
            case Screen::Settings: drawSettings(); break;
            case Screen::Sessions:
            case Screen::Maps: break;
            case Screen::Gps: drawGps(); break;
            case Screen::Targets: drawTargets(); break;
            case Screen::Segments: drawSegments(); break;
        }
    }
    drawButtons(v);
    if (race.open) drawRaceModal();
    forceRedraw = false;
}

}  // namespace ui
