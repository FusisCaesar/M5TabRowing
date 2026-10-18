#pragma once

#include <stdint.h>

// Built-in SC2356 camera (MIPI-CSI). Streams only while active; the UI draws the
// newest frame with draw(), which crops, scales and rotates it with the PPA
// straight into the display framebuffer.
namespace camera {

enum class Status : uint8_t { Off, Starting, Running, Failed };

void begin();
void setActive(bool on);
bool active();
Status status();

// Draws the newest frame not drawn yet into a rectangle in display coordinates,
// scaled to cover it and centre-cropped. Mirror gives a rear-view-mirror image;
// rotate180 turns the image relative to the current display rotation. Call from
// the task that draws the UI. Returns false if there was no new frame.
bool draw(int x, int y, int w, int h, bool mirror, bool rotate180);

// Frames drawn per second.
float fps();

}  // namespace camera
