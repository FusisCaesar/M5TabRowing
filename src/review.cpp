#include "review.h"

#include <M5Unified.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "recordings.h"

namespace review {

namespace {

constexpr int MAX_SECONDS = 6 * 3600;
constexpr int SMOOTH_HALF = 2;            // 5 s centred average
constexpr float MOVING_MPS = 1.0f;        // slower is drifting or stopped
constexpr float MIN_SPLIT_SPEED = 0.5f;

Summary sum;
bool isLoaded = false;
int n = 0, tn = 0;
float *el = nullptr, *spd = nullptr, *rt = nullptr, *hrs = nullptr;
double *tLat = nullptr, *tLon = nullptr;
uint16_t* tCol = nullptr;
float slowV = 0, fastV = 1;

template <typename T>
T* alloc(int count) {
    return (T*)heap_caps_malloc(sizeof(T) * std::max(count, 1), MALLOC_CAP_SPIRAM);
}

// Splits a CSV line in place; returns the number of fields.
int splitCsv(char* line, char** fields, int max) {
    int k = 0;
    fields[k++] = line;
    for (char* p = line; *p && k < max; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[k++] = p + 1;
        }
    }
    return k;
}

// Calls fn(fields, count) for every data row of a session file.
template <typename Fn>
void forEachRow(int number, const char* stream, Fn fn) {
    char path[96];
    if (!recordings::pathOf(number, stream, path, sizeof(path))) return;
    File f = SD_MMC.open(path);
    if (!f) return;
    char line[256];
    f.readBytesUntil('\n', line, sizeof(line) - 1);  // header
    while (f.available()) {
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len <= 0) continue;
        line[len] = '\0';
        char* fields[20];
        int k = splitCsv(line, fields, 20);
        fn(fields, k);
    }
    f.close();
}

uint16_t speedColor(float v) {
    if (v < MOVING_MPS) return lgfx::color565(140, 140, 140);
    float t = fastV > slowV ? (v - slowV) / (fastV - slowV) : 0.5f;
    t = std::max(0.0f, std::min(1.0f, t));
    uint8_t r = t < 0.5f ? 230 : (uint8_t)(230 * (1 - t) * 2);
    uint8_t g = t < 0.5f ? (uint8_t)(200 * t * 2) : 200;
    return lgfx::color565(r, g, 20);
}

}  // namespace

void unload() {
    heap_caps_free(el);
    heap_caps_free(spd);
    heap_caps_free(rt);
    heap_caps_free(hrs);
    heap_caps_free(tLat);
    heap_caps_free(tLon);
    heap_caps_free(tCol);
    el = spd = rt = hrs = nullptr;
    tLat = tLon = nullptr;
    tCol = nullptr;
    n = tn = 0;
    isLoaded = false;
}

