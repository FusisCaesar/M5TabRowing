#pragma once

#include <stdint.h>

#include "stroke_detector.h"

// One of these in each of the eight Main-screen slots; tap a tile to cycle.
enum class TileMetric : uint8_t {
    Split = 0,
    Rate,
    Distance,
    Time,
    DistStroke,
    AvgSplit,
    AvgRate,
    Strokes,
    HeartRate,
    AvgHr,
    RecDrive,
    Speed,
    Dtt,      // metres to the selected target, or remaining along the selected segment
    Ttt,      // that distance at the current speed
    Finish,   // that distance at the average split
    Count
};
constexpr int TILE_SLOTS = 8;

struct Settings {
    float thresholdK;
    float thresholdFloorG;
    bool useMinimaThreshold;  // not offered any more; the automatic threshold replaces it
    bool autoThreshold;       // the catch threshold follows the signal; K and the floor are then unused
    bool rawLog;
    bool fusion;
    bool dayTheme;
    bool flip;
    uint8_t brightness;  // 0..255
    bool camFull;    // picture-in-picture content shown full width instead of in the corner
    bool camMirror;  // mirror image, like a rear-view mirror
    uint8_t pipMode;    // Main-screen corner content: 0 off, 1 camera, 2 map
    uint8_t mapZoom;         // index into cfg::MAP_ZOOM_M
    uint8_t mapOrientation;  // mapview::HeadUp / BowUp / NorthUp
    float ratioShift;   // added to the rate-based recovery:drive target
    uint8_t crew;       // 1, 2, 4 or 8; the Stroke screen's togetherness wording
    uint8_t lang;       // i18n::Lang; coaching text on Efficiency and Stroke
    uint8_t tiles[TILE_SLOTS];  // metric shown in each Main-screen slot

    StrokeParams strokeParams() const {
        StrokeParams p;
        p.thresholdK = thresholdK;
        p.thresholdFloorG = thresholdFloorG;
        p.useMinimaThreshold = useMinimaThreshold;
        p.autoThreshold = autoThreshold;
        return p;
    }
};

namespace settings {

void begin();
Settings& get();
void save();

}  // namespace settings
