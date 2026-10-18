#pragma once

#include <stddef.h>
#include <time.h>

// Real date and time. M5.begin() copies the battery-backed RTC into the system clock;
// this keeps both right from the GPS (real receiver only) and from internet time while
// Wi-Fi is connected. The RTC and system clock hold UTC.
namespace rtclock {

void begin();
void update();  // call from the main loop

bool valid();             // the clock has been set at some point (year >= 2024)
const char* source();     // "GPS", "NTP", "RTC" or "none"

// "2026-10-18T19:30:05Z"; false (and empty) when the clock is not set.
bool isoUtc(char* out, size_t len);
// Parses "YYYY-MM-DDTHH:MM:SSZ"; 0 when it is not a time.
time_t parseIsoUtc(const char* iso);
// "2026-10-18 21:30" in local time.
void localText(time_t t, char* out, size_t len);

}  // namespace rtclock
