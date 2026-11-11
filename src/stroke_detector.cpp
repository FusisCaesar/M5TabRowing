#include "stroke_detector.h"

#include <math.h>

using dsp::Vec3;

namespace {
constexpr float WARMUP_S = 3.0f;
constexpr float MAX_DRIVE_S = 2.5f;
constexpr int MIN_RECOVERY_SAMPLES = 10;
}  // namespace

StrokeDetector::StrokeDetector() {
    alphaG_ = dsp::emaAlpha(cfg::GRAVITY_TAU_S, cfg::IMU_DT_S);
    alphaC_ = dsp::emaAlpha(cfg::AXIS_COV_TAU_S, cfg::IMU_DT_S);
    alphaS_ = dsp::emaAlpha(cfg::SKEW_TAU_S, cfg::IMU_DT_S);
    alphaR_ = dsp::emaAlpha(cfg::RMS_TAU_S, cfg::IMU_DT_S);
    reset();
}

void StrokeDetector::reset() {
    init_ = false;
    for (auto& b : bp_) b.setup(cfg::BANDPASS_LOW_HZ, cfg::BANDPASS_HIGH_HZ, cfg::IMU_RATE_HZ);
    skewLp_.lowpass(cfg::SKEW_LOWPASS_HZ, cfg::IMU_RATE_HZ);
    cxx_ = cxy_ = cxz_ = cyy_ = cyz_ = czz_ = 0;
    sign_ = 1;
    m1_ = m2_ = m3_ = ms_ = 0;
    skewN_ = 0;
    gpsLocked_ = false;
    vInt_ = gpsMean_ = 0;
    dirVg_ = dirVv_ = dirGg_ = 0;
    gpsDirN_ = 0;
    dip_ = DipState::Seek;
    rescueDip_ = DipState::Seek;
    rescueChain_ = 0;
    lastCatchT_ = -1;
    intervalCount_ = intervalHead_ = 0;
    minimaCount_ = minimaHead_ = 0;
    seq_ = 0;
    phase_ = Phase::None;
    resetCycle();
}

void StrokeDetector::resetCycle() {
    drivePeak_ = 0;
    finishT_ = -1;
    driveTime_ = 0;
    rn_ = rt_ = rx_ = rtt_ = rtx_ = rxx_ = 0;
    vSum2_ = 0;
    vN_ = 0;
}

float StrokeDetector::rms() const { return sqrtf(ms_); }

float StrokeDetector::skew() const {
    float var = m2_ - m1_ * m1_;
    float c3 = m3_ - 3.0f * m1_ * m2_ + 2.0f * m1_ * m1_ * m1_;
    return var > 1e-8f ? c3 / powf(var, 1.5f) : 0.0f;
}

float StrokeDetector::gpsCorrelation() const {
    float d = sqrtf(dirVv_ * dirGg_);
    return d > 1e-12f ? dirVg_ / d : 0.0f;
}

float StrokeDetector::rate(double now) const {
    if (lastCatchT_ < 0 || intervalCount_ == 0) return 0;
    if (now - lastCatchT_ > cfg::STROKE_TIMEOUT_S) return 0;
    float sum = 0;
    for (int i = 0; i < intervalCount_; i++) sum += intervals_[i];
    return 60.0f * intervalCount_ / sum;
}

float StrokeDetector::thresholdMagnitude() const {
    float mag = fmaxf(params_.thresholdK * rms(), params_.thresholdFloorG);
    if (params_.useMinimaThreshold && minimaCount_ >= cfg::MINIMA_COUNT) {
        float deepest = 0;
        for (int i = 0; i < minimaCount_; i++) deepest = fminf(deepest, minima_[i]);
        mag = fmaxf(params_.thresholdFloorG, cfg::MINIMA_FACTOR * -deepest);
    }
    return mag;
}

bool StrokeDetector::rescuePeriod(float& period) const {
    if (lastCatchT_ < 0 || intervalCount_ < cfg::RESCUE_MIN_INTERVALS) return false;
    float sum = 0;
    for (int i = 0; i < intervalCount_; i++) sum += intervals_[i];
    period = sum / (float)intervalCount_;
    return period > 0;
}

