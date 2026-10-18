#pragma once

// Synthetic rowing for bench testing (build with -DSIM_MODE=1).
// Rate sweeps 20 -> 32 -> 20 spm over two minutes.
namespace sim {

float rateSpm(double t);
float strokePhase(double t);  // 0..1, 0 = catch
void accel(double t, float& ax, float& ay, float& az);
float boatSpeed(double t);

}  // namespace sim
