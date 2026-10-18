#pragma once

#include <math.h>

namespace dsp {

// RBJ cookbook biquad, transposed direct form II.
class Biquad {
public:
    void lowpass(float fc, float fs, float q = 0.70710678f) {
        float w0 = 2.0f * (float)M_PI * fc / fs;
        float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
        float a0 = 1.0f + alpha;
        b0_ = (1.0f - c) * 0.5f / a0;
        b1_ = (1.0f - c) / a0;
        b2_ = b0_;
        a1_ = -2.0f * c / a0;
        a2_ = (1.0f - alpha) / a0;
        reset();
    }

    void highpass(float fc, float fs, float q = 0.70710678f) {
        float w0 = 2.0f * (float)M_PI * fc / fs;
        float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
        float a0 = 1.0f + alpha;
        b0_ = (1.0f + c) * 0.5f / a0;
        b1_ = -(1.0f + c) / a0;
        b2_ = b0_;
        a1_ = -2.0f * c / a0;
        a2_ = (1.0f - alpha) / a0;
        reset();
    }

    float process(float x) {
        float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

    void reset() { z1_ = z2_ = 0.0f; }

private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_ = 0, z2_ = 0;
};

class BandPass {
public:
    void setup(float lowHz, float highHz, float fs) {
        hp_.highpass(lowHz, fs);
        lp_.lowpass(highHz, fs);
    }
    float process(float x) { return lp_.process(hp_.process(x)); }
    void reset() { hp_.reset(); lp_.reset(); }

private:
    Biquad hp_, lp_;
};

inline float emaAlpha(float tauS, float dtS) { return dtS / (tauS + dtS); }

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float norm() const { return sqrtf(dot(*this)); }
};

}  // namespace dsp
