#pragma once

// Oar angle and the stroke's turning points, from the accelerometer and gyro of one stick.
// No Arduino or M5 types in here: tools/oar_sim.py is a line-by-line Python port that is used to check
// this against simulated rowing on a PC. Keep them in step.
//
// The stick sits anywhere on the oar, in any orientation. The oar turns about the vertical pin, so the
// angle is the gyro rate projected on "up" (found from the accelerometer, and carried along by the
// gyro while the oar feathers) and integrated. The stroke is its two turning points, found where the
// rate crosses zero: the catch is the one followed by the shorter phase (the drive), and the finish
// the other. That works the same on stroke side and bow side and for any mounting.
//
// The catch here is the oar turning round at the stern end of the arc, not the blade entering the
// water. Everything is timed the same way on every stick, so seats can still be compared with each other.

#include <math.h>
#include <stdint.h>

namespace oartrack {

constexpr float RATE_FILTER_HZ = 8.0f;     // low pass on the yaw rate used to find the turns
constexpr float MIN_ARC_DEG = 15.0f;       // a stroke is at least this wide
constexpr float STRONG_DPS = 20.0f;        // a phase must reach this rate before its end counts
constexpr float MIN_PHASE_S = 0.15f;       // shortest drive or recovery
constexpr float MAX_PHASE_S = 6.0f;        // longer than this is a pause
constexpr int MID_LEARN_STROKES = 4;       // strokes that set where the arc is centred after a zero
constexpr float MID_GAIN = 0.5f;           // share of the drift taken out per stroke
constexpr float MID_MAX_ERR_DEG = 35.0f;   // a bigger jump of the arc is a new position, not drift

struct Stroke {
    uint8_t no = 0;
    double catchT = 0, finishT = 0;  // seconds on the caller's clock
    float catchDeg = 0, finishDeg = 0;
    float recoveryS = 0;  // previous finish to this catch, 0 = unknown
};

class Tracker {
public:
    // One IMU sample: time in seconds, acceleration in g, rotation rate in degrees per second.
    void update(double t, const float a[3], const float w[3]) {
        if (!started_) {
            started_ = true;
            t0_ = prevT_ = t;
            float m = norm(a);
            if (m < 0.5f) return;
            for (int i = 0; i < 3; i++) u_[i] = a[i] / m;
            haveU_ = true;
            return;
        }
        double tPrev = prevT_;
        double dtd = t - tPrev;
        prevT_ = t;
        if (!haveU_) {
            float m = norm(a);
            if (m < 0.5f) return;
            for (int i = 0; i < 3; i++) u_[i] = a[i] / m;
            haveU_ = true;
            return;
        }
        if (dtd <= 0) return;
        float dt = dtd > 0.1 ? 0.1f : (float)dtd;

        float m = norm(a);
        float wc[3] = {w[0] - bias_[0], w[1] - bias_[1], w[2] - bias_[2]};
        bool quiet = norm(wc) < 3.0f && fabsf(m - 1.0f) < 0.05f;
        stillS_ = quiet ? stillS_ + dt : 0;
        still_ = stillS_ > 0.5f;
        if (still_) {
            // Gyro bias: quick at the start (the stick is usually put down to switch on), slow after.
            float k = dt / ((t - t0_) < 10.0 ? 0.5f : 5.0f);
            for (int i = 0; i < 3; i++) bias_[i] += (w[i] - bias_[i]) * k;
        }

        // "Up" in the stick's own frame: carried along by the gyro, pulled towards the accelerometer
        // when that reads about 1 g (the oar is hardly accelerating).
        // Turned by the exact rotation (Rodrigues), not a small step: the blade feathers at several
        // hundred degrees a second and a plain step would tilt "up" by a few degrees each time.
        const float D2R = 0.017453292f;
        float wn = norm(wc);
        if (wn > 1e-3f) {
            float phi = wn * D2R * dt;
            float n[3] = {wc[0] / wn, wc[1] / wn, wc[2] / wn};
            float cp = cosf(phi), sp = sinf(phi);
            float nu = n[0] * u_[0] + n[1] * u_[1] + n[2] * u_[2];
            float cr[3] = {n[1] * u_[2] - n[2] * u_[1], n[2] * u_[0] - n[0] * u_[2], n[0] * u_[1] - n[1] * u_[0]};
            for (int i = 0; i < 3; i++) u_[i] = u_[i] * cp - cr[i] * sp + n[i] * nu * (1.0f - cp);
        }
        if (fabsf(m - 1.0f) < 0.1f) {
            float k = dt / (quiet ? 0.3f : 3.0f);
            for (int i = 0; i < 3; i++) u_[i] += k * (a[i] / m - u_[i]);
        }
        float un = norm(u_);
        for (int i = 0; i < 3; i++) u_[i] /= un;

        learnShaft(wc, wn, dt);
        float yaw = wc[0] * u_[0] + wc[1] * u_[1] + wc[2] * u_[2];  // degrees per second about up
        float su = shaft_[0] * u_[0] + shaft_[1] * u_[1] + shaft_[2] * u_[2];
        if (shaftValid_ && fabsf(su) < 0.35f) {  // a shaft pointing up is not a shaft
            // Feathering turns the oar about its own axis, a few hundred degrees a second, twice a stroke
            // and right at the catch and the finish. A little error in "up" lets that leak into the yaw
            // rate and shifts the turning points. What matters is how fast the shaft's direction swings
            // round the vertical, which leaves the rotation about the shaft itself out:
            // (w.up - (s.up)(w.s)) / (1 - (s.up)^2). A tilt error in "up" along the shaft drops out too.
            float ws = wc[0] * shaft_[0] + wc[1] * shaft_[1] + wc[2] * shaft_[2];
            float den = 1.0f - su * su;
            if (den < 0.5f) den = 0.5f;
            yaw = (yaw - su * ws) / den;
        }
        theta_ += yaw * dt;
        float alpha = 1.0f - expf(-6.2831853f * RATE_FILTER_HZ * dt);
        float prevWf = wf_;
        wf_ += (yaw - wf_) * alpha;
        detect(t, tPrev, prevWf);
    }

