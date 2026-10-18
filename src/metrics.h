#pragma once

#include <stdint.h>

namespace metrics {

enum class State : uint8_t { Ready, Running, Stopped };

struct Lap {
    int number = 0;
    float time = 0;      // s
    float distance = 0;  // m
    float avgSplit = 0;  // s / 500 m
    float avgRate = 0;   // spm
    int strokes = 0;
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

    int battery = -1;
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
void lap();
void reset();
void setBattery(int pct);

View view();

}  // namespace metrics
