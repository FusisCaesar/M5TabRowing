#include "track.h"

#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "gps_nav.h"
#include "metrics.h"

namespace track {

namespace {

Point* buf = nullptr;
int head = 0;   // index of next write
int size = 0;   // number of valid points (<= CAP)
bool full = false;
bool wasRunning = false;

}  // namespace

void begin() {
    buf = (Point*)heap_caps_malloc(sizeof(Point) * cfg::TRACK_MAX_POINTS, MALLOC_CAP_SPIRAM);
    if (!buf) Serial.println("[track] PSRAM alloc failed");
}

void clear() {
    head = 0;
    size = 0;
    full = false;
}

void update() {
    if (!buf) return;
    bool running = metrics::view().state == metrics::State::Running;
    // Start of a session: begin a fresh track.
    if (running && !wasRunning) clear();
    wasRunning = running;
    if (!running) return;

    gps::Snapshot g = gps::get();
    if (!g.fix) return;

    if (size > 0) {
        int last = (head - 1 + cfg::TRACK_MAX_POINTS) % cfg::TRACK_MAX_POINTS;
        double d = TinyGPSPlus::distanceBetween(buf[last].lat, buf[last].lon, g.lat, g.lon);
        if (d < cfg::TRACK_MIN_STEP_M) return;
    }
    buf[head] = {g.lat, g.lon};
    head = (head + 1) % cfg::TRACK_MAX_POINTS;
    if (full) {
        // overwrote the oldest; size stays at CAP
    } else if (++size >= cfg::TRACK_MAX_POINTS) {
        size = cfg::TRACK_MAX_POINTS;
        full = true;
    }
}

int count() { return size; }

Point get(int i) {
    if (!buf || i < 0 || i >= size) return {0, 0};
    int start = full ? head : 0;
    return buf[(start + i) % cfg::TRACK_MAX_POINTS];
}

}  // namespace track
