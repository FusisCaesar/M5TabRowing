#include "settings.h"

#include <Preferences.h>
#include <string.h>

#include "config.h"
#include "i18n.h"
#include "map_view.h"

namespace settings {

namespace {
Settings s;
Preferences prefs;
constexpr const char* NS = "rowing";
}  // namespace

void begin() {
    prefs.begin(NS, false);
    s.thresholdK = prefs.getFloat("thrK", cfg::THRESHOLD_K_DEFAULT);
    s.thresholdFloorG = prefs.getFloat("thrFloor", cfg::THRESHOLD_FLOOR_G_DEFAULT);
    s.useMinimaThreshold = prefs.getBool("minima", false);
    s.rawLog = prefs.getBool("rawLog", true);
    s.fusion = prefs.getBool("fusion", false);
    s.dayTheme = prefs.getBool("day", false);
    s.flip = prefs.getBool("flip", false);
    s.brightness = prefs.getUChar("bright", 200);
    s.camFull = prefs.getBool("camFull", false);
    s.camMirror = prefs.getBool("camMirror", true);
    s.pipMode = prefs.getUChar("pipMode", 0);
    s.mapZoom = prefs.getUChar("mapZoom", cfg::MAP_ZOOM_DEFAULT);
    if (prefs.isKey("mapOrient")) {
        s.mapOrientation = prefs.getUChar("mapOrient", mapview::HeadUp);
        if (s.mapOrientation >= mapview::ORIENTATION_COUNT) s.mapOrientation = mapview::HeadUp;
    } else {
        // Old HEAD UP was course-up, which is now Bow Up, so the map does not flip 180°.
        s.mapOrientation = prefs.getBool("mapHeadUp", true) ? mapview::BowUp : mapview::NorthUp;
    }
    s.ratioShift = prefs.getFloat("ratioShift", 0.0f);
    s.crew = prefs.getUChar("crew", 1);
    if (s.crew != 1 && s.crew != 2 && s.crew != 4 && s.crew != 8) s.crew = 1;
    s.lang = prefs.getUChar("lang", (uint8_t)Lang::En);
    if (s.lang >= (uint8_t)Lang::Count) s.lang = (uint8_t)Lang::En;
    i18n::set((Lang)s.lang);
    static const uint8_t TILE_DEFAULT[TILE_SLOTS] = {
        (uint8_t)TileMetric::Split,     (uint8_t)TileMetric::Rate,      (uint8_t)TileMetric::Distance,
        (uint8_t)TileMetric::Time,      (uint8_t)TileMetric::DistStroke, (uint8_t)TileMetric::AvgSplit,
        (uint8_t)TileMetric::HeartRate, (uint8_t)TileMetric::RecDrive,
    };
    memcpy(s.tiles, TILE_DEFAULT, TILE_SLOTS);
    prefs.getBytes("tiles", s.tiles, TILE_SLOTS);
    for (int i = 0; i < TILE_SLOTS; i++)
        if (s.tiles[i] >= (uint8_t)TileMetric::Count) s.tiles[i] = TILE_DEFAULT[i];
    prefs.end();
}

Settings& get() { return s; }

void save() {
    prefs.begin(NS, false);
    prefs.putFloat("thrK", s.thresholdK);
    prefs.putFloat("thrFloor", s.thresholdFloorG);
    prefs.putBool("minima", s.useMinimaThreshold);
    prefs.putBool("rawLog", s.rawLog);
    prefs.putBool("fusion", s.fusion);
    prefs.putBool("day", s.dayTheme);
    prefs.putBool("flip", s.flip);
    prefs.putUChar("bright", s.brightness);
    prefs.putBool("camFull", s.camFull);
    prefs.putBool("camMirror", s.camMirror);
    prefs.putUChar("pipMode", s.pipMode);
    prefs.putUChar("mapZoom", s.mapZoom);
    prefs.putUChar("mapOrient", s.mapOrientation);
    prefs.putFloat("ratioShift", s.ratioShift);
    prefs.putUChar("crew", s.crew);
    prefs.putUChar("lang", s.lang);
    prefs.putBytes("tiles", s.tiles, TILE_SLOTS);
    prefs.end();
}

}  // namespace settings
