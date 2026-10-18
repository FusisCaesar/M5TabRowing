#pragma once

#include <stdint.h>

// Breadcrumb of where the boat has been, for drawing on the map. Records one point
// every few metres while a session is running and there is a GPS fix. Stored in
// PSRAM. Call update() from the UI task; it reads the GPS and session state.
namespace track {

struct Point {
    double lat, lon;
};

void begin();
void update();
void clear();

int count();
Point get(int i);  // 0 = oldest kept, count()-1 = newest

}  // namespace track
