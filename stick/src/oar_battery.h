// Battery level of the StickS3 (250 mAh Li-ion, read through the M5PM1 power chip).
//
// M5Unified turns the voltage into a percentage with one straight line from 3.35 V to 4.15 V. Two things
// make that wrong for this stick:
//   * a Li-ion cell is nowhere near linear: it sits near 3.7 V for most of its charge, so the line reads
//     about 20 points too high in the middle and falls off a cliff at the end;
//   * the voltage reading itself jumps by up to 100 mV from one read to the next (the charger switches
//     while USB is plugged in, and the converter is noisy when it is not), which is 25 points of "level".
// So the voltage is sampled often, the median of the last 10 s is used, the percentage comes from a typical
// discharge curve, and on battery it only ever goes down (until the stick is charged again).
//
// The stick also keeps a battery log in flash (a sample every minute, the last 16 hours) so a discharge
// test can be read back afterwards: send 'h' over USB serial to print it, 'x' to clear it.

#pragma once

#include <M5Unified.h>
#include <Preferences.h>
#include <esp_timer.h>

#include <algorithm>

namespace oarbat {

struct Sample {
    uint16_t upMin;  // minutes since the stick started
    uint16_t mv;     // filtered battery voltage
    uint8_t pct;
    uint8_t flags;   // FLAG_*
};

constexpr uint8_t FLAG_USB = 0x01;     // USB power was present
constexpr uint8_t FLAG_SCREEN = 0x02;  // the screen was on
constexpr uint8_t FLAG_BOOT = 0x04;    // first sample after a start

constexpr int SAMPLE_MS = 250;
constexpr int WINDOW = 40;  // 10 s
constexpr int PER_CHUNK = 60;
constexpr int CHUNKS = 16;  // 16 h of minutes

// Typical Li-ion discharge at a light load (about 0.3 C). Voltage on the left, percent on the right.
struct Point {
    int mv;
    int pct;
};
constexpr Point CURVE[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70}, {3910, 65},
    {3870, 60},  {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35}, {3770, 30}, {3750, 25},
    {3730, 20},  {3710, 15}, {3690, 10}, {3610, 5},  {3500, 2},  {3300, 0},
};
constexpr int CURVE_N = sizeof(CURVE) / sizeof(CURVE[0]);

// While USB charges the cell, its terminal sits above its resting voltage by the charge current times the
// cell resistance. This is a typical value for the StickS3's 250 mAh cell, not a measurement.
constexpr int CHARGING_OFFSET_MV = 60;

inline int percentFromMv(int mv) {
    if (mv >= CURVE[0].mv) return 100;
    for (int i = 1; i < CURVE_N; i++) {
        if (mv >= CURVE[i].mv) {
            int dv = CURVE[i - 1].mv - CURVE[i].mv, dp = CURVE[i - 1].pct - CURVE[i].pct;
            return CURVE[i].pct + (mv - CURVE[i].mv) * dp / dv;
        }
    }
    return 0;
}

namespace detail {

struct State {
    uint16_t ring[WINDOW];
    uint8_t usbRing[WINDOW];
    int n = 0, head = 0;
    uint32_t lastSample = 0, lastMedian = 0, lastLog = 0;
    int mv = 0;
    int pct = 255;
    bool usb = false;
    uint32_t count = 0;  // samples ever written to the log
    Sample cur[PER_CHUNK];
    bool first = true;
    Preferences prefs;
};
inline State& s() {
    static State st;
    return st;
}

inline int median(uint16_t* v, int n) {
    uint16_t tmp[WINDOW];
    memcpy(tmp, v, n * sizeof(uint16_t));
    std::sort(tmp, tmp + n);
    return tmp[n / 2];
}

inline void readOnce() {
    State& st = s();
    int mv = (int)M5.Power.getBatteryVoltage();
    int vbus = (int)M5.Power.getVBUSVoltage();
    if (mv < 2500 || mv > 4600) return;  // a failed read
    st.ring[st.head] = (uint16_t)mv;
    st.usbRing[st.head] = vbus >= 4000;
    st.head = (st.head + 1) % WINDOW;
    if (st.n < WINDOW) st.n++;
}

inline void evaluate() {
    State& st = s();
    if (st.n == 0) return;
    uint16_t tmp[WINDOW];
    int usbVotes = 0;
    for (int i = 0; i < st.n; i++) {
        tmp[i] = st.ring[i];
        usbVotes += st.usbRing[i];
    }
    st.mv = median(tmp, st.n);
    bool usb = usbVotes * 2 > st.n;
    st.usb = usb;
    int raw = percentFromMv(usb ? st.mv - CHARGING_OFFSET_MV : st.mv);
    if (usb) raw = std::min(raw, 99);  // 100 is only believed once the stick is unplugged
    // Charging: follow the voltage. On battery the level only goes down, so a noisy reading never
    // makes it jump back up.
    if (st.pct == 255 || usb) st.pct = raw;
    else st.pct = std::min(raw, st.pct);
}

inline void chunkKey(uint32_t chunk, char* key) { snprintf(key, 8, "c%u", (unsigned)(chunk % CHUNKS)); }

inline void saveChunk() {
    State& st = s();
    uint32_t idx = st.count - 1;  // the sample just added
    char key[8];
    chunkKey(idx / PER_CHUNK, key);
    int used = (int)(idx % PER_CHUNK) + 1;
    st.prefs.putBytes(key, st.cur, used * sizeof(Sample));
    st.prefs.putUInt("n", st.count);
}

}  // namespace detail

