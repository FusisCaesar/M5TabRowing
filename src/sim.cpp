#include "sim.h"

#include <math.h>
#include <stdlib.h>

namespace sim {

namespace {
constexpr double PERIOD_S = 120.0;
constexpr double W = 2.0 * M_PI / PERIOD_S;
constexpr float TILT_RAD = 35.0f * (float)M_PI / 180.0f;

float noise(float amp) { return amp * ((rand() / (float)RAND_MAX) * 2.0f - 1.0f); }

float gauss(float x, float mu, float sigma) {
    float d = (x - mu) / sigma;
    return expf(-0.5f * d * d);
}
}  // namespace

float rateSpm(double t) { return (float)(26.0 - 6.0 * cos(W * t)); }

float strokePhase(double t) {
    double cycles = (26.0 * t - 6.0 * sin(W * t) / W) / 60.0;
    return (float)(cycles - floor(cycles));
}

void accel(double t, float& ax, float& ay, float& az) {
    float period = 60.0f / rateSpm(t);
    float tau = strokePhase(t) * period;
    float tw = tau > period * 0.5f ? tau - period : tau;

    float surge = -0.45f * gauss(tw, 0.0f, 0.05f) + 0.22f * gauss(tau, 0.45f, 0.18f) - 0.04f;
    float vert = 0.03f * sinf(2.0f * (float)M_PI * tau / period);
    float vib = 0.02f * sinf(2.0f * (float)M_PI * 8.0f * (float)t);

    // Device tilted 35 degrees toward the rower and rotated so surge falls on -Y/+Z.
    float upY = sinf(TILT_RAD), upZ = cosf(TILT_RAD);
    float fwY = -cosf(TILT_RAD), fwZ = sinf(TILT_RAD);
    float g = 1.0f + vert;
    ax = noise(0.015f) + vib;
    ay = upY * g + fwY * surge + noise(0.015f) + vib;
    az = upZ * g + fwZ * surge + noise(0.015f);
}

float boatSpeed(double t) {
    float r = rateSpm(t);
    float base = 3.2f + 0.08f * (r - 20.0f);
    float ph = strokePhase(t);
    return base + 0.35f * sinf(2.0f * (float)M_PI * (ph - 0.15f)) + noise(0.05f);
}

}  // namespace sim
