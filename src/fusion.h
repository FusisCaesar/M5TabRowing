#pragma once

#include <math.h>

// Complementary filter: high-passed integrated surge acceleration plus
// low-passed GPS speed (PLOS One 2019, "Adaptive smartphone-based sensor fusion").
class SpeedFusion {
public:
    void setCrossoverHz(float fc) { k_ = 2.0f * (float)M_PI * fc; }

    void setGps(float speedMps, bool valid) {
        gps_ = speedMps;
        if (valid && !valid_) v_ = speedMps;
        valid_ = valid;
    }

    void step(float accelMps2, float dt) {
        if (!valid_) return;
        v_ += (accelMps2 + k_ * (gps_ - v_)) * dt;
        if (v_ < 0) v_ = 0;
        dist_ += v_ * dt;
    }

    bool active() const { return valid_; }
    float speed() const { return v_; }
    double distance() const { return dist_; }

private:
    float k_ = 2.0f * (float)M_PI * 0.3f;
    float gps_ = 0;
    bool valid_ = false;
    float v_ = 0;
    double dist_ = 0;
};
