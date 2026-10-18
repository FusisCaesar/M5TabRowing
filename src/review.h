#pragma once

#include <stdint.h>

// A recorded session loaded back from the SD card for the review screen: the
// per-second GPS log (track, speed, split, rate) plus totals.
namespace review {

struct Summary {
    int number = 0;
    char label[32] = "";
    char start[20] = "";
    float durationS = 0;
    float distanceM = 0;
    float avgSplitS = 0;    // 0 = unknown
    float avgRate = 0;      // spm, strokes only
    int strokes = 0;
    int laps = 0;
    float maxSpeed = 0;     // m/s, 5 s average
    float best500S = 0;     // fastest 500 m, as a split; 0 = not rowed that far
    float avgDps = 0;       // metres per stroke
};

// Reads S####_*seconds.csv, *strokes.csv and *laps.csv. False if the session has no GPS log.
bool load(int number);
void unload();
bool loaded();
const Summary& summary();

// Per-second samples, oldest first.
int count();
float elapsed(int i);     // s
float speed(int i);       // m/s, 5 s average
float split(int i);       // s / 500 m from the 5 s average speed, 0 when stopped
float rate(int i);        // spm, 0 between pieces

// Track points (seconds with a GPS position), with a colour from slow (red) to fast (green).
int trackCount();
const double* trackLat();
const double* trackLon();
const uint16_t* trackColor();  // RGB565
bool trackBounds(double& south, double& west, double& north, double& east);
// The speeds that map to the two ends of the colour scale (10th and 90th percentile).
void speedScale(float& slow, float& fast);

}  // namespace review
