#pragma once

#include <math.h>

#include "config.h"
#include "dsp.h"

// How late the GPS speed arrives, measured while rowing.
//
// The speed in a recording is stamped when the sentence is read, not when the boat did that speed,
// and the difference is anything from 0.1 to 0.6 s depending on the outing. The forward surge,
// integrated, rises and falls with the speed of the boat once per stroke, so the two curves match
// when the surge is held back by exactly that delay. The match is tried at 19 delays and kept as a
// running average; the delay that matches best is the lag. tools/gps_lag.py does the same over a
// whole recording, and tools/replay.py has a port of this class.
//
// Both curves have the same filters applied, so their phases are alike: the surge has the slow
// gravity estimate taken off (GRAVITY_TAU_S), the GPS speed gets the very same filter, the
// integration is a plain one with only a very long leak, and then both go through one band-pass
// that keeps what goes with the strokes.
class GpsLagEstimator {
public:
    GpsLagEstimator() : alphaG_(dsp::emaAlpha(cfg::GRAVITY_TAU_S, cfg::IMU_DT_S)),
                        alphaC_(dsp::emaAlpha(cfg::GPS_LAG_CORR_TAU_S, cfg::IMU_DT_S)),
                        alphaHp_(dsp::emaAlpha(cfg::GPS_LAG_HP_TAU_S, cfg::IMU_DT_S)),
                        alphaLp_(dsp::emaAlpha(cfg::GPS_LAG_LP_TAU_S, cfg::IMU_DT_S)) {
        reset();
    }

    // Starts the measuring again. The lag in use stays: it is the best guess there is.
    void reset() {
        vH_ = vSlow_ = vFast_ = 0;
        gSlow_ = gFast_ = 0;
        for (float& v : ring_) v = 0;
        head_ = 0;
        run_ = 0;
        gMean_ = lastGps_ = 0;
        sinceChange_ = 0;
        holdS_ = 0.1f;
        for (float& c : c_) c = 0;
        vv_ = gg_ = 0;
        n_ = 0;
        tick_ = 0;
        corr_ = 0;
    }

    float delay() const { return delay_; }
    // Whether the delay has been measured on this outing yet, or is still the starting value.
    bool measured() const { return measured_; }
    // Match at the best delay, 0..1, as of the last reading.
    float correlation() const { return corr_; }
    // The delay in samples, at least 1 and short enough for the ring of speed that is kept.
    int delaySamples() const {
        int d = (int)(delay_ * cfg::IMU_RATE_HZ + 0.5f);
        if (d < 1) d = 1;
        if (d > cfg::GPS_LAG_RING - 1) d = cfg::GPS_LAG_RING - 1;
        return d;
    }
    void setDelay(float s) { delay_ = s; }

