#pragma once

#include <stdint.h>

#include "config.h"
#include "dsp.h"

struct StrokeParams {
    float thresholdK = cfg::THRESHOLD_K_DEFAULT;
    float thresholdFloorG = cfg::THRESHOLD_FLOOR_G_DEFAULT;
    bool useMinimaThreshold = false;
};

// Emitted at each accepted catch. Interval, drive, recovery, check and bounce
// describe the stroke cycle that this catch completes.
struct StrokeEvent {
    uint32_t seq = 0;
    double catchTime = 0;     // s, detector time base
    float interval = 0;       // s, 0 for the first stroke after a pause
    float driveTime = 0;      // s, 0 if the finish wasn't found
    float recoveryTime = 0;   // s, 0 if the finish wasn't found
    float peakDecelG = 0;     // g, negative
    float checkMg = 0;        // recovery deceleration residual, milli-g, 0 if unknown
    float bounceMg = 0;       // RMS vertical acceleration over the cycle, milli-g
    float rateSpm = 0;        // mean of the last few intervals
    bool rescued = false;     // true when only the relaxed, expected-window threshold saw it
    double fusedDistanceM = 0;  // filled in by the IMU task
};

struct DetectorSample {
    float surge = 0;        // band-passed surge, g, forward positive
    float surgeLinear = 0;  // gravity removed, not band-passed, g
    float vertical = 0;     // band-passed vertical, g
    float threshold = 0;    // catch threshold, g, negative
    bool catchDetected = false;
    bool finishDetected = false;
};

class StrokeDetector {
public:
    StrokeDetector();

    void setParams(const StrokeParams& p) { params_ = p; }
    void setGps(float speedMps, bool valid) {
        gpsSpeed_ = speedMps;
        gpsValid_ = valid;
    }
    bool directionFromGps() const { return gpsLocked_; }
    float gpsCorrelation() const;
    const StrokeParams& params() const { return params_; }
    void reset();

    // Acceleration in g, t in seconds (monotonic, sampled at cfg::IMU_RATE_HZ).
    // Returns true when `ev` holds a new stroke.
    bool process(double t, float ax, float ay, float az, DetectorSample& out, StrokeEvent& ev);

    float rate(double now) const;
    double lastCatchTime() const { return lastCatchT_; }
    uint32_t strokeCount() const { return seq_; }
    float rms() const;
    float skew() const;
    int sign() const { return sign_; }
    dsp::Vec3 axis() const { return axis_; }
    dsp::Vec3 gravity() const { return g_; }

private:
    enum class DipState { Seek, InDip };
    enum class Phase { None, Drive, Recovery };

    void resetCycle();
    void updateAxis(const dsp::Vec3& h, const dsp::Vec3& gu);
    float thresholdMagnitude() const;
    // Recent rhythm: average of the intervals kept for the rate. False until two exist.
    bool rescuePeriod(float& period) const;
    bool inRescueWindow(double t, float loFactor) const;
    // Shared tail of a normal and a rescued catch. False when the interval is too short.
    bool acceptCatch(double t, double catchT, float dipMin, bool rescued, DetectorSample& out,
                     StrokeEvent& ev);

    StrokeParams params_;

    bool init_ = false;
    double t0_ = 0;
    float alphaG_, alphaC_, alphaS_, alphaR_;

    dsp::Vec3 g_;
    dsp::BandPass bp_[3];
    dsp::Biquad skewLp_;
    float cxx_ = 0, cxy_ = 0, cxz_ = 0, cyy_ = 0, cyz_ = 0, czz_ = 0;
    dsp::Vec3 axis_{1, 0, 0};
    int sign_ = 1;
    float m1_ = 0, m2_ = 0, m3_ = 0;
    uint32_t skewN_ = 0;

    float gpsSpeed_ = 0;
    bool gpsValid_ = false;
    bool gpsLocked_ = false;
    float vInt_ = 0, gpsMean_ = 0;
    float vDelay_[cfg::GPS_DIR_DELAY_SAMPLES] = {};  // integrated surge, oldest at vDelayHead_
    int vDelayHead_ = 0;
    float dirVg_ = 0, dirVv_ = 0, dirGg_ = 0;
    uint32_t gpsDirN_ = 0;
    float ms_ = 0;

    DipState dip_ = DipState::Seek;
    double prevT_ = 0;
    float prevX_ = 0;
    double tDown_ = 0;
    float dipMin_ = 0;
    float dipThr_ = 0;

    // Second dip tracker, armed only inside the expected window, at a lower threshold.
    DipState rescueDip_ = DipState::Seek;
    double rescueTDown_ = 0;
    float rescueMin_ = 0;
    float rescueThr_ = 0;
    int rescueChain_ = 0;

    double lastCatchT_ = -1;
    float intervals_[cfg::RATE_AVG_STROKES] = {};
    int intervalCount_ = 0;
    int intervalHead_ = 0;
    float minima_[cfg::MINIMA_COUNT] = {};
    int minimaCount_ = 0;
    int minimaHead_ = 0;
    uint32_t seq_ = 0;

    Phase phase_ = Phase::None;
    float drivePeak_ = 0;
    double finishT_ = -1;
    float driveTime_ = 0;

    // Recovery: linear regression of surge vs time for the check metric.
    double rn_ = 0, rt_ = 0, rx_ = 0, rtt_ = 0, rtx_ = 0, rxx_ = 0;
    // Whole cycle: vertical energy for the bounce metric.
    double vSum2_ = 0;
    uint32_t vN_ = 0;
};