    // The oar is square to the boat right now: this is zero degrees.
    void zero() {
        zeroOff_ = theta_;
        zeroed_ = true;
        midRefValid_ = false;
        midN_ = 0;
        midSum_ = 0;
    }

    // Degrees, positive towards the stern once the catch has been told from the finish.
    float angleDeg() const { return sgn_ * (theta_ - zeroOff_); }
    bool zeroed() const { return zeroed_; }
    bool still() const { return still_; }
    uint32_t strokeCount() const { return strokes_; }
    const Stroke& last() const { return last_; }
    // Strokes per minute from the last two catches, 0 when unknown or stale.
    float rateSpm(double now) const { return rate_ > 0 && now - last_.catchT < 8.0 ? rate_ : 0; }
    float arcDeg() const { return arcEma_; }

private:
    static float norm(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

    // The shaft is the axis the gyro turns about hardest: the feathering is several times faster than
    // the swing of the oar. It is the dominant direction of the rotation-rate covariance, found with a
    // power iteration a sample at a time. Only trusted when that direction really dominates.
    void learnShaft(const float wc[3], float wn, float dt) {
        if (wn > 5.0f) {
            const float k = dt / 20.0f;  // the last twenty seconds of movement
            const float keep = 1.0f - k;
            const float kw = k * wn * wn * 1e-4f;  // weighted by the rate squared: the feathering stands out even more
            cov_[0] = keep * cov_[0] + kw * wc[0] * wc[0];
            cov_[1] = keep * cov_[1] + kw * wc[0] * wc[1];
            cov_[2] = keep * cov_[2] + kw * wc[0] * wc[2];
            cov_[3] = keep * cov_[3] + kw * wc[1] * wc[1];
            cov_[4] = keep * cov_[4] + kw * wc[1] * wc[2];
            cov_[5] = keep * cov_[5] + kw * wc[2] * wc[2];
        }
        float v[3] = {cov_[0] * shaft_[0] + cov_[1] * shaft_[1] + cov_[2] * shaft_[2],
                      cov_[1] * shaft_[0] + cov_[3] * shaft_[1] + cov_[4] * shaft_[2],
                      cov_[2] * shaft_[0] + cov_[4] * shaft_[1] + cov_[5] * shaft_[2]};
        float vn = norm(v);
        if (vn < 1e-3f) return;
        for (int i = 0; i < 3; i++) shaft_[i] = v[i] / vn;
        float trace = cov_[0] + cov_[3] + cov_[5];
        float lam = shaft_[0] * v[0] + shaft_[1] * v[1] + shaft_[2] * v[2];
        shaftValid_ = trace > 2000.0f && lam > 0.65f * trace;
    }

    void detect(double t, double prevT, float prevWf) {
        float h = 0.15f * arcEma_;
        h = h < 6.0f ? 6.0f : (h > 15.0f ? 15.0f : h);

        if (dir_ == 0) {
            if (!seeded_) {
                seeded_ = true;
                lo_ = hi_ = theta_;
            }
            if (theta_ > hi_) hi_ = theta_;
            if (theta_ < lo_) lo_ = theta_;
            if (theta_ > lo_ + 2 * h) startDir(+1);
            else if (theta_ < hi_ - 2 * h) startDir(-1);
            return;
        }

        float s = (float)dir_;
        // Moving on in the current direction: the running extreme of the angle.
        if (s * (theta_ - extreme_) >= 0) {
            extreme_ = theta_;
            extremeT_ = t;
        }
        if (s * wf_ >= STRONG_DPS) {
            strongSeen_ = true;
            crossT_ = -1;
        } else if (strongSeen_ && crossT_ < 0 && s * wf_ <= 0) {
            float d = prevWf - wf_;
            double f = d != 0 ? prevWf / d : 1.0;
            crossT_ = prevT + (t - prevT) * f;
        }
        // The reversal is confirmed once the angle has come back by h.
        if (s * (extreme_ - theta_) >= h) {
            // The rate was low passed: take its delay off. Never earlier than the previous turn.
            double turnT = crossT_ >= 0 ? crossT_ - 1.0 / (6.2831853 * RATE_FILTER_HZ) : extremeT_;
            if (haveLast_ && turnT < lastT_) turnT = extremeT_;
            confirm(dir_, extreme_, turnT);
            dir_ = -dir_;
            extreme_ = theta_;
            extremeT_ = t;
            strongSeen_ = false;
            crossT_ = -1;
        }
    }

    void startDir(int d) {
        dir_ = d;
        extreme_ = theta_;
        extremeT_ = prevT_;
        strongSeen_ = false;
        crossT_ = -1;
    }

    void confirm(int kind, float th, double tt) {
        float pending = 0;
        if (haveLast_ && lastKind_ == -kind) {
            if (fabsf(th - lastTh_) < MIN_ARC_DEG) return;  // a bump, not a turn
            double seg = tt - lastT_;
            if (seg < MIN_PHASE_S || seg > MAX_PHASE_S) {
                haveLast_ = false;
            } else {
                int idx = lastKind_ > 0 ? 0 : 1;
                segAfter_[idx] = segN_[idx] ? 0.7f * segAfter_[idx] + 0.3f * (float)seg : (float)seg;
                if (segN_[idx] < 100) segN_[idx]++;
                float arc = fabsf(th - lastTh_);
                arcEma_ = arcN_ ? 0.7f * arcEma_ + 0.3f * arc : arc;
                arcN_++;
                classify();
                if (catchKind_ != 0 && kind != catchKind_) pending = emit(lastT_, lastTh_, tt, th);
            }
        }
        lastKind_ = kind;
        lastT_ = tt;
        lastTh_ = th;
        haveLast_ = true;
        if (pending != 0) {
            theta_ += pending;
            lastTh_ += pending;
        }
    }

    // The turn followed by the shorter phase is the catch. Switches only on a clear difference.
    void classify() {
        if (!segN_[0] || !segN_[1]) return;
        if (segAfter_[0] < 0.9f * segAfter_[1]) catchKind_ = +1;
        else if (segAfter_[1] < 0.9f * segAfter_[0]) catchKind_ = -1;
    }

    // Records a stroke; returns the shift to apply to the angle to take out gyro drift.
    float emit(double ct, float cth, double ft, float fth) {
        if (ft - ct < MIN_PHASE_S) return 0;
        sgn_ = cth >= fth ? 1.0f : -1.0f;
        Stroke s;
        s.no = (uint8_t)(strokes_ + 1);
        s.catchT = ct;
        s.finishT = ft;
        s.catchDeg = sgn_ * (cth - zeroOff_);
        s.finishDeg = sgn_ * (fth - zeroOff_);
        s.recoveryS = 0;
        if (haveFinish_ && ct - prevFinishT_ >= 0.2 && ct - prevFinishT_ <= MAX_PHASE_S)
            s.recoveryS = (float)(ct - prevFinishT_);
        if (haveCatch_ && ct - prevCatchT_ > 0.8 && ct - prevCatchT_ < MAX_PHASE_S * 1.5)
            rate_ = (float)(60.0 / (ct - prevCatchT_));
        else
            rate_ = 0;
        prevCatchT_ = ct;
        haveCatch_ = true;
        prevFinishT_ = ft;
        haveFinish_ = true;
        strokes_++;
        last_ = s;

        // Where the arc is centred drifts with the gyro bias. After a zero the first strokes set where it
        // belongs, and from then on it is pulled back there, a part of the error per stroke.
        float mid = 0.5f * (cth + fth);
        if (!midRefValid_) {
            midSum_ += mid;
            if (++midN_ >= MID_LEARN_STROKES) {
                midRef_ = midSum_ / midN_;
                midRefValid_ = true;
            }
            return 0;
        }
        float err = mid - midRef_;
        if (fabsf(err) > MID_MAX_ERR_DEG) {
            midRefValid_ = false;
            midN_ = 0;
            midSum_ = 0;
            return 0;
        }
        return -MID_GAIN * err;
    }

    bool started_ = false, haveU_ = false, still_ = false, zeroed_ = false;
    double t0_ = 0, prevT_ = 0;
    float u_[3] = {0, 0, 1};
    float bias_[3] = {0, 0, 0};
    float stillS_ = 0;
    float theta_ = 0, zeroOff_ = 0, sgn_ = 1.0f;
    float wf_ = 0;
    float cov_[6] = {0, 0, 0, 0, 0, 0};  // rotation rate covariance: xx xy xz yy yz zz
    float shaft_[3] = {1, 0, 0};         // the oar's own axis, in the stick's frame
    bool shaftValid_ = false;

    // Turn finding.
    int dir_ = 0;  // +1 angle rising, -1 falling, 0 not moving yet
    bool seeded_ = false, strongSeen_ = false;
    float lo_ = 0, hi_ = 0, extreme_ = 0;
    double extremeT_ = 0, crossT_ = -1;

    // Turns and the stroke built from them.
    bool haveLast_ = false;
    int lastKind_ = 0, catchKind_ = 0;  // +1 peak of the angle, -1 trough
    double lastT_ = 0;
    float lastTh_ = 0;
    float segAfter_[2] = {0, 0};  // average phase length after a peak, after a trough
    int segN_[2] = {0, 0};
    float arcEma_ = 60.0f;
    int arcN_ = 0;

    uint32_t strokes_ = 0;
    Stroke last_;
    bool haveFinish_ = false, haveCatch_ = false;
    double prevFinishT_ = 0, prevCatchT_ = 0;
    float rate_ = 0;

    bool midRefValid_ = false;
    int midN_ = 0;
    float midSum_ = 0, midRef_ = 0;
};

}  // namespace oartrack
