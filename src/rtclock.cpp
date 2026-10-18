#include "rtclock.h"

#include <M5Unified.h>
#include <esp_sntp.h>
#include <stdlib.h>
#include <sys/time.h>

#include "board.h"
#include "config.h"
#include "gps_nav.h"
#include "net.h"

namespace rtclock {

namespace {

constexpr int MIN_YEAR = 2024;
constexpr time_t MAX_DRIFT_S = 2;

const char* src = "none";
bool gpsSynced = false;
bool ntpStarted = false;
volatile bool ntpDone = false;
uint32_t lastCheck = 0;

// Seconds since 1970 for a UTC date and time (mktime would apply the local time zone).
time_t epochUtc(int y, int mo, int d, int h, int mi, int s) {
    y -= mo <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + h * 3600 + mi * 60 + s;
}

void writeRtc(time_t t) {
    tm u;
    gmtime_r(&t, &u);
    board::I2CLock lock;
    M5.Rtc.setDateTime(&u);
}

void setSystem(time_t t) {
    timeval tv{t, 0};
    settimeofday(&tv, nullptr);
}

void logNow(const char* what) {
    char iso[24], local[24];
    isoUtc(iso, sizeof(iso));
    localText(time(nullptr), local, sizeof(local));
    Serial.printf("[clock] %s: %s (local %s)\n", what, iso, local);
}

void onNtpSync(timeval*) { ntpDone = true; }

}  // namespace

void begin() {
    setenv("TZ", cfg::TIMEZONE, 1);
    tzset();
    if (valid()) {
        src = "RTC";
        logNow("from RTC");
    } else {
        Serial.println("[clock] RTC not set; waiting for GPS or internet time");
    }
}

void update() {
    if (millis() - lastCheck < 1000) return;
    lastCheck = millis();

#if !SIM_MODE
    // The simulator's GPS reports a fixed made-up time, so only a real receiver sets the clock.
    gps::Snapshot g = gps::get();
    if (g.fix && g.timeValid && g.year >= MIN_YEAR) {
        time_t t = epochUtc(g.year, g.month, g.day, g.hour, g.minute, g.second);
        time_t diff = time(nullptr) - t;
        if (!gpsSynced || diff > MAX_DRIFT_S || diff < -MAX_DRIFT_S) {
            setSystem(t);
            writeRtc(t);
            gpsSynced = true;
            src = "GPS";
            logNow("set from GPS");
        }
    }
#endif

    if (!ntpStarted && net::connected()) {
        ntpStarted = true;
        sntp_set_time_sync_notification_cb(onNtpSync);
        configTzTime(cfg::TIMEZONE, cfg::NTP_SERVER_1, cfg::NTP_SERVER_2);
    }
    if (ntpDone) {
        ntpDone = false;
        if (valid()) {
            writeRtc(time(nullptr));
            if (!gpsSynced) src = "NTP";
            logNow("set from internet time");
        }
    }
}

bool valid() {
    time_t t = time(nullptr);
    tm u;
    gmtime_r(&t, &u);
    return u.tm_year + 1900 >= MIN_YEAR;
}

const char* source() { return valid() ? src : "none"; }

bool isoUtc(char* out, size_t len) {
    if (!valid()) {
        if (len) out[0] = '\0';
        return false;
    }
    time_t t = time(nullptr);
    tm u;
    gmtime_r(&t, &u);
    strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &u);
    return true;
}

time_t parseIsoUtc(const char* iso) {
    int y, mo, d, h, mi, s;
    if (!iso || sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    return epochUtc(y, mo, d, h, mi, s);
}

void localText(time_t t, char* out, size_t len) {
    tm l;
    localtime_r(&t, &l);
    strftime(out, len, "%Y-%m-%d %H:%M", &l);
}

}  // namespace rtclock
