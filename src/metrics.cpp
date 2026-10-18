#include "metrics.h"

#include <Arduino.h>

#include "board.h"
#include "config.h"
#include "gps_nav.h"
#include "imu_stroke.h"
#include "logger.h"
#include "settings.h"

namespace metrics {

namespace {

struct SpeedSample {
    int64_t tUs;
    float speed;
    bool valid;
};

View v;
SpeedSample speeds[cfg::SPEED_HISTORY];
int speedHead = 0;
int speedCount = 0;
uint32_t lastGpsUpdates = 0;
gps::Snapshot gpsNow;

double prevCatch = -1;
double prevFused = -1;
float strokeSplit = 0;

float ratios[cfg::TECHNIQUE_AVG_STROKES], checks[cfg::TECHNIQUE_AVG_STROKES],
    bounces[cfg::TECHNIQUE_AVG_STROKES];
int techHead = 0, techCount = 0;

int64_t startUs = 0;
double startDistance = 0;
double rateSum = 0;
int rateCount = 0;
int lastLoggedSecond = -1;

int64_t lapStartUs = 0;
double lapStartDistance = 0;
int lapStrokes = 0;
double lapRateSum = 0;
int lapRateCount = 0;

void pushSpeed(int64_t t, float s, bool valid) {
    speeds[speedHead] = {t, s, valid};
    speedHead = (speedHead + 1) % cfg::SPEED_HISTORY;
    if (speedCount < cfg::SPEED_HISTORY) speedCount++;
}

// Mean GPS speed over [fromUs, toUs]; returns -1 if there aren't enough good samples.
float meanSpeed(int64_t fromUs, int64_t toUs) {
    double sum = 0;
    int n = 0;
    for (int i = 0; i < speedCount; i++) {
        const SpeedSample& s = speeds[(speedHead - 1 - i + cfg::SPEED_HISTORY) % cfg::SPEED_HISTORY];
        if (s.tUs < fromUs) break;
        if (s.tUs > toUs) continue;
        if (!s.valid) return -1;
        sum += s.speed;
        n++;
    }
    return n >= 3 ? (float)(sum / n) : -1;
}

float splitFromSpeed(float s) { return s >= cfg::MIN_SPLIT_SPEED_MPS ? 500.0f / s : 0; }

float avgOf(const float* a) {
    float sum = 0;
    int n = 0;
    for (int i = 0; i < techCount; i++) {
        if (a[i] > 0) {
            sum += a[i];
            n++;
        }
    }
    return n ? sum / n : 0;
}

void onStroke(const StrokeEvent& ev) {
    int64_t catchUs = (int64_t)(ev.catchTime * 1e6);
    float meanV = -1;
    float dps = 0;
    bool fused = false;

    if (ev.interval > 0 && prevCatch > 0) {
        meanV = meanSpeed((int64_t)(prevCatch * 1e6), catchUs);
        strokeSplit = meanV > 0 ? splitFromSpeed(meanV) : 0;
        if (settings::get().fusion && ev.fusedDistanceM >= 0 && prevFused >= 0) {
            dps = (float)(ev.fusedDistanceM - prevFused);
            fused = true;
        } else if (meanV > 0) {
            dps = meanV * ev.interval;
        }
    } else {
        strokeSplit = 0;
    }
    prevCatch = ev.catchTime;
    prevFused = ev.fusedDistanceM;
    v.dps = dps;
    v.dpsFused = fused;

    float ratio = ev.driveTime > 0 ? ev.recoveryTime / ev.driveTime : 0;
    ratios[techHead] = ratio;
    checks[techHead] = ev.checkMg;
    bounces[techHead] = ev.bounceMg;
    techHead = (techHead + 1) % cfg::TECHNIQUE_AVG_STROKES;
    if (techCount < cfg::TECHNIQUE_AVG_STROKES) techCount++;
    v.ratio = avgOf(ratios);
    v.checkMg = avgOf(checks);
    v.bounceMg = avgOf(bounces);

    Serial.printf("[stroke] #%lu rate %.1f int %.2fs drive %.2f rec %.2f split %.1f dps %.2f%s\n",
                  (unsigned long)ev.seq, ev.rateSpm, ev.interval, ev.driveTime, ev.recoveryTime,
                  strokeSplit, dps, fused ? " (fused)" : "");

    if (v.state != State::Running) return;
    v.strokes++;
    lapStrokes++;
    if (ev.rateSpm > 0) {
        rateSum += ev.rateSpm;
        rateCount++;
        lapRateSum += ev.rateSpm;
        lapRateCount++;
    }
    char line[200];
    float elapsed = (float)((catchUs - startUs) * 1e-6);
    snprintf(line, sizeof(line),
             "%.2f,%d,%.1f,%.3f,%.3f,%.3f,%.2f,%.1f,%.2f,%.2f,%.1f,%.3f,%.1f,%.1f,%.7f,%.7f\n",
             elapsed, v.strokes, ev.rateSpm, ev.interval, ev.driveTime, ev.recoveryTime, ratio,
             strokeSplit, meanV > 0 ? meanV : 0, dps, v.distance, ev.peakDecelG, ev.checkMg,
             ev.bounceMg, gpsNow.lat, gpsNow.lon);
    logger::write(logger::Stream::Strokes, line);
}

void logSecond(int second) {
    char utc[24] = "";
    if (gpsNow.timeValid) {
        snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02uZ", gpsNow.year, gpsNow.month,
                 gpsNow.day, gpsNow.hour, gpsNow.minute, gpsNow.second);
    }
    char line[200];
    snprintf(line, sizeof(line), "%d,%s,%.7f,%.7f,%.2f,%.1f,%.1f,%.1f,%.1f,%d,%d\n", second, utc,
             gpsNow.lat, gpsNow.lon, gpsNow.speed, v.split, v.distance, v.rate, gpsNow.hdop,
             gpsNow.sats, v.battery);
    logger::write(logger::Stream::Seconds, line);
}

void closeLap(int64_t nowUs) {
    Lap l;
    l.number = v.lapNumber;
    l.time = (float)((nowUs - lapStartUs) * 1e-6);
    l.distance = (float)(gpsNow.distance - lapStartDistance);
    l.avgSplit = l.distance > 1 ? l.time * 500.0f / l.distance : 0;
    l.avgRate = lapRateCount ? (float)(lapRateSum / lapRateCount) : 0;
    l.strokes = lapStrokes;
    v.lastLap = l;
    v.hasLap = true;
    v.lapShownMs = millis();

    char line[160];
    snprintf(line, sizeof(line), "%d,%.1f,%.1f,%.1f,%.1f,%.1f,%d\n", l.number, v.elapsed, l.time,
             l.distance, l.avgSplit, l.avgRate, l.strokes);
    logger::write(logger::Stream::Laps, line);
    Serial.printf("[lap] %d time %.1f dist %.1f split %.1f rate %.1f\n", l.number, l.time,
                  l.distance, l.avgSplit, l.avgRate);

    v.lapNumber++;
    lapStartUs = nowUs;
    lapStartDistance = gpsNow.distance;
    lapStrokes = 0;
    lapRateSum = 0;
    lapRateCount = 0;
}

void clearSession() {
    v.distance = 0;
    v.elapsed = 0;
    v.avgSplit = 0;
    v.avgRate = 0;
    v.strokes = 0;
    v.lapNumber = 1;
    v.hasLap = false;
    rateSum = 0;
    rateCount = 0;
    lastLoggedSecond = -1;
}

}  // namespace