bool StrokeDetector::inRescueWindow(double t, float loFactor) const {
    float period;
    if (rescueChain_ >= cfg::RESCUE_CHAIN_MAX || !rescuePeriod(period)) return false;
    if (t - lastCatchT_ > cfg::STROKE_TIMEOUT_S) return false;
    double lo = lastCatchT_ + (double)loFactor * period;
    double hi = lastCatchT_ + (double)cfg::RESCUE_WINDOW_HI * period;
    double cap = lastCatchT_ + cfg::STROKE_TIMEOUT_S;
    if (hi > cap) hi = cap;
    return t >= lo && t <= hi;
}

bool StrokeDetector::acceptCatch(double t, double catchT, float dipMin, bool rescued, DetectorSample& out,
                                 StrokeEvent& ev) {
    if (lastCatchT_ >= 0 && catchT - lastCatchT_ < cfg::MIN_STROKE_INTERVAL_S) return false;

    float interval = lastCatchT_ < 0 ? 0.0f : (float)(catchT - lastCatchT_);
    if (interval > cfg::STROKE_TIMEOUT_S) interval = 0;

    ev = StrokeEvent{};
    ev.seq = ++seq_;
    ev.catchTime = catchT;
    ev.interval = interval;
    ev.peakDecelG = dipMin;
    ev.rescued = rescued;
    if (interval > 0) {
        if (finishT_ > lastCatchT_ && driveTime_ > 0) {
            ev.driveTime = driveTime_;
            ev.recoveryTime = (float)(catchT - finishT_);
        }
        if (rn_ >= MIN_RECOVERY_SAMPLES) {
            double stt = rtt_ - rt_ * rt_ / rn_;
            double stx = rtx_ - rt_ * rx_ / rn_;
            double sxx = rxx_ - rx_ * rx_ / rn_;
            double resid = stt > 1e-9 ? sxx - stx * stx / stt : sxx;
            ev.checkMg = (float)(sqrt(fmax(resid, 0.0) / rn_) * 1000.0);
        }
        if (vN_ > 0) ev.bounceMg = (float)(sqrt(vSum2_ / vN_) * 1000.0);

        intervals_[intervalHead_] = interval;
        intervalHead_ = (intervalHead_ + 1) % cfg::RATE_AVG_STROKES;
        if (intervalCount_ < cfg::RATE_AVG_STROKES) intervalCount_++;
    } else {
        intervalCount_ = intervalHead_ = 0;
    }

    // Rescued dips go in too, so the minima threshold can follow the rhythm down
    // after a hard piece instead of staying stuck on the deep catches.
    minima_[minimaHead_] = dipMin;
    minimaHead_ = (minimaHead_ + 1) % cfg::MINIMA_COUNT;
    if (minimaCount_ < cfg::MINIMA_COUNT) minimaCount_++;

    lastCatchT_ = catchT;
    ev.rateSpm = rate(t);
    resetCycle();
    phase_ = Phase::Drive;
    out.catchDetected = true;
    rescueChain_ = rescued && interval > 0 ? rescueChain_ + 1 : 0;
    return true;
}

