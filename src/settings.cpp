#include "settings.h"

#include <Preferences.h>

#include "config.h"
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
    prefs.end();
}

}  // namespace settings
