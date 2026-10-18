#include "efficiency.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "settings.h"

namespace efficiency {

namespace {

constexpr int HISTORY = 2000;  // strokes kept for the chart (a long session)
constexpr int BINS = MAX_RATE - MIN_RATE + 1;
constexpr int SMOOTH = 3;       // strokes averaged so single GPS jumps don't set a best
constexpr int TECH_AVG = 5;
constexpr int BIN_MIN = 3;      // strokes at a rate before its best is drawn
constexpr int SCORE_MIN = 5;    // strokes at a rate before it is scored
constexpr int USUAL_WINDOW = 200, USUAL_MIN = 20;

Stroke* hist = nullptr;
int head = 0, count = 0;
float binBest[BINS];
int binCount[BINS];
uint32_t ver = 0;

const Stroke& recent(int back) { return hist[(head - 1 - back + HISTORY) % HISTORY]; }

int binOf(float rate) {
    int b = (int)lroundf(rate) - MIN_RATE;
    return b < 0 || b >= BINS ? -1 : b;
}

// Mean rate and distance per stroke over the last SMOOTH strokes; false if any lacks GPS.
bool smoothed(float& rate, float& dps) {
    if (count < SMOOTH) return false;
    float r = 0, d = 0;
    for (int i = 0; i < SMOOTH; i++) {
        const Stroke& s = recent(i);
        if (s.dps <= 0) return false;
        r += s.rate;
        d += s.dps;
    }
    rate = r / SMOOTH;
    dps = d / SMOOTH;
    return true;
}

float recentMean(float Stroke::*field, int n) {
    float sum = 0;
    int k = 0;
    for (int i = 0; i < std::min(n, count); i++) {
        float v = recent(i).*field;
        if (v > 0) sum += v, k++;
    }
    return k ? sum / k : 0;
}

// Median of the last USUAL_WINDOW strokes, 0 until there are USUAL_MIN of them.
float usual(float Stroke::*field) {
    static float buf[USUAL_WINDOW];
    int k = 0;
    for (int i = 0; i < std::min(USUAL_WINDOW, count); i++) {
        float v = recent(i).*field;
        if (v > 0) buf[k++] = v;
    }
    if (k < USUAL_MIN) return 0;
    std::nth_element(buf, buf + k / 2, buf + k);
    return buf[k / 2];
}

}  // namespace

float targetRatio(float rateSpm) {
    // Coaching rule of thumb: about 2.5 at 20 spm, falling to about 1.3 at 34 spm.
    float r = fminf(fmaxf(rateSpm, 18.0f), 36.0f);
    float t = 2.5f - (r - 20.0f) * (1.2f / 14.0f);
    return fmaxf(1.0f, t + settings::get().ratioShift);
}

RatioCheck checkRatio(float ratio, float rateSpm) {
    RatioCheck c;
    if (ratio <= 0 || rateSpm <= 0) return c;
    c.target = targetRatio(rateSpm);
    float dev = ratio / c.target - 1.0f;
    c.advice = dev < -RATIO_TOLERANCE ? Advice::Slower : dev > RATIO_TOLERANCE ? Advice::Quicker : Advice::Good;
    c.far = fabsf(dev) > 2 * RATIO_TOLERANCE;
    return c;
}

const char* adviceWord(Advice a) {
    switch (a) {
        case Advice::Slower: return "SLOWER";
        case Advice::Good: return "GOOD";
        case Advice::Quicker: return "QUICKER";
        default: return "";
    }
}

void reset() {
    head = count = 0;
    for (int b = 0; b < BINS; b++) binBest[b] = 0, binCount[b] = 0;
    ver++;
}

void addStroke(const Stroke& s) {
    if (!hist) {
        hist = (Stroke*)heap_caps_malloc(HISTORY * sizeof(Stroke), MALLOC_CAP_SPIRAM);
        if (!hist) return;
    }
    hist[head] = s;
    head = (head + 1) % HISTORY;
    if (count < HISTORY) count++;
    float rate, dps;
    if (smoothed(rate, dps)) {
        int b = binOf(rate);
        if (b >= 0) {
            binCount[b]++;
            binBest[b] = fmaxf(binBest[b], dps);
        }
    }
    ver++;
}

Summary summary(float liveRate) {
    Summary s;
    if (liveRate <= 0 || count == 0) {
        snprintf(s.hint, sizeof(s.hint), "Start rowing: the chart fills in stroke by stroke");
        return s;
    }
    s.rate = liveRate;
    s.ratio = recentMean(&Stroke::ratio, TECH_AVG);
    s.ratioCheck = checkRatio(s.ratio, liveRate);
    s.checkMg = recentMean(&Stroke::checkMg, TECH_AVG);
    s.checkUsualMg = usual(&Stroke::checkMg);
    s.bounceMg = recentMean(&Stroke::bounceMg, TECH_AVG);
    s.bounceUsualMg = usual(&Stroke::bounceMg);

    float rate;
    bool haveDps = smoothed(rate, s.dps);
    int b = haveDps ? binOf(rate) : -1;
    if (b >= 0 && binCount[b] >= SCORE_MIN) {
        s.best = binBest[b];
        s.score = (int)lroundf(100.0f * s.dps / s.best);
    }
    int spm = (int)lroundf(liveRate);

    const RatioCheck& rc = s.ratioCheck;
    s.hintWarn = true;
    if (rc.advice == Advice::Slower) {
        snprintf(s.hint, sizeof(s.hint), "Slower slide: ratio %.1f, aim for %.1f at %d spm", s.ratio, rc.target, spm);
    } else if (rc.advice == Advice::Quicker) {
        snprintf(s.hint, sizeof(s.hint), "Quicker hands away: ratio %.1f, aim for %.1f at %d spm", s.ratio, rc.target,
                 spm);
    } else if (s.checkUsualMg > 0 && s.checkMg > 1.25f * s.checkUsualMg) {
        snprintf(s.hint, sizeof(s.hint), "Smoother slide: check %.0f mg, your usual is %.0f mg", s.checkMg,
                 s.checkUsualMg);
    } else if (!haveDps) {
        snprintf(s.hint, sizeof(s.hint), "No GPS: distance per stroke needs a GPS fix");
    } else if (s.score == 0) {
        s.hintWarn = false;
        snprintf(s.hint, sizeof(s.hint), "Learning your best at %d spm: keep rowing, try other rates too", spm);
    } else if (s.score < 93) {
        snprintf(s.hint, sizeof(s.hint), "Lengthen and push: %d%% of your best at %d spm (%.1f m)", s.score, spm,
                 s.best);
    } else {
        s.hintWarn = false;
        if (s.score >= 97)
            snprintf(s.hint, sizeof(s.hint), "On your best line at %d spm - hold this stroke", spm);
        else
            snprintf(s.hint, sizeof(s.hint), "Close to your best at %d spm: %d%%", spm, s.score);
    }
    return s;
}

int strokeCount() { return count; }

const Stroke& stroke(int i) { return recent(count - 1 - i); }

float bestAt(int rate) {
    int b = binOf((float)rate);
    return b >= 0 && binCount[b] >= BIN_MIN ? binBest[b] : 0;
}

uint32_t version() { return ver; }

}  // namespace efficiency
