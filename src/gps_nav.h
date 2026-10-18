#pragma once

#include <stdint.h>

namespace gps {

struct Snapshot {
    bool connected = false;  // NMEA arriving
    bool fix = false;        // recent fix that passes HDOP/satellite checks
    uint32_t baud = 0;
    float speed = 0;         // m/s, speed over ground
    float course = 0;        // deg, course over ground (held when slow/invalid)
    double lat = 0, lon = 0;
    float hdop = 99;
    int sats = 0;
    double distance = 0;     // m since boot, monotonic
    int64_t lastFixUs = 0;
    uint32_t updates = 0;    // increments on every processed fix
    float updateHz = 0;
    bool timeValid = false;
    uint16_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

void begin();
Snapshot get();

}  // namespace gps