bool load(int number) {
    unload();
    sum = Summary{};
    sum.number = number;
    float listedDuration = 0;
    for (int i = 0; i < recordings::count(); i++) {
        recordings::Info ri;
        if (recordings::get(i, ri) && ri.number == number) {
            strlcpy(sum.label, ri.label, sizeof(sum.label));
            strlcpy(sum.start, ri.start, sizeof(sum.start));
            listedDuration = ri.durationS;
        }
    }

    el = alloc<float>(MAX_SECONDS);
    spd = alloc<float>(MAX_SECONDS);
    rt = alloc<float>(MAX_SECONDS);
    hrs = alloc<float>(MAX_SECONDS);
    tLat = alloc<double>(MAX_SECONDS);
    tLon = alloc<double>(MAX_SECONDS);
    tCol = alloc<uint16_t>(MAX_SECONDS);
    float* dist = alloc<float>(MAX_SECONDS);
    float* raw = alloc<float>(MAX_SECONDS);
    int* trackRow = alloc<int>(MAX_SECONDS);
    if (!el || !spd || !rt || !hrs || !tLat || !tLon || !tCol || !dist || !raw || !trackRow) {
        heap_caps_free(dist);
        heap_caps_free(raw);
        heap_caps_free(trackRow);
        unload();
        return false;
    }

    // elapsed_s,utc,lat,lon,speed_mps,split_s,distance_m,rate_spm,hdop,sats,battery_pct,hr_bpm
    double hrSum = 0;
    int hrN = 0;
    forEachRow(number, "seconds", [&](char** f, int k) {
        if (k < 8 || n >= MAX_SECONDS) return;
        el[n] = atof(f[0]);
        raw[n] = atof(f[4]);
        dist[n] = atof(f[6]);
        rt[n] = atof(f[7]);
        hrs[n] = k >= 12 ? atof(f[11]) : 0;
        if (hrs[n] > 0) {
            hrSum += hrs[n];
            hrN++;
            sum.maxHr = std::max(sum.maxHr, (int)hrs[n]);
        }
        double lat = atof(f[2]), lon = atof(f[3]);
        if (lat != 0 || lon != 0) {
            tLat[tn] = lat;
            tLon[tn] = lon;
            trackRow[tn++] = n;
        }
        n++;
    });

    for (int i = 0; i < n; i++) {
        float s = 0;
        int c = 0;
        for (int j = std::max(0, i - SMOOTH_HALF); j <= std::min(n - 1, i + SMOOTH_HALF); j++) {
            s += raw[j];
            c++;
        }
        spd[i] = s / c;
        sum.maxSpeed = std::max(sum.maxSpeed, spd[i]);
    }
    if (hrN) sum.avgHr = hrSum / hrN;
    sum.hasGps = tn >= 2;
    sum.durationS = n ? el[n - 1] : listedDuration;
    if (n && sum.hasGps) {
        sum.distanceM = dist[n - 1];
        if (sum.distanceM > 10) sum.avgSplitS = sum.durationS * 500 / sum.distanceM;
    }

    // Fastest 500 m: the shortest time between two rows that are 500 m apart.
    for (int i = 0, j = 0; j < n; j++) {
        while (i + 1 < j && dist[j] - dist[i + 1] >= 500) i++;
        if (dist[j] - dist[i] >= 500) {
            float t = (el[j] - el[i]) * 500 / (dist[j] - dist[i]);
            if (!sum.best500S || t < sum.best500S) sum.best500S = t;
        }
    }

    // Colour scale from the 10th to the 90th percentile of rowing speed.
    int m = 0;
    for (int i = 0; i < n; i++)
        if (spd[i] >= MOVING_MPS) raw[m++] = spd[i];
    if (m >= 5) {
        std::sort(raw, raw + m);
        slowV = raw[m / 10];
        fastV = raw[m * 9 / 10];
    } else {
        slowV = MOVING_MPS;
        fastV = 4;
    }
    for (int i = 0; i < tn; i++) tCol[i] = speedColor(spd[trackRow[i]]);

    heap_caps_free(dist);
    heap_caps_free(raw);
    heap_caps_free(trackRow);

    // elapsed_s,stroke,rate_spm,interval_s,drive_s,recovery_s,ratio,split_s,speed_mps,dps_m,...
    double rateSum = 0, dpsSum = 0;
    int rateN = 0, dpsN = 0;
    forEachRow(number, "strokes", [&](char** f, int k) {
        if (k < 10) return;
        sum.strokes++;
        float r = atof(f[2]), d = atof(f[9]);
        if (r > 0) {
            rateSum += r;
            rateN++;
        }
        if (d > 0) {
            dpsSum += d;
            dpsN++;
        }
    });
    if (rateN) sum.avgRate = rateSum / rateN;
    if (dpsN) sum.avgDps = dpsSum / dpsN;
    forEachRow(number, "laps", [&](char**, int) { sum.laps++; });
    if (!sum.hasGps) {
        sum.maxSpeed = 0;
        sum.avgDps = 0;
    }

    isLoaded = n > 0 || sum.strokes > 0 || sum.laps > 0;
    Serial.printf("[review] S%04d: %d s, %d track points, %d strokes, hr avg %.0f\n", number, n, tn, sum.strokes,
                  sum.avgHr);
    if (!isLoaded) unload();
    return isLoaded;
}

bool loaded() { return isLoaded; }
const Summary& summary() { return sum; }
int count() { return n; }
float elapsed(int i) { return el[i]; }
float speed(int i) { return spd[i]; }
float split(int i) { return spd[i] >= MIN_SPLIT_SPEED ? 500.0f / spd[i] : 0; }
float rate(int i) { return rt[i]; }
float hr(int i) { return hrs[i]; }
int trackCount() { return tn; }
const double* trackLat() { return tLat; }
const double* trackLon() { return tLon; }
const uint16_t* trackColor() { return tCol; }

bool trackBounds(double& south, double& west, double& north, double& east) {
    if (!tn) return false;
    south = north = tLat[0];
    west = east = tLon[0];
    for (int i = 1; i < tn; i++) {
        south = std::min(south, tLat[i]);
        north = std::max(north, tLat[i]);
        west = std::min(west, tLon[i]);
        east = std::max(east, tLon[i]);
    }
    return true;
}

void speedScale(float& slow, float& fast) {
    slow = slowV;
    fast = fastV;
}

}  // namespace review