    // One call per IMU sample. surgeG: forward acceleration, gravity removed, g, pointing forward.
    // gpsMps: the speed as the device has it (held between fixes). moving: the GPS is valid and fast
    // enough, and which way is forward is known.
    void process(float surgeG, float gpsMps, bool moving) {
        constexpr float dt = cfg::IMU_DT_S;
        vH_ += (surgeG * cfg::STANDARD_GRAVITY - vH_ * (1.0f / cfg::GPS_LAG_INTEGRATOR_TAU_S)) * dt;
        // Speed from the surge, band-passed: what is kept is the part that goes with the strokes.
        vSlow_ += (vH_ - vSlow_) * alphaHp_;
        vFast_ += (vH_ - vSlow_ - vFast_) * alphaLp_;
        const float vB = vFast_;
        ring_[head_] = vB;

        if (!moving) {
            run_ = 0;
        } else {
            if (run_ == 0) {
                gMean_ = gpsMps;
                lastGps_ = gpsMps;
                gSlow_ = gFast_ = 0;
                sinceChange_ = 0;
            }
            run_++;
            gMean_ += (gpsMps - gMean_) * alphaG_;
            float gIn = gpsMps - gMean_;
            gSlow_ += (gIn - gSlow_) * alphaHp_;
            gFast_ += (gIn - gSlow_ - gFast_) * alphaLp_;
            const float gH = gFast_;

            // How often the receiver reports: the speed is held between fixes, which delays the held
            // curve by half of that on top of the receiver's own delay.
            if (sinceChange_ < 1000) sinceChange_++;
            if (gpsMps != lastGps_) {
                if (sinceChange_ >= 3 && sinceChange_ <= 50) holdS_ += (sinceChange_ * dt - holdS_) * 0.05f;
                sinceChange_ = 0;
                lastGps_ = gpsMps;
            }

            if (run_ >= (int)(cfg::GPS_LAG_SETTLE_S * cfg::IMU_RATE_HZ)) {
                n_++;
                float a = fmaxf(alphaC_, 1.0f / (float)n_);
                for (int k = 0; k < cfg::GPS_LAG_STEPS; k++) {
                    int idx = head_ - k * cfg::GPS_LAG_STEP_SAMPLES;
                    if (idx < 0) idx += cfg::GPS_LAG_RING;
                    c_[k] += (ring_[idx] * gH - c_[k]) * a;
                }
                vv_ += (vB * vB - vv_) * a;
                gg_ += (gH * gH - gg_) * a;
            }
        }
        head_ = (head_ + 1) % cfg::GPS_LAG_RING;

        if (++tick_ >= (int)cfg::IMU_RATE_HZ) {
            tick_ = 0;
            read();
        }
    }

private:
    // Once a second: the delay with the best match, if the match is good.
    void read() {
        if (n_ < (uint32_t)(cfg::GPS_LAG_MIN_TIME_S * cfg::IMU_RATE_HZ)) return;
        float den = sqrtf(vv_ * gg_);
        if (den < 1e-12f) return;
        int best = 0;
        for (int k = 1; k < cfg::GPS_LAG_STEPS; k++) {
            if (c_[k] > c_[best]) best = k;
        }
        float r = c_[best] / den;
        corr_ = r;
        if (r < cfg::GPS_LAG_MIN_CORR) return;
        float k = (float)best;
        if (best > 0 && best < cfg::GPS_LAG_STEPS - 1) {
            float y0 = c_[best - 1] / den, y2 = c_[best + 1] / den;
            float d = y0 - 2.0f * r + y2;
            if (d < 0) k += fminf(fmaxf(0.5f * (y0 - y2) / d, -0.5f), 0.5f);
        }
        float lag = k * cfg::GPS_LAG_STEP_SAMPLES * cfg::IMU_DT_S - 0.5f * holdS_;
        lag = fminf(fmaxf(lag, 0.0f), cfg::GPS_LAG_MAX_S);
        if (!measured_) {
            delay_ = lag;
            measured_ = true;
        } else {
            delay_ += (lag - delay_) * cfg::GPS_LAG_FOLLOW;
        }
    }

    float alphaG_, alphaC_, alphaHp_, alphaLp_;
    float delay_ = cfg::GPS_LAG_INITIAL_S;
    bool measured_ = false;

    float vH_ = 0, vSlow_ = 0, vFast_ = 0;  // integrated surge, its slow part, and the band-passed rest
    float gSlow_ = 0, gFast_ = 0;           // the same for the GPS speed
    float ring_[cfg::GPS_LAG_RING] = {};    // band-passed speed from the surge; the newest is at head_
    int head_ = 0;
    int run_ = 0;  // samples in a row that the GPS was moving
    float gMean_ = 0, lastGps_ = 0;
    int sinceChange_ = 0;
    float holdS_ = 0.1f;
    float c_[cfg::GPS_LAG_STEPS] = {};  // running average of surge at each delay times GPS speed
    float vv_ = 0, gg_ = 0;
    uint32_t n_ = 0;
    int tick_ = 0;
    float corr_ = 0;
};
