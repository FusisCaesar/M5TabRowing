#pragma once

#include <stdint.h>

#include "stroke_detector.h"

namespace imu {

struct Live {
    float rate = 0;
    uint32_t strokes = 0;
    double lastCatch = -1;
    float rms = 0;
    float skew = 0;
    int sign = 1;
    bool directionFromGps = false;
    float gpsCorrelation = 0;
    float axis[3] = {};
    float gravity[3] = {};
    float accel[3] = {};
    float threshold = 0;
    float sampleHz = 0;
    uint32_t overruns = 0;
    bool fusionActive = false;
    float fusedSpeed = 0;
    double fusedDistance = 0;
};

struct PlotPoint {
    float surge;
    float threshold;
    uint8_t flags;  // bit0 catch, bit1 finish
};

void begin(const StrokeParams& p, bool fusionEnabled);
void setParams(const StrokeParams& p);
void setFusionEnabled(bool on);
void setGpsSpeed(float speedMps, bool valid);

bool popStroke(StrokeEvent& ev);
Live live();

// Copies the newest `n` plot points (oldest first); returns how many were copied.
int copyPlot(PlotPoint* dst, int n);

}  // namespace imu