inline void begin() {
    detail::State& st = detail::s();
    st.prefs.begin("oarbat", false);
    st.count = st.prefs.getUInt("n", 0);
    if (st.count % PER_CHUNK) {  // continue in the chunk that is half full
        char key[8];
        detail::chunkKey(st.count / PER_CHUNK, key);
        st.prefs.getBytes(key, st.cur, (st.count % PER_CHUNK) * sizeof(Sample));
    }
    for (int i = 0; i < 12; i++) {  // a first value straight away
        detail::readOnce();
        delay(20);
    }
    detail::evaluate();
}

inline int percent() { return detail::s().pct; }  // 0..100, 255 while unknown
inline int millivolts() { return detail::s().mv; }
inline bool usb() { return detail::s().usb; }

inline void update(uint32_t nowMs, bool screenOn) {
    detail::State& st = detail::s();
    if (nowMs - st.lastSample >= SAMPLE_MS) {
        st.lastSample = nowMs;
        detail::readOnce();
    }
    if (nowMs - st.lastMedian >= 2000) {
        st.lastMedian = nowMs;
        detail::evaluate();
    }
    if (nowMs - st.lastLog >= 60000 || (st.lastLog == 0 && nowMs > 15000)) {
        st.lastLog = nowMs ? nowMs : 1;
        if (st.pct == 255) return;
        Sample smp;
        smp.upMin = (uint16_t)std::min<uint32_t>(nowMs / 60000, 65535);
        smp.mv = (uint16_t)st.mv;
        smp.pct = (uint8_t)st.pct;
        smp.flags = (st.usb ? FLAG_USB : 0) | (screenOn ? FLAG_SCREEN : 0) | (st.first ? FLAG_BOOT : 0);
        st.first = false;
        st.cur[st.count % PER_CHUNK] = smp;
        st.count++;
        detail::saveChunk();
        if (st.count % PER_CHUNK == 0) memset(st.cur, 0, sizeof(st.cur));
    }
}

// One line per logged minute: index, minutes since start, volts, percent, flags (u = USB, s = screen on,
// b = first sample after a start).
inline void dump(Print& out) {
    detail::State& st = detail::s();
    uint32_t total = st.count;
    uint32_t first = total > (uint32_t)(PER_CHUNK * CHUNKS) ? total - PER_CHUNK * CHUNKS : 0;
    out.printf("[bat-hist] %u samples\n", (unsigned)(total - first));
    Sample chunk[PER_CHUNK];
    for (uint32_t c = first / PER_CHUNK; c * PER_CHUNK < total; c++) {
        char key[8];
        detail::chunkKey(c, key);
        size_t got = st.prefs.getBytes(key, chunk, sizeof(chunk)) / sizeof(Sample);
        for (uint32_t k = 0; k < got; k++) {
            uint32_t idx = c * PER_CHUNK + k;
            if (idx < first || idx >= total) continue;
            const Sample& x = chunk[k];
            out.printf("[bat-hist] %u,%u,%u,%u,%s%s%s\n", (unsigned)idx, x.upMin, x.mv, x.pct,
                       x.flags & FLAG_USB ? "u" : "", x.flags & FLAG_SCREEN ? "s" : "",
                       x.flags & FLAG_BOOT ? "b" : "");
        }
    }
    out.println("[bat-hist] end");
}

inline void clear() {
    detail::State& st = detail::s();
    st.prefs.clear();
    st.count = 0;
    memset(st.cur, 0, sizeof(st.cur));
    st.first = true;
}

}  // namespace oarbat
