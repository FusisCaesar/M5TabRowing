#pragma once

#include <stdint.h>

// Rowing efficiency: the recovery:drive target and how distance per stroke compares
// with the rower's own best at the same stroke rate in this session.
namespace efficiency {

enum class Advice : uint8_t { None, Slower, Good, Quicker };

struct RatioCheck {
    float target = 0;          // recovery / drive the rower should aim for at this rate
    Advice advice = Advice::None;
    bool far = false;          // more than twice the tolerance away from the target
};

constexpr float RATIO_TOLERANCE = 0.15f;  // +-15 % of the target counts as good

float targetRatio(float rateSpm);  // includes the Settings shift
RatioCheck checkRatio(float ratio, float rateSpm);
const char* adviceWord(Advice a);

constexpr int MIN_RATE = 14, MAX_RATE = 40;

struct Stroke {
    float rate, dps, ratio, checkMg, bounceMg;
};

struct Summary {
    float rate = 0;          // spm, last 3 strokes
    float dps = 0;           // m, last 3 strokes, 0 without GPS
    float best = 0;          // m, session best around this rate, 0 while still learning
    int score = 0;           // dps as % of best, 0 while still learning
    float ratio = 0;
    RatioCheck ratioCheck;
    float checkMg = 0, checkUsualMg = 0;    // current and session median
    float bounceMg = 0, bounceUsualMg = 0;
    char hint[96] = "";
    bool hintWarn = false;
};

void reset();
void addStroke(const Stroke& s);
Summary summary(float liveRate);

// Chart data. Strokes are returned oldest first.
int strokeCount();
const Stroke& stroke(int i);
float bestAt(int rate);  // 0 if fewer than BIN_MIN strokes at that rate
uint32_t version();      // changes with every stroke and reset

}  // namespace efficiency
