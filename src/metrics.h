#pragma once

#include <stdint.h>

#include "hr.h"

namespace metrics {

enum class State : uint8_t { Ready, Running, Stopped };

struct Lap {
    int number = 0;
    float time = 0;      // s
    float distance = 0;  // m
    float avgSplit = 0;  // s / 500 m
    float avgRate = 0;   // spm
    int strokes = 0;
    float avgHr = 0;  // bpm, 0 = no readings
    int maxHr = 0;    // bpm
};

struct View {
    State state = State::Ready;
    float rate = 0;        // spm, 0 = no strokes
    float split = 0;       // s / 500 m, 0 = unknown
    float speed = 0;       // m/s
    float distance = 0;    // m, session
    float elapsed = 0;     // s, session
    float dps = 0;         // m, last stroke
    float avgSplit = 0;    // s / 500 m, session
    float avgRate = 0;     // spm, session
    int strokes = 0;       // session
    float ratio = 0;       // recovery / drive, averaged
    float checkMg = 0;     // averaged
    float bounceMg = 0;    // averaged
    bool dpsFused = false;

    bool gpsConnected = false;
    bool gpsFix = false;
    int sats = 0;
    float hdop = 99;
    float gpsHz = 0;

    hr::State hrState = hr::State::Off;
    bool hrConnected = false;
    int hr = 0;        // bpm, 0 = no fresh reading
    float avgHr = 0;   // bpm, session
    int maxHr = 0;     // bpm, session

    int battery = -1;  // 0..100, -1 unknown, -2 USB with no pack
    int session = -1;
    int lapNumber = 1;
    bool hasLap = false;
    Lap lastLap;
    uint32_t lapShownMs = 0;
};

void begin();
void update();

void start();
void stop();
// One line in the open session's info file. Ignored when nothing is recording.
void note(const char* key, const char* value);
void lap();
void reset();
void setBattery(int pct);

View view();

}  // namespace metrics
