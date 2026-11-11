#include "stroke_anatomy.h"

#include <Arduino.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "efficiency.h"
#include "settings.h"

namespace anatomy {

namespace {

// Thresholds for the phase split and for which hint is worth saying.
// tools/anatomy.py repeats these; keep the two together.
constexpr float MIN_CATCH_DEPTH_G = 0.05f;
constexpr float HALF_DEPTH = 0.50f;
constexpr float DIP_START_FRAC = 0.25f;
constexpr float NOTCH_FRAC = 0.40f;
constexpr float NOTCH_GAP_S = 0.030f;
constexpr float FINISH_WINDOW_S = 0.35f;
constexpr float SETTLE_G = 0.03f;
constexpr float APPROACH_MARGIN_G = 0.025f;
constexpr float SERIES_DT = 0.01f;
constexpr int MAX_N = 700;

constexpr float BEST_FRAC = 0.95f;
constexpr int AVG_N = 5;
constexpr int RECENT = 8;
constexpr int REF_MIN = 3;
constexpr int RUN_CAP = 30;
constexpr int MIN_RATE = 14, MAX_RATE = 40, BAND_W = 4;
constexpr int BANDS = (MAX_RATE - MIN_RATE) / BAND_W + 1;

constexpr float HINT_WIDTH_RATIO = 1.25f;
constexpr float HINT_WIDTH_ABS_S = 0.020f;
constexpr float HINT_NOTCH_EXTRA = 0.8f;
constexpr float HINT_FALL_RATIO = 1.30f;
constexpr float HINT_FALL_ABS_S = 0.025f;
constexpr float HINT_APPROACH_ABS_S = 0.08f;
constexpr float HINT_LOSS_ABS = 0.04f;
constexpr float HINT_LOSS_RATIO = 1.25f;
constexpr float HINT_GAIN_RATIO = 0.90f;
constexpr float HINT_PEAK_DIFF = 0.15f;
constexpr float HINT_RELEASE_ABS_G = 0.025f;

// analyse() and the display share these; only the UI task calls them.
float gT[MAX_N], gA[MAX_N], gV[MAX_N], gRun[MAX_N];
float kT[PREROLL_N + BINS], kA[PREROLL_N + BINS];
int gN = 0;

struct Held {
    Cycle c;
    float rate = 0;
};

struct Template {
    int n = 0;
    float preroll[PREROLL_N] = {};
    float accel[BINS] = {};
    float driveS = 0, recS = 0;
};

Held recent[RECENT];
int recentHead = 0, recentCount = 0;
Template usual[BANDS], bestT[BANDS];
uint32_t ver = 1;

int bandOf(float rate) {
    int r = (int)lroundf(rate);
    if (r < MIN_RATE) r = MIN_RATE;
    if (r > MAX_RATE) r = MAX_RATE;
    return (r - MIN_RATE) / BAND_W;
}

void buildKnots(const float* preroll, const float* accel, float driveS, float recS) {
    for (int i = 0; i < PREROLL_N; i++) {
        kT[i] = prerollTime(i);
        kA[i] = preroll[i];
    }
    for (int i = 0; i < BINS; i++) {
        kT[PREROLL_N + i] = binTime(i, driveS, recS);
        kA[PREROLL_N + i] = accel[i];
    }
}

float sampleAt(float time) {
    constexpr int n = PREROLL_N + BINS;
    if (time <= kT[0]) return kA[0];
    if (time >= kT[n - 1]) return kA[n - 1];
    int i = 1;
    while (i < n - 1 && kT[i] < time) i++;
    float span = kT[i] - kT[i - 1];
    float u = span > 1e-6f ? (time - kT[i - 1]) / span : 0;
    return kA[i - 1] + u * (kA[i] - kA[i - 1]);
}

float atTime(const float* t, const float* v, int n, float time) {
    if (n <= 0) return 0;
    if (time <= t[0]) return v[0];
    if (time >= t[n - 1]) return v[n - 1];
    int i = 1;
    while (i < n - 1 && t[i] < time) i++;
    float span = t[i] - t[i - 1];
    float u = span > 1e-6f ? (time - t[i - 1]) / span : 0;
    return v[i - 1] + u * (v[i] - v[i - 1]);
}

int indexAt(const float* t, int n, float time) {
    int i = 0;
    while (i < n - 1 && t[i] < time) i++;
    return i;
}

// Crossing of `level` walking away from a point that is below it.
float crossLeft(int n, int from, float level) {
    int j = from;
    while (j > 0 && gA[j] < level) j--;
    if (j + 1 >= n || gA[j] < level || gA[j + 1] >= level) return gT[j];
    float span = gA[j + 1] - gA[j];
    float u = span != 0 ? (level - gA[j]) / span : 0;
    return gT[j] + u * (gT[j + 1] - gT[j]);
}

float crossRight(int n, int from, float level) {
    int j = from;
    while (j < n - 1 && gA[j] < level) j++;
    if (j == 0 || gA[j] < level || gA[j - 1] >= level) return gT[j];
    float span = gA[j] - gA[j - 1];
    float u = span != 0 ? (level - gA[j - 1]) / span : 0;
    return gT[j - 1] + u * (gT[j] - gT[j - 1]);
}

float crossDown(int n, int from, float level) {
    for (int i = from; i < n - 1; i++) {
        if (gA[i] >= level && gA[i + 1] < level) {
            float span = gA[i + 1] - gA[i];
            float u = span != 0 ? (level - gA[i]) / span : 0;
            return gT[i] + u * (gT[i + 1] - gT[i]);
        }
    }
    return -1;
}

// Fills gT/gA/gV from the resampled stroke and reads the five phases off it.
bool analyse(const float* preroll, const float* accel, float driveS, float recS, Features& f, Bounds& b) {
    f = Features{};
    b = Bounds{};
    gN = 0;
    float interval = driveS + recS;
    if (driveS < 0.15f || recS < 0.15f) return false;

    int n = (int)lroundf((PREROLL_S + interval) / SERIES_DT);
    if (n < 10) return false;
    if (n > MAX_N) n = MAX_N;
    gN = n;

    buildKnots(preroll, accel, driveS, recS);
    gT[0] = -PREROLL_S;
    gA[0] = sampleAt(gT[0]);
    gV[0] = 0;
    for (int i = 1; i < n; i++) {
        gT[i] = -PREROLL_S + i * SERIES_DT;
        gA[i] = sampleAt(gT[i]);
        gV[i] = gV[i - 1] + 0.5f * (gA[i] + gA[i - 1]) * cfg::STANDARD_GRAVITY * SERIES_DT;
    }

    int iLo = indexAt(gT, n, -PREROLL_S);
    int iHi = indexAt(gT, n, 0.20f);
    int imin = iLo;
    for (int i = iLo; i <= iHi && i < n; i++)
        if (gA[i] < gA[imin]) imin = i;
    float depth = -gA[imin];
    if (depth < MIN_CATCH_DEPTH_G) return false;

    float half = -HALF_DEPTH * depth;
    float widthL = crossLeft(n, imin, half);
    float widthR = crossRight(n, imin, half);
    float dipStart = crossLeft(n, imin, -DIP_START_FRAC * depth);
    float dipEnd = crossRight(n, imin, 0);
    if (dipEnd > 0.30f) dipEnd = widthR;

    int notches = 0;
    float lastNotch = -1e9f;
    int iStart = indexAt(gT, n, dipStart);
    int iEnd = indexAt(gT, n, dipEnd);
    for (int i = iStart + 1; i < iEnd && i < n - 1; i++) {
        if (gA[i] <= gA[i - 1] && gA[i] < gA[i + 1] && gA[i] < -NOTCH_FRAC * depth &&
            gT[i] - lastNotch >= NOTCH_GAP_S) {
            notches++;
            lastNotch = gT[i];
        }
    }

    auto vel = [&](float time) { return atTime(gT, gV, n, time); };
    f.catchDepthG = depth;
    f.catchWidthS = fmaxf(0, widthR - widthL);
    f.catchLoss = fmaxf(0, vel(dipStart) - vel(dipEnd));
    f.notches = (float)(notches > 0 ? notches : 1);

    int iDrive = indexAt(gT, n, fmaxf(dipEnd, 0));
    int iFinish = indexAt(gT, n, driveS);
    int iPeak = iDrive;
    for (int i = iDrive; i <= iFinish && i < n; i++)
        if (gA[i] > gA[iPeak]) iPeak = i;
    f.drivePeakG = gA[iPeak];
    f.drivePeakPct = driveS > 0 ? fminf(1, fmaxf(0, gT[iPeak] / driveS)) : 0;
    f.driveGain = fmaxf(0, vel(driveS) - vel(dipEnd));

    float mid = 0.5f * (fmaxf(dipEnd, 0) + driveS);
    int iMid = indexAt(gT, n, mid);
    int iP1 = iDrive, iP2 = iMid;
    for (int i = iDrive; i < iMid && i < n; i++)
        if (gA[i] > gA[iP1]) iP1 = i;
    for (int i = iMid; i <= iFinish && i < n; i++)
        if (gA[i] > gA[iP2]) iP2 = i;
    if (gA[iP1] > 0.45f * f.drivePeakG && gA[iP2] > 0.45f * f.drivePeakG && iP2 > iP1) {
        float valley = gA[iP1];
        for (int i = iP1; i <= iP2; i++) valley = fminf(valley, gA[i]);
        float sag = fminf(gA[iP1], gA[iP2]) - valley;
        if (sag > 0.03f) f.driveSagG = sag;
    }

    float t80 = f.drivePeakG > 0 ? crossDown(n, iPeak, 0.8f * f.drivePeakG) : -1;
    float t20 = t80 > 0 ? crossDown(n, indexAt(gT, n, t80), 0.2f * f.drivePeakG) : -1;
    if (t20 > t80 && t80 > 0) f.fallTimeS = t20 - t80;

    // Glide level from the middle of the recovery, clear of the release and the approach.
    float runFrom = driveS + 0.20f;
    float runTo = interval - 0.40f;
    if (runTo - runFrom < 0.15f) {
        runFrom = driveS + 0.10f;
        runTo = interval - 0.10f;
    }
    int runN = 0;
    if (runTo > runFrom) {
        int a = indexAt(gT, n, runFrom), c = indexAt(gT, n, runTo);
        for (int i = a; i <= c && i < n && runN < MAX_N; i++) gRun[runN++] = gA[i];
    }
    float runBase = 0;
    if (runN > 0) {
        std::nth_element(gRun, gRun + runN / 2, gRun + runN);
        runBase = gRun[runN / 2];
    }

    int iRel0 = indexAt(gT, n, driveS);
    int iRel1 = indexAt(gT, n, fminf(interval, driveS + FINISH_WINDOW_S));
    int iRelMin = iRel0;
    for (int i = iRel0; i <= iRel1 && i < n; i++)
        if (gA[i] < gA[iRelMin]) iRelMin = i;
    f.releaseDipG = fmaxf(0, runBase - gA[iRelMin]);
    float finishEnd = gT[iRel1];
    for (int i = iRelMin; i <= iRel1 && i < n; i++) {
        if (fabsf(gA[i] - runBase) < SETTLE_G) {
            finishEnd = gT[i];
            break;
        }
    }

    // Walk back from the next catch until the deceleration eases off to the glide.
    float thr = runBase - APPROACH_MARGIN_G;
    int iBack = indexAt(gT, n, fminf(interval, driveS + 0.05f));
    int iGlide = iBack;
    for (int i = n - 1; i >= iBack; i--) {
        if (gA[i] > thr) {
            iGlide = i;
            break;
        }
    }
    float approachT = gT[iGlide];
    f.approachLeadS = fmaxf(0, interval - approachT);
    f.approachLoss = fmaxf(0, vel(approachT) - vel(interval));

    float runSum = 0;
    int runCount = 0;
    int iRun0 = indexAt(gT, n, finishEnd);
    int iRun1 = indexAt(gT, n, approachT);
    for (int i = iRun0; i <= iRun1 && i < n; i++) {
        runSum += gA[i];
        runCount++;
    }
    float runMean = runCount > 0 ? runSum / runCount : runBase;
    f.runDecelMg = fmaxf(0, -runMean * 1000.0f);

    int iFast = indexAt(gT, n, 0);
    for (int i = iFast; i < n; i++)
        if (gV[i] > gV[iFast]) iFast = i;
    f.speedPeakS = fmaxf(0, gT[iFast]);

    if (dipEnd < 0) dipEnd = 0;
    if (dipEnd > driveS) dipEnd = driveS;
    if (finishEnd < driveS) finishEnd = driveS;
    if (approachT < finishEnd) approachT = finishEnd;
    if (approachT > interval) approachT = interval;

    b.catch0 = dipStart;
    b.catch1 = dipEnd;
    b.finish = driveS;
    b.finishEnd = finishEnd;
    b.approach = approachT;
    b.interval = interval;

    f.valid = f.drivePeakG > 0.02f;
    return f.valid;
}

// Speed along the drawn samples: the integral of the acceleration, shifted so
// its mean over the stroke equals the GPS mean. With no GPS the mean is zero,
// so the line shows only the rise and fall.
// Uses the series left by the most recent analyse() of this same curve.
void fillSpeed(float driveS, float recS, float meanSpeed, float* out) {
    int n = gN;
    if (n < 2) {
        for (int i = 0; i < PREROLL_N + BINS; i++) out[i] = meanSpeed;
        return;
    }
    double sum = 0;
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (gT[i] >= 0 && gT[i] <= driveS + recS) {
            sum += gV[i];
            m++;
        }
    }
    float shift = meanSpeed - (m ? (float)(sum / m) : 0);
    for (int i = 0; i < PREROLL_N; i++) out[i] = atTime(gT, gV, n, prerollTime(i)) + shift;
    for (int i = 0; i < BINS; i++) out[PREROLL_N + i] = atTime(gT, gV, n, binTime(i, driveS, recS)) + shift;
}

void blend(Template& dst, const Cycle& c) {
    dst.n++;
    float k = 1.0f / (float)(dst.n < RUN_CAP ? dst.n : RUN_CAP);
    for (int i = 0; i < PREROLL_N; i++) dst.preroll[i] += (c.preroll[i] - dst.preroll[i]) * k;
    for (int i = 0; i < BINS; i++) dst.accel[i] += (c.accel[i] - dst.accel[i]) * k;
    dst.driveS += (c.driveS - dst.driveS) * k;
    dst.recS += (c.recS - dst.recS) * k;
}

int averageRecent(float rate, Cycle& out, float& meanSpeed) {
    int band = bandOf(rate);
    float preroll[PREROLL_N] = {}, accel[BINS] = {};
    float drive = 0, rec = 0, speed = 0;
    int n = 0, nSpeed = 0;
    for (int i = 0; i < recentCount && n < AVG_N; i++) {
        const Held& h = recent[(recentHead - 1 - i + RECENT) % RECENT];
        if (!h.c.ok || bandOf(h.rate) != band) continue;
        for (int j = 0; j < PREROLL_N; j++) preroll[j] += h.c.preroll[j];
        for (int j = 0; j < BINS; j++) accel[j] += h.c.accel[j];
        drive += h.c.driveS;
        rec += h.c.recS;
        if (h.c.meanSpeed > 0) {
            speed += h.c.meanSpeed;
            nSpeed++;
        }
        n++;
    }
    out = Cycle{};
    if (n == 0) return 0;
    for (int j = 0; j < PREROLL_N; j++) out.preroll[j] = preroll[j] / n;
    for (int j = 0; j < BINS; j++) out.accel[j] = accel[j] / n;
    out.driveS = drive / n;
    out.recS = rec / n;
    out.ok = true;
    meanSpeed = nSpeed ? speed / nSpeed : 0;
    return n;
}

const char* whoWord(bool vsBest) { return vsBest ? "best" : "usual"; }
const char* sharpWord(bool vsBest) { return vsBest ? "sharpest" : "usual"; }

bool widerThan(const Features& now, const Features& ref) {
    return ref.catchWidthS > 0.02f && now.catchWidthS > ref.catchWidthS * HINT_WIDTH_RATIO &&
           now.catchWidthS > ref.catchWidthS + HINT_WIDTH_ABS_S;
}

bool moreNotches(const Features& now, const Features& ref) {
    return now.notches >= ref.notches + HINT_NOTCH_EXTRA;
}

bool slowerFall(const Features& now, const Features& ref) {
    return ref.fallTimeS > 0.02f && now.fallTimeS > ref.fallTimeS * HINT_FALL_RATIO &&
           now.fallTimeS > ref.fallTimeS + HINT_FALL_ABS_S;
}

void writeHint(Summary& s) {
    const Features& now = s.now;
    const Features& ref = s.ref;
    int spm = (int)lroundf(s.rate);
    const char* who = whoWord(s.vsBest);
    const char* sharp = sharpWord(s.vsBest);
    s.hintWarn = true;

    if (!s.haveRef) {
        s.hintWarn = false;
        snprintf(s.hint, sizeof(s.hint), "Learning your stroke at %d spm", spm);
        return;
    }
    int nowMs = (int)lroundf(now.catchWidthS * 1000);
    int refMs = (int)lroundf(ref.catchWidthS * 1000);
    if (s.crew > 1 && (widerThan(now, ref) || moreNotches(now, ref))) {
        if (widerThan(now, ref))
            snprintf(s.hint, sizeof(s.hint), "Catch together: catch %d ms wide, your %s %d ms", nowMs, sharp, refMs);
        else
            snprintf(s.hint, sizeof(s.hint), "Catch together: %.0f notches, your %s has %.0f", now.notches, sharp,
                     ref.notches);
        return;
    }
    if (s.crew == 1 && widerThan(now, ref)) {
        snprintf(s.hint, sizeof(s.hint), "Quicker into the water: catch %d ms wide, your %s %d ms", nowMs, who, refMs);
        return;
    }
    int fallNow = (int)lroundf(now.fallTimeS * 1000);
    int fallRef = (int)lroundf(ref.fallTimeS * 1000);
    if (s.crew > 1 && slowerFall(now, ref)) {
        snprintf(s.hint, sizeof(s.hint), "Finish together: release takes %d ms, your %s %d ms", fallNow, sharp, fallRef);
        return;
    }
    if (s.crew == 1 && slowerFall(now, ref)) {
        snprintf(s.hint, sizeof(s.hint), "Sharper finish: release takes %d ms, your %s %d ms", fallNow, who, fallRef);
        return;
    }
    if (now.approachLeadS > ref.approachLeadS + HINT_APPROACH_ABS_S) {
        snprintf(s.hint, sizeof(s.hint), "Dip later: slows %.2f s before the catch, your %s %.2f s", now.approachLeadS,
                 who, ref.approachLeadS);
        return;
    }
    if (ref.catchLoss > 0.02f && now.catchLoss > ref.catchLoss * HINT_LOSS_RATIO &&
        now.catchLoss > ref.catchLoss + HINT_LOSS_ABS) {
        snprintf(s.hint, sizeof(s.hint), "Quicker catch: losing %.2f m/s, your %s %.2f", now.catchLoss, who, ref.catchLoss);
        return;
    }
    if (ref.driveGain > 0.05f && now.driveGain < ref.driveGain * HINT_GAIN_RATIO) {
        snprintf(s.hint, sizeof(s.hint), "Push harder: drive adds %.2f m/s, your %s %.2f at %d spm", now.driveGain, who,
                 ref.driveGain, spm);
        return;
    }
    if (now.drivePeakPct > ref.drivePeakPct + HINT_PEAK_DIFF) {
        snprintf(s.hint, sizeof(s.hint), "Build the drive earlier: peak at %d%% of the drive",
                 (int)lroundf(now.drivePeakPct * 100));
        return;
    }
    if (now.drivePeakPct + HINT_PEAK_DIFF < ref.drivePeakPct) {
        snprintf(s.hint, sizeof(s.hint), "Hold it through the finish: peak at %d%% of the drive",
                 (int)lroundf(now.drivePeakPct * 100));
        return;
    }
    if (now.releaseDipG > ref.releaseDipG + HINT_RELEASE_ABS_G) {
        snprintf(s.hint, sizeof(s.hint), "Cleaner release: dip %.2f g, your %s %.2f g", now.releaseDipG, who,
                 ref.releaseDipG);
        return;
    }
    s.hintWarn = false;
    snprintf(s.hint, sizeof(s.hint), "Matches your %s stroke at %d spm", who, spm);
}

}  // namespace

