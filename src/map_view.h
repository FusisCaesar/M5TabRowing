#pragma once

#include <M5Unified.h>

// Draws the loaded waterway map, the recorded track and the boat into a canvas.
// The caller sizes the canvas and pushes it (via fb::push). Pure drawing: it reads
// map_data and track but keeps no screen state.
namespace mapview {

struct View {
    double lat = 0, lon = 0;   // boat position
    float course = 0;          // deg, course over ground
    bool haveFix = false;
    float metersPerPixel = 1;  // zoom
    bool headUp = false;       // rotate so the course points up (else north up)
    bool showControls = true;  // draw on-map zoom buttons

    // Review: draw this recorded track (one colour per point) centred on lat/lon instead of
    // the live track and boat. Works without a map file for the area.
    const double* trackLat = nullptr;
    const double* trackLon = nullptr;
    const uint16_t* trackColor = nullptr;
    int trackCount = 0;
};

void begin();  // allocates scratch buffers in PSRAM
void render(M5Canvas& c, const View& v);

// Screen rectangle of the on-map zoom buttons, in canvas coordinates, for hit
// testing. Returns false if controls are hidden.
bool zoomInRect(const M5Canvas& c, int& x, int& y, int& w, int& h);
bool zoomOutRect(const M5Canvas& c, int& x, int& y, int& w, int& h);

}  // namespace mapview
