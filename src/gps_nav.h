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

    // Diagnostics. scanning is true while baud and pin order are still being tried.
    bool scanning = false;
    int rxPin = 0;             // Grove pin the receiver's TX is wired to
    int txPin = 0;
    uint32_t sentences = 0;    // NMEA sentences with a valid checksum
    uint32_t failed = 0;       // checksum failures
    uint32_t fixes = 0;        // sentences that carried a position
    uint32_t chars = 0;        // bytes read from the receiver
    uint32_t lastByteMs = 0;   // millis() of the last byte, 0 = none yet
    char sentence[88] = "";    // last complete NMEA sentence
};

void begin();
Snapshot get();

}  // namespace gps