void begin() { clearSession(); }

void update() {
    int64_t now = board::nowUs();
    gpsNow = gps::get();
    if (gpsNow.updates != lastGpsUpdates) {
        lastGpsUpdates = gpsNow.updates;
        pushSpeed(gpsNow.fix ? gpsNow.lastFixUs : now, gpsNow.speed, gpsNow.fix);
    }
    imu::setGpsSpeed(gpsNow.speed, gpsNow.fix);

    StrokeEvent ev;
    while (imu::popStroke(ev)) onStroke(ev);

    imu::Live live = imu::live();
    v.rate = live.rate;
    bool rowing = live.lastCatch > 0 && (now * 1e-6 - live.lastCatch) < cfg::STROKE_TIMEOUT_S;
    if (rowing && strokeSplit > 0) {
        v.split = strokeSplit;
    } else if (rowing) {
        v.split = 0;
    } else {
        float s = meanSpeed(now - (int64_t)(cfg::SPLIT_IDLE_WINDOW_S * 1e6), now);
        v.split = s > 0 ? splitFromSpeed(s) : 0;
    }
    if (!gpsNow.fix) v.split = 0;
    if (!rowing) v.dps = 0;

    v.speed = gpsNow.speed;
    v.gpsConnected = gpsNow.connected;
    v.gpsFix = gpsNow.fix;
    v.sats = gpsNow.sats;
    v.hdop = gpsNow.hdop;
    v.gpsHz = gpsNow.updateHz;
    v.session = logger::sessionOpen() ? logger::sessionNumber() : -1;

    if (v.state == State::Running) {
        v.elapsed = (float)((now - startUs) * 1e-6);
        v.distance = (float)(gpsNow.distance - startDistance);
        v.avgSplit = v.distance > 10 ? v.elapsed * 500.0f / v.distance : 0;
        v.avgRate = rateCount ? (float)(rateSum / rateCount) : 0;
        int sec = (int)v.elapsed;
        if (sec != lastLoggedSecond) {
            lastLoggedSecond = sec;
            logSecond(sec);
        }
    }
}

void start() {
    if (v.state == State::Running) return;
    clearSession();
    startUs = board::nowUs();
    startDistance = gps::get().distance;
    lapStartUs = startUs;
    lapStartDistance = startDistance;
    lapStrokes = 0;
    lapRateSum = 0;
    lapRateCount = 0;
    int s = logger::startSession();
    v.state = State::Running;
    Serial.printf("[session] start, log session %d\n", s);
}

void stop() {
    if (v.state != State::Running) return;
    int64_t now = board::nowUs();
    update();
    if (v.lapNumber > 1 || v.elapsed > 1) closeLap(now);
    v.state = State::Stopped;
    logger::endSession();
    Serial.printf("[session] stop, %.1f s, %.1f m, %d strokes\n", v.elapsed, v.distance, v.strokes);
}

void lap() {
    if (v.state != State::Running) return;
    closeLap(board::nowUs());
}

void reset() {
    if (v.state == State::Running) return;
    clearSession();
    v.state = State::Ready;
}

void setBattery(int pct) { v.battery = pct; }

View view() { return v; }

}  // namespace metrics