void StrokeDetector::updateAxis(const Vec3& h, const Vec3& gu) {
    cxx_ += (h.x * h.x - cxx_) * alphaC_;
    cxy_ += (h.x * h.y - cxy_) * alphaC_;
    cxz_ += (h.x * h.z - cxz_) * alphaC_;
    cyy_ += (h.y * h.y - cyy_) * alphaC_;
    cyz_ += (h.y * h.z - cyz_) * alphaC_;
    czz_ += (h.z * h.z - czz_) * alphaC_;

    // One power-iteration step per sample tracks the dominant horizontal direction.
    Vec3 v(cxx_ * axis_.x + cxy_ * axis_.y + cxz_ * axis_.z,
           cxy_ * axis_.x + cyy_ * axis_.y + cyz_ * axis_.z,
           cxz_ * axis_.x + cyz_ * axis_.y + czz_ * axis_.z);
    v = v - gu * v.dot(gu);
    float n = v.norm();
    if (n > 1e-12f) {
        v = v * (1.0f / n);
        if (v.dot(axis_) < 0) v = v * -1.0f;
        axis_ = v;
        return;
    }
    // Degenerate (no motion): keep the axis horizontal.
    Vec3 a = axis_ - gu * axis_.dot(gu);
    n = a.norm();
    if (n < 1e-3f) {
        Vec3 trial = fabsf(gu.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        a = trial - gu * trial.dot(gu);
        n = a.norm();
    }
    axis_ = a * (1.0f / n);
}

bool StrokeDetector::process(double t, float ax, float ay, float az, DetectorSample& out,
                             StrokeEvent& ev) {
    Vec3 a(ax, ay, az);
    if (!init_) {
        init_ = true;
        t0_ = t;
        g_ = a;
        prevT_ = t;
        prevX_ = 0;
        Vec3 gu = g_.norm() > 0.1f ? g_ * (1.0f / g_.norm()) : Vec3(0, 0, 1);
        Vec3 trial = fabsf(gu.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        axis_ = trial - gu * trial.dot(gu);
        axis_ = axis_ * (1.0f / axis_.norm());
    }

    g_ = g_ + (a - g_) * alphaG_;
    float gn = g_.norm();
    Vec3 gu = gn > 0.1f ? g_ * (1.0f / gn) : Vec3(0, 0, 1);
    Vec3 lin = a - g_;

    Vec3 bp(bp_[0].process(lin.x), bp_[1].process(lin.y), bp_[2].process(lin.z));
    float vert = bp.dot(gu);
    Vec3 h = bp - gu * vert;
    updateAxis(h, gu);

    float p = h.dot(axis_);
    Vec3 linH = lin - gu * lin.dot(gu);
    float pLin = linH.dot(axis_);

    // The catch is a short, deep deceleration, so forward surge is negatively skewed.
    // The 3 Hz band-pass smears that spike, so skew is measured on a wider band.
    // Cumulative average until the EMA window fills, so the direction settles within a few strokes.
    // Skipped during warm-up while the gravity estimate settles and leaves an offset.
    float pw = skewLp_.process(pLin);
    bool warm = (t - t0_) >= WARMUP_S;
    if (warm) {
        skewN_++;
        float aS = fmaxf(alphaS_, 1.0f / (float)skewN_);
        m1_ += (pw - m1_) * aS;
        m2_ += (pw * pw - m2_) * aS;
        m3_ += (pw * pw * pw - m3_) * aS;
    }
    bool flipped = false;
    if (warm && !gpsLocked_ && sign_ * skew() > cfg::SKEW_FLIP_THRESHOLD) {
        sign_ = -sign_;
        flipped = true;
    }

    // Shallow catches (smooth or fixed-seat rowing) can defeat the skew cue, so GPS overrides it.
    if (warm && gpsValid_ && gpsSpeed_ >= cfg::GPS_DIR_MIN_SPEED_MPS) {
        constexpr float dt = cfg::IMU_DT_S;
        vInt_ += (pLin * cfg::STANDARD_GRAVITY - vInt_ / cfg::GPS_DIR_INTEGRATOR_TAU_S) * dt;
        gpsMean_ += (gpsSpeed_ - gpsMean_) * dsp::emaAlpha(cfg::GPS_DIR_HP_TAU_S, dt);
        float vg = gpsSpeed_ - gpsMean_;
        float a = dsp::emaAlpha(cfg::GPS_DIR_CORR_TAU_S, dt);
        dirVg_ += (vInt_ * vg - dirVg_) * a;
        dirVv_ += (vInt_ * vInt_ - dirVv_) * a;
        dirGg_ += (vg * vg - dirGg_) * a;
        gpsDirN_++;
        float corr = gpsCorrelation();
        if (gpsDirN_ >= cfg::GPS_DIR_MIN_TIME_S * cfg::IMU_RATE_HZ && fabsf(corr) >= cfg::GPS_DIR_MIN_CORR) {
            int want = corr > 0 ? 1 : -1;
            gpsLocked_ = true;
            if (want != sign_) {
                sign_ = want;
                flipped = true;
            }
        }
    }

    float x = sign_ * p;
    out.surge = x;
    out.surgeLinear = sign_ * pLin;
    out.vertical = vert;
    out.catchDetected = false;
    out.finishDetected = false;

    ms_ += (x * x - ms_) * alphaR_;

    if (flipped) {
        dip_ = DipState::Seek;
        rescueDip_ = DipState::Seek;
        phase_ = Phase::None;
        resetCycle();
    }

    float normalMag = thresholdMagnitude();
    float normalThrMag = dip_ == DipState::InDip ? dipThr_ : normalMag;
    float relaxMag = fmaxf(cfg::RESCUE_THRESHOLD_FACTOR * normalMag, cfg::RESCUE_THRESHOLD_FLOOR_G);
    bool relaxUseful = relaxMag + 1e-4f < normalMag;
    float shown = normalThrMag;
    if (warm && dip_ != DipState::InDip) {
        if (rescueDip_ == DipState::InDip) shown = rescueThr_;
        else if (relaxUseful && inRescueWindow(t, cfg::RESCUE_WINDOW_LO)) shown = relaxMag;
    }
    out.threshold = -shown;

    vSum2_ += (double)vert * vert;
    vN_++;

    if (phase_ == Phase::Drive) {
        if (x > drivePeak_) drivePeak_ = x;
        float fin = cfg::FINISH_FRACTION * drivePeak_;
        if (drivePeak_ > 0.5f * params_.thresholdFloorG && prevX_ >= fin && x < fin) {
            double tf = prevT_ + (fin - prevX_) / (x - prevX_) * (t - prevT_);
            finishT_ = tf;
            driveTime_ = (float)(tf - lastCatchT_);
            phase_ = Phase::Recovery;
            out.finishDetected = true;
        } else if (t - lastCatchT_ > MAX_DRIVE_S) {
            phase_ = Phase::None;
        }
    } else if (phase_ == Phase::Recovery && dip_ == DipState::Seek && rescueDip_ == DipState::Seek) {
        double rt = t - finishT_;
        rn_ += 1;
        rt_ += rt;
        rx_ += x;
        rtt_ += rt * rt;
        rtx_ += rt * x;
        rxx_ += (double)x * x;
    }

    bool produced = false;
    float normalThr = -normalThrMag;
    if (!warm) {
        dip_ = DipState::Seek;
        rescueDip_ = DipState::Seek;
    } else if (dip_ == DipState::Seek && prevX_ >= normalThr && x < normalThr) {
        // Deep enough for the normal threshold: this dip belongs to it, not the gap-fill.
        tDown_ = prevT_ + (normalThr - prevX_) / (x - prevX_) * (t - prevT_);
        dipMin_ = x;
        dipThr_ = normalThrMag;
        dip_ = DipState::InDip;
        rescueDip_ = DipState::Seek;
    } else if (dip_ == DipState::Seek && rescueDip_ == DipState::Seek && relaxUseful &&
               inRescueWindow(t, cfg::RESCUE_WINDOW_LO) && prevX_ >= -relaxMag && x < -relaxMag) {
        rescueTDown_ = prevT_ + (-relaxMag - prevX_) / (x - prevX_) * (t - prevT_);
        rescueMin_ = x;
        rescueThr_ = relaxMag;
        rescueDip_ = DipState::InDip;
    } else if (dip_ == DipState::Seek && rescueDip_ == DipState::InDip) {
        float rescueThr = -rescueThr_;
        if (x < rescueMin_) rescueMin_ = x;
        if (x >= rescueThr) {
            double tUp = prevT_ + (rescueThr - prevX_) / (x - prevX_) * (t - prevT_);
            double catchT = 0.5 * (rescueTDown_ + tUp);
            rescueDip_ = DipState::Seek;
            if (inRescueWindow(catchT, cfg::RESCUE_ACCEPT_LO))
                produced = acceptCatch(t, catchT, rescueMin_, true, out, ev);
        } else if (t - rescueTDown_ > cfg::MAX_DIP_DURATION_S) {
            rescueDip_ = DipState::Seek;
        }
    } else if (dip_ == DipState::InDip) {
        if (x < dipMin_) dipMin_ = x;
        if (x >= normalThr) {
            double tUp = prevT_ + (normalThr - prevX_) / (x - prevX_) * (t - prevT_);
            double catchT = 0.5 * (tDown_ + tUp);
            dip_ = DipState::Seek;
            rescueDip_ = DipState::Seek;
            produced = acceptCatch(t, catchT, dipMin_, false, out, ev);
        } else if (t - tDown_ > cfg::MAX_DIP_DURATION_S) {
            dip_ = DipState::Seek;
        }
    }

    prevT_ = t;
    prevX_ = x;
    return produced;
}