void Recorder::push(double t, float surgeLinearG, float gpsMps, bool gpsValid) {
    if (!init_) {
        lp_.lowpass(LOWPASS_HZ, cfg::IMU_RATE_HZ);
        init_ = true;
    }
    ring_[head_] = {t, lp_.process(surgeLinearG), gpsValid ? gpsMps : -1.0f};
    head_ = (head_ + 1) & 1023;
    if (count_ < 1024) count_++;
}

bool Recorder::cut(double catchT, double prevCatchT, double finishT, Cycle& out) {
    out = Cycle{};
    double drive = finishT - prevCatchT;
    double rec = catchT - finishT;
    double interval = catchT - prevCatchT;
    if (interval < 0.8 || interval > cfg::STROKE_TIMEOUT_S || drive < 0.15 || rec < 0.15) return false;

    // Static: cut() runs on the IMU task, whose stack is shared with the detector.
    static double sumP[PREROLL_N];
    static double sumA[BINS];
    static int nP[PREROLL_N], nA[BINS];
    memset(sumP, 0, sizeof(sumP));
    memset(sumA, 0, sizeof(sumA));
    memset(nP, 0, sizeof(nP));
    memset(nA, 0, sizeof(nA));
    float prevGps = -1e9f;
    double prevGpsT = -1e9;
    double speedSum = 0;
    int speedN = 0;

    int idx = (head_ - count_ + 2048) & 1023;
    for (int i = 0; i < count_; i++) {
        const Sample& s = ring_[(idx + i) & 1023];
        if (s.t < prevCatchT - (double)PREROLL_S) continue;
        if (s.t > catchT) break;
        if (s.t < prevCatchT) {
            int b = (int)((s.t - (prevCatchT - (double)PREROLL_S)) / 0.01);
            if (b >= 0 && b < PREROLL_N) {
                sumP[b] += s.surge;
                nP[b]++;
            }
        } else if (s.t < finishT) {
            int b = (int)((s.t - prevCatchT) / drive * DRIVE_BINS);
            if (b >= DRIVE_BINS) b = DRIVE_BINS - 1;
            if (b >= 0) {
                sumA[b] += s.surge;
                nA[b]++;
            }
        } else {
            int b = DRIVE_BINS + (int)((s.t - finishT) / rec * REC_BINS);
            if (b >= BINS) b = BINS - 1;
            if (b >= DRIVE_BINS) {
                sumA[b] += s.surge;
                nA[b]++;
            }
        }
        if (s.gps >= 0 && out.gpsN < GPS_MAX && (s.gps != prevGps || s.t - prevGpsT >= 0.08)) {
            out.gpsT[out.gpsN] = (float)(s.t - prevCatchT);
            out.gpsV[out.gpsN] = s.gps;
            if (s.t >= prevCatchT) {
                speedSum += s.gps;
                speedN++;
            }
            out.gpsN++;
            prevGps = s.gps;
            prevGpsT = s.t;
        }
    }

    auto fill = [](float* dst, const double* sum, const int* n, int count) {
        int known = 0;
        for (int i = 0; i < count; i++) {
            if (n[i] > 0) {
                dst[i] = (float)(sum[i] / n[i]);
                known++;
            }
        }
        if (known == 0) return 0;
        int first = 0;
        while (first < count && n[first] == 0) first++;
        int last = count - 1;
        while (last > first && n[last] == 0) last--;
        for (int i = 0; i < first; i++) dst[i] = dst[first];
        for (int i = last + 1; i < count; i++) dst[i] = dst[last];
        for (int i = first; i <= last; i++) {
            if (n[i] > 0) continue;
            int j = i;
            while (j <= last && n[j] == 0) j++;
            float a = dst[i - 1], b = dst[j];
            for (int k = i; k < j; k++) dst[k] = a + (b - a) * (float)(k - (i - 1)) / (float)(j - (i - 1));
            i = j;
        }
        return known;
    };

    int gotP = fill(out.preroll, sumP, nP, PREROLL_N);
    int gotA = fill(out.accel, sumA, nA, BINS);
    if (gotP < PREROLL_N / 2 || gotA < BINS * 7 / 10) return false;

    out.driveS = (float)drive;
    out.recS = (float)rec;
    out.meanSpeed = speedN >= 2 ? (float)(speedSum / speedN) : 0;
    out.ok = true;
    return true;
}

