#pragma once

#include <stdint.h>

#include "config.h"
#include "dsp.h"

// One stroke, catch to catch, cut from the hull acceleration.
// Drive and recovery are resampled separately so strokes at different rates
// still line up phase for phase. The pre-roll is real time, so the catch dip
// (which starts before the catch the detector reports) stays visible.
namespace anatomy {

// Long enough to hold the whole run-in to the catch: a heavy boat starts slowing 0.3 s or more before it.
constexpr float PREROLL_S = 0.40f;
constexpr int PREROLL_N = 40;  // 10 ms each
constexpr int DRIVE_BINS = 32;
constexpr int REC_BINS = 64;
constexpr int BINS = DRIVE_BINS + REC_BINS;
constexpr int GPS_MAX = 48;

constexpr float LOWPASS_HZ = 8.0f;

inline float prerollTime(int i) { return -PREROLL_S + (i + 0.5f) * (PREROLL_S / PREROLL_N); }

// Bin centre in seconds from the catch. Drive bins come first.
inline float binTime(int i, float driveS, float recS) {
    if (i < DRIVE_BINS) return (i + 0.5f) / DRIVE_BINS * driveS;
    return driveS + (i - DRIVE_BINS + 0.5f) / REC_BINS * recS;
}

struct Cycle {
    float driveS = 0;
    float recS = 0;
    float meanSpeed = 0;  // m/s over the stroke, 0 without GPS
    float preroll[PREROLL_N] = {};
    float accel[BINS] = {};
    float gpsT[GPS_MAX] = {};  // seconds from the catch, negative in the pre-roll, less the GPS delay
    float gpsV[GPS_MAX] = {};
    uint8_t gpsN = 0;
    bool ok = false;
};

// Numbers read off one acceleration curve. Times in seconds, acceleration in g.
struct Features {
    float catchDepthG = 0;    // positive
    float catchWidthS = 0;    // width at half depth
    float catchLoss = 0;      // m/s lost from the catch until the boat picks up again
    float notches = 0;        // separate dips in the catch, 1 when it is clean
    float drivePeakG = 0;
    float driveGain = 0;      // m/s added by the drive
    float drivePeakPct = 0;   // 0..1, where the peak sits in the drive
    float driveSagG = 0;      // dip between the two drive humps
    float releaseDipG = 0;    // positive, below the glide
    float fallTimeS = 0;      // 80% to 20% of the drive peak
    float runUnevenMg = 0;    // scatter of the glide around a straight line, milli-g
    float speedPeakS = 0;     // seconds after the catch
    float approachLeadS = 0;  // how long before the next catch the boat starts slowing
    float approachLoss = 0;   // m/s lost before the blade is in
    bool valid = false;
};

// Phase boundaries in seconds from the catch. The catch phase runs from 0 to catch1.
// catch0 is where the boat starts slowing into it, usually negative: that stretch is
// the approach of the stroke before, so its loss is counted there and not twice.
struct Bounds {
    float catch0 = 0, catch1 = 0;
    float finish = 0, finishEnd = 0;
    float approach = 0;
    float interval = 0;
};

struct Summary {
    bool have = false;
    float rate = 0;
    int samples = 0;  // strokes in the average
    float driveS = 0, recS = 0;
    float preroll[PREROLL_N] = {};
    float accel[BINS] = {};
    float speed[PREROLL_N + BINS] = {};  // m/s at each pre-roll sample and bin
    float meanSpeed = 0;
    bool haveRef = false;
    bool vsBest = false;  // otherwise the reference is the usual stroke
    float refPreroll[PREROLL_N] = {};
    float refAccel[BINS] = {};
    Features now, ref;
    Bounds bounds;
    int gpsN = 0;
    float gpsT[GPS_MAX] = {};
    float gpsV[GPS_MAX] = {};
    int crew = 1;
    char hint[96] = "";
    bool hintWarn = false;
};

// Fed one sample at a time from the IMU task. No heap use.
class Recorder {
public:
    void push(double t, float surgeLinearG, float gpsMps, bool gpsValid);

    // Cuts the stroke that ends at catchT and started at prevCatchT.
    // finishT is the detector's finish, between the two catches.
    bool cut(double catchT, double prevCatchT, double finishT, Cycle& out);

private:
    struct Sample {
        double t;
        float surge;
        float gps;  // negative when there is no fix
    };
    Sample ring_[1024] = {};
    int head_ = 0;
    int count_ = 0;
    dsp::Biquad lp_;
    bool init_ = false;
};

void reset();
void addStroke(const Cycle& cycle, float rateSpm, float dps);
const Summary& summary(float liveRate);
uint32_t version();

}  // namespace anatomy
