#pragma once

#include <stdint.h>

// OpenStreetMap raster tiles (256 x 256), fetched over Wi-Fi in a background task
// and kept decoded in PSRAM, as the background for drawing a map area. Only for
// interactive use while choosing an area (OSM tile usage policy).
namespace tiles {

constexpr int SIZE = 256;

void begin();

// Call before requesting the tiles of a frame: forgets requests for tiles no longer in view.
void beginFrame();

// Locks the cache while the caller copies tile pixels; get() needs the lock held.
void lock();
void unlock();

// Byte-swapped RGB565 pixels (as M5Canvas stores them), or nullptr; a missing
// tile is queued for download.
const uint16_t* get(int z, int x, int y);

// Increments whenever a tile arrives, so the view knows to redraw.
uint32_t version();

// Frees the decoded tiles (when leaving the area picker).
void clear();

}  // namespace tiles
