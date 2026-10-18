#pragma once

#include <stdint.h>

#include "stroke_detector.h"

struct Settings {
    float thresholdK;
    float thresholdFloorG;
    bool useMinimaThreshold;
    bool rawLog;
    bool fusion;
    bool dayTheme;
    bool flip;
    uint8_t brightness;  // 0..255
    bool camFull;    // picture-in-picture content shown full width instead of in the corner
    bool camMirror;  // mirror image, like a rear-view mirror
    uint8_t pipMode;    // Main-screen corner content: 0 off, 1 camera, 2 map
    uint8_t mapZoom;    // index into cfg::MAP_ZOOM_M
    bool mapHeadUp;     // rotate the map so the course points up
    float ratioShift;   // added to the rate-based recovery:drive target

    StrokeParams strokeParams() const {
        StrokeParams p;
        p.thresholdK = thresholdK;
        p.thresholdFloorG = thresholdFloorG;
        p.useMinimaThreshold = useMinimaThreshold;
        return p;
    }
};

namespace settings {

void begin();
Settings& get();
void save();

}  // namespace settings