void reset() {
    recentHead = recentCount = 0;
    memset(usual, 0, sizeof(usual));
    memset(bestT, 0, sizeof(bestT));
    ver++;
}

void addStroke(const Cycle& c, float rate, float dps) {
    if (!c.ok || rate <= 0) return;
    recent[recentHead] = {c, rate};
    recentHead = (recentHead + 1) % RECENT;
    if (recentCount < RECENT) recentCount++;

    Features f;
    Bounds b;
    if (analyse(c.preroll, c.accel, c.driveS, c.recS, f, b)) {
        int band = bandOf(rate);
        blend(usual[band], c);
        float bestDps = efficiency::bestAt((int)lroundf(rate));
        if (dps > 0 && bestDps > 0 && dps >= BEST_FRAC * bestDps) blend(bestT[band], c);
        Serial.printf("[anatomy] %.0f spm catch %.0f ms %.2f g  drive %+.2f m/s peak %.0f%%  approach %.2f s\n", rate,
                      f.catchWidthS * 1000.0f, f.catchDepthG, f.driveGain, f.drivePeakPct * 100.0f, f.approachLeadS);
    }
    ver++;
}

const Summary& summary(float liveRate) {
    static Summary s;
    s = Summary{};
    int crew = settings::get().crew;
    s.crew = (crew == 2 || crew == 4 || crew == 8) ? crew : 1;

    if (recentCount == 0) {
        snprintf(s.hint, sizeof(s.hint), "Start rowing: the stroke builds up catch by catch");
        return s;
    }
    float rate = liveRate;
    if (rate <= 0) rate = recent[(recentHead - 1 + RECENT) % RECENT].rate;

    Cycle avg;
    float meanSpeed = 0;
    int n = averageRecent(rate, avg, meanSpeed);
    if (n == 0) {
        rate = recent[(recentHead - 1 + RECENT) % RECENT].rate;
        n = averageRecent(rate, avg, meanSpeed);
    }
    if (n == 0) {
        snprintf(s.hint, sizeof(s.hint), "Start rowing: the stroke builds up catch by catch");
        return s;
    }

    s.have = true;
    s.rate = rate;
    s.samples = n;
    s.driveS = avg.driveS;
    s.recS = avg.recS;
    s.meanSpeed = meanSpeed;
    memcpy(s.preroll, avg.preroll, sizeof(s.preroll));
    memcpy(s.accel, avg.accel, sizeof(s.accel));

    // GPS dots from the newest stroke at this rate, so they belong to one stroke.
    int band = bandOf(rate);
    for (int i = 0; i < recentCount; i++) {
        const Held& h = recent[(recentHead - 1 - i + RECENT) % RECENT];
        if (!h.c.ok || bandOf(h.rate) != band || h.c.gpsN == 0) continue;
        s.gpsN = h.c.gpsN;
        memcpy(s.gpsT, h.c.gpsT, sizeof(float) * h.c.gpsN);
        memcpy(s.gpsV, h.c.gpsV, sizeof(float) * h.c.gpsN);
        break;
    }

    if (!analyse(avg.preroll, avg.accel, avg.driveS, avg.recS, s.now, s.bounds)) {
        fillSpeed(avg.driveS, avg.recS, meanSpeed, s.speed);
        s.hintWarn = false;
        snprintf(s.hint, sizeof(s.hint), "Reading the stroke shape");
        return s;
    }
    fillSpeed(avg.driveS, avg.recS, meanSpeed, s.speed);

    // fillSpeed has sampled the series, so analysing the reference may reuse it.
    Template* ref = nullptr;
    if (bestT[band].n >= REF_MIN) {
        ref = &bestT[band];
        s.vsBest = true;
    } else if (usual[band].n >= REF_MIN) {
        ref = &usual[band];
    }
    if (ref) {
        Features rf;
        Bounds rb;
        if (analyse(ref->preroll, ref->accel, ref->driveS, ref->recS, rf, rb)) {
            s.haveRef = true;
            s.ref = rf;
            memcpy(s.refPreroll, ref->preroll, sizeof(s.refPreroll));
            memcpy(s.refAccel, ref->accel, sizeof(s.refAccel));
        }
    }
    writeHint(s);
    return s;
}

uint32_t version() { return ver; }

}  // namespace anatomy
