#pragma once

#include <stdint.h>

// Heart rate from a Bluetooth sensor with the standard Heart Rate Service: a chest strap, or a
// Garmin watch with Broadcast Heart Rate on. Bluetooth runs in its own task (connecting blocks).
namespace hr {

enum class State : uint8_t { Off, Scanning, Connecting, Connected, Lost };

struct Snapshot {
    State state = State::Off;
    int bpm = 0;         // 0 = no fresh reading
    uint32_t ageMs = 0;  // since the last reading
    int rrMs = 0;        // last beat-to-beat interval, 0 if the sensor doesn't send them
    char name[24] = "";  // saved sensor, empty if none
};

void begin();

// Scans, saves the strongest heart rate sensor in range and connects to it.
void startPairing();
void forget();
bool hasSaved();
bool pairing();

Snapshot get();
const char* stateName(State s);

}  // namespace hr
