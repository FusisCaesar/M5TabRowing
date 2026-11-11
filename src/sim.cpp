#include "sim.h"

#include <math.h>
#include <stdlib.h>

namespace sim {

namespace {
constexpr double PERIOD_S = 120.0;
constexpr double W = 2.0 * M_PI / PERIOD_S;
constexpr float TILT_RAD = 35.0f * (float)M_PI / 180.0f;

float noise(float amp) { return amp * ((rand() / (float)RAND_MAX) * 2.0f - 1.0f); }

// Slow drifts, so a session contains both good strokes and ones the Stroke screen should flag.
// +1 is the better end of each (sharper catch, stronger drive, later dip, cleaner release).
float driveShape(double t) { return sinf((float)t / 17.0f); }
float catchShape(double t) { return sinf((float)t / 23.0f); }
float approachShape(double t) { return sinf((float)t / 29.0f); }
float releaseShape(double t) { return sinf((float)t / 31.0f); }
float notchShape(double t) { return fmaxf(0.0f, sinf((float)t / 13.0f)); }

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

    // Catch width, drive strength, release notch and the run-in to the next catch all drift,
    // so the stroke screen has something to compare against the rower's own best.
    float catchSigma = 0.055f - 0.018f * catchShape(t);
    float driveGain = 1.0f + 0.22f * driveShape(t);
    float approachLead = 0.24f - 0.10f * approachShape(t);
    float releaseAmp = 0.015f + 0.030f * (1.0f - releaseShape(t));

    float surge = -0.55f * gauss(tw, 0.0f, catchSigma);
    surge += -0.30f * notchShape(t) * gauss(tw, 0.055f, 0.022f);
    surge += 0.22f * driveGain * gauss(tau, 0.26f, 0.09f);
    surge += 0.16f * driveGain * gauss(tau, 0.50f, 0.11f);
    surge += -releaseAmp * gauss(tau, 0.72f, 0.035f);
    surge += -0.028f;
    if (tau > period - approachLead) surge += -0.08f * (tau - (period - approachLead)) / approachLead;

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
    // Better catches and drives go faster, so those strokes become the "best" reference.
    float quality = 1.0f + 0.07f * (0.5f * driveShape(t) + 0.3f * catchShape(t) + 0.2f * approachShape(t));
    float base = (3.2f + 0.08f * (r - 20.0f)) * quality;
    float ph = strokePhase(t);
    return base + 0.40f * sinf(2.0f * (float)M_PI * (ph - 0.08f)) + noise(0.04f);
}

}  // namespace sim
