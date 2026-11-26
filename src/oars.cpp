#include "oars.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/semphr.h>
#include <math.h>

#include <atomic>

#include "board.h"
#include "logger.h"
#include "oar_protocol.h"

#if SIM_MODE
#include "sim.h"
#endif

namespace oars {
namespace {

constexpr int RING = 8;                  // strokes kept per seat
constexpr int AVG = 5;                   // strokes the averages run over
constexpr int OFFSET_BUCKETS = 32;       // one per second
constexpr int64_t OFFSET_WINDOW_S = 30;  // the clock offset is the best (lowest) of this many seconds
constexpr double MATCH_S = 0.6;          // catches this close are the same stroke
constexpr uint32_t WAIT_REF_MS = 2500;   // wait this long for the reference seat to report a stroke
constexpr uint32_t GROUP_WAIT_MS = 700;  // let the other seats report before the spread is taken
constexpr uint32_t PAIR_TIMEOUT_MS = 20000;
constexpr int PAIR_MIN_RSSI = -95;
constexpr uint32_t REF_ACTIVE_MS = 6000;  // a seat counts as rowing if it reported a stroke this recently

struct Rec {
    bool used = false, resolved = false, groupDone = false;
    uint8_t no = 0;
    int64_t catchUs = 0, finishUs = 0;  // the stick's clock
    float catchDeg = 0, finishDeg = 0, recoveryS = 0;
    uint32_t seenMs = 0;
};

struct Hist {
    bool hasOff = false;
    float catchOff = 0, finishOff = 0, arc = 0, drive = 0, recovery = 0;
};

struct Seat {
    uint16_t id = 0;  // 0 = no stick

    // The stick's clock, unwrapped, and how it relates to ours.
    bool init = false;
    uint32_t last32 = 0;
    int64_t stickUs = 0;
    int64_t bucketMin[OFFSET_BUCKETS] = {};
    int64_t bucketSec[OFFSET_BUCKETS] = {};
    int64_t offsetUs = 0;  // our clock minus the stick's

    bool heard = false;
    uint32_t lastRecvMs = 0;
    int rssi = 0;
    int battery = -1;
    bool zeroed = false;
    float angle = 0;
    uint32_t pktCount = 0, rateStartMs = 0;
    float pps = 0;

    int strokeTotal = 0;
    int lastStrokeNo = -1;
    uint32_t lastStrokeMs = 0;
    int64_t lastCatchUs = 0;
    Rec ring[RING];
    int ringHead = 0;

    Hist hist[AVG];
    int histHead = 0, histN = 0;
    bool hasLast = false;
    Hist last;
    float lastCatchDeg = 0, lastFinishDeg = 0;
};

SemaphoreHandle_t mutex = nullptr;
Seat seats[MAX_SEATS];
std::atomic<int> pairedCount{0};
std::atomic<int> pairSeat{-1};
uint32_t pairStartMs = 0;

float spreadC[AVG], spreadF[AVG];
int spreadHead = 0, spreadN = 0;

class Lock {
public:
    Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

void resetSeatClock(Seat& s) {
    s.init = false;
    s.offsetUs = 0;
    for (int i = 0; i < OFFSET_BUCKETS; i++) {
        s.bucketSec[i] = -1;
        s.bucketMin[i] = 0;
    }
    s.lastStrokeNo = -1;
    s.lastCatchUs = 0;
    for (auto& r : s.ring) r = Rec{};
    s.ringHead = 0;
}

void resetSeat(Seat& s) {
    uint16_t id = s.id;
    s = Seat{};
    s.id = id;
    resetSeatClock(s);
}

void countPaired() {
    int n = 0;
    for (auto& s : seats)
        if (s.id) n++;
    pairedCount = n;
}

void savePrefs() {
#if !SIM_MODE
    Preferences prefs;
    prefs.begin("oars", false);
    for (int i = 0; i < MAX_SEATS; i++) {
        char key[4];
        snprintf(key, sizeof(key), "s%d", i);
        prefs.putUShort(key, seats[i].id);
    }
    prefs.end();
#endif
}

bool online(const Seat& s, uint32_t nowMs) { return s.id && s.heard && nowMs - s.lastRecvMs < ONLINE_MS; }

double tabS(const Seat& s, int64_t stickUs) { return (double)(stickUs + s.offsetUs) * 1e-6; }

// The offset between the clocks: every packet gives "arrival minus stick clock", which is the true offset
// plus however long that packet took to get here. That is never negative, so the lowest value over the
// last half minute is the best estimate, and it follows the drift of the two crystals.
void noteOffsetSample(Seat& s, int64_t recvUs) {
    int64_t sample = recvUs - s.stickUs;
    int64_t sec = recvUs / 1000000;
    int idx = (int)(sec % OFFSET_BUCKETS);
    if (s.bucketSec[idx] != sec) {
        s.bucketSec[idx] = sec;
        s.bucketMin[idx] = sample;
    } else if (sample < s.bucketMin[idx]) {
        s.bucketMin[idx] = sample;
    }
    bool have = false;
    int64_t best = 0;
    for (int i = 0; i < OFFSET_BUCKETS; i++) {
        if (s.bucketSec[i] < 0 || s.bucketSec[i] <= sec - OFFSET_WINDOW_S || s.bucketSec[i] > sec) continue;
        if (!have || s.bucketMin[i] < best) {
            best = s.bucketMin[i];
            have = true;
        }
    }
    if (have) s.offsetUs = best;
}

void ingest(int k, const oarproto::Packet& p, int rssi, int64_t recvUs) {
    Seat& s = seats[k];
    uint32_t nowMs = (uint32_t)(recvUs / 1000);
    if (!s.init) {
        s.init = true;
        s.stickUs = (int64_t)p.ms * 1000;
        s.last32 = p.ms;
    } else {
        int32_t d = (int32_t)(p.ms - s.last32);
        if (d == 0) return;  // the same packet again
        if (d < 0) {
            if (d > -3000) return;  // out of order
            // The stick restarted: its clock began again.
            resetSeatClock(s);
            s.init = true;
            s.stickUs = (int64_t)p.ms * 1000;
        } else {
            s.stickUs += (int64_t)d * 1000;
        }
        s.last32 = p.ms;
    }
    noteOffsetSample(s, recvUs);

    s.heard = true;
    s.lastRecvMs = nowMs;
    s.rssi = rssi;
    s.battery = p.battery <= 100 ? p.battery : -1;
    s.zeroed = (p.flags & oarproto::FLAG_ZEROED) != 0;
    s.angle = p.angleDeci * 0.1f;
    s.pktCount++;
    if (!s.rateStartMs) s.rateStartMs = nowMs;
    if (nowMs - s.rateStartMs >= 2000) {
        float inst = s.pktCount * 1000.0f / (nowMs - s.rateStartMs);
        s.pps = s.pps > 0 ? 0.5f * s.pps + 0.5f * inst : inst;
        s.pktCount = 0;
        s.rateStartMs = nowMs;
    }

    if (!(p.flags & oarproto::FLAG_STROKE) || p.catchAgoMs == oarproto::AGO_NONE ||
        p.finishAgoMs == oarproto::AGO_NONE)
        return;
    if (p.strokeNo == s.lastStrokeNo) return;
    bool first = s.lastStrokeNo < 0;
    s.lastStrokeNo = p.strokeNo;
    // The first packet of a stick repeats a stroke from before we were listening.
    if (first && p.catchAgoMs > 4000) return;

    Rec& r = s.ring[s.ringHead];
    s.ringHead = (s.ringHead + 1) % RING;
    r = Rec{};
    r.used = true;
    r.no = p.strokeNo;
    r.catchUs = s.stickUs - (int64_t)p.catchAgoMs * 1000;
    r.finishUs = s.stickUs - (int64_t)p.finishAgoMs * 1000;
    r.catchDeg = p.catchDeci * 0.1f;
    r.finishDeg = p.finishDeci * 0.1f;
    r.recoveryS = p.recoveryMs * 0.001f;
    r.seenMs = nowMs;
    s.strokeTotal++;
    s.lastStrokeMs = nowMs;
    s.lastCatchUs = r.catchUs;
}

// The seat the others are compared with: the highest numbered one that is online and rowing.
int refIndex(uint32_t nowMs) {
    for (int i = MAX_SEATS - 1; i >= 0; i--) {
        const Seat& s = seats[i];
        if (online(s, nowMs) && s.strokeTotal > 0 && nowMs - s.lastStrokeMs < REF_ACTIVE_MS) return i;
    }
    return -1;
}

const Rec* nearestCatch(const Seat& ref, double catchTabS, double& distS) {
    const Rec* best = nullptr;
    distS = 1e9;
    for (const Rec& q : ref.ring) {
        if (!q.used) continue;
        double d = fabs(tabS(ref, q.catchUs) - catchTabS);
        if (d < distS) {
            distS = d;
            best = &q;
        }
    }
    return best;
}

void pushHist(Seat& s, const Hist& h) {
    s.hist[s.histHead] = h;
    s.histHead = (s.histHead + 1) % AVG;
    if (s.histN < AVG) s.histN++;
}

void logStroke(int k, const Seat& s, const Rec& r, const Hist& h) {
    char id[5];
    oarproto::idText(s.id, id);
    char offC[16] = "", offF[16] = "", rec[16] = "";
    if (h.hasOff) {
        snprintf(offC, sizeof(offC), "%.1f", h.catchOff);
        snprintf(offF, sizeof(offF), "%.1f", h.finishOff);
    }
    if (r.recoveryS > 0) snprintf(rec, sizeof(rec), "%.3f", r.recoveryS);
    char line[200];
    snprintf(line, sizeof(line), "%.3f,%d,%s,%.3f,%.3f,%s,%s,%.1f,%.1f,%.1f,%.3f,%s,%d,%d\n", board::nowS(), k + 1, id,
             tabS(s, r.catchUs), tabS(s, r.finishUs), offC, offF, r.catchDeg, r.finishDeg, r.catchDeg - r.finishDeg,
             h.drive, rec, s.rssi, s.battery);
    logger::write(logger::Stream::Oars, line);
}

void resolveStrokes(uint32_t nowMs, int ref) {
    for (int k = 0; k < MAX_SEATS; k++) {
        Seat& s = seats[k];
        if (!s.id) continue;
        for (Rec& r : s.ring) {
            if (!r.used || r.resolved) continue;
            Hist h;
            h.arc = r.catchDeg - r.finishDeg;
            h.drive = (float)((r.finishUs - r.catchUs) * 1e-6);
            h.recovery = r.recoveryS;
            bool resolve = false;
            if (k == ref) {
                h.hasOff = true;
                resolve = true;
            } else if (ref >= 0) {
                const Seat& rs = seats[ref];
                double catchT = tabS(s, r.catchUs), dist;
                const Rec* q = nearestCatch(rs, catchT, dist);
                if (q && dist < MATCH_S) {
                    h.hasOff = true;
                    h.catchOff = (float)((catchT - tabS(rs, q->catchUs)) * 1000.0);
                    h.finishOff = (float)((tabS(s, r.finishUs) - tabS(rs, q->finishUs)) * 1000.0);
                    resolve = true;
                } else {
                    // No stroke of the reference seat is close enough. If it is still behind, it may just not have
                    // reported yet; otherwise this stroke has nothing to be compared with.
                    bool refBehind = tabS(rs, rs.lastCatchUs) < catchT - MATCH_S;
                    resolve = !refBehind || nowMs - r.seenMs >= WAIT_REF_MS;
                }
            } else if (nowMs - r.seenMs >= WAIT_REF_MS) {
                resolve = true;
            }
            if (!resolve) continue;
            r.resolved = true;
            pushHist(s, h);
            s.hasLast = true;
            s.last = h;
            s.lastCatchDeg = r.catchDeg;
            s.lastFinishDeg = r.finishDeg;
            logStroke(k, s, r, h);
        }
    }
}

// How far apart the seats were at one stroke of the reference seat, once the others have had time to report.
void takeSpread(uint32_t nowMs, int ref) {
    if (ref < 0) return;
    Seat& rs = seats[ref];
    for (Rec& r : rs.ring) {
        if (!r.used || r.groupDone || nowMs - r.seenMs < GROUP_WAIT_MS) continue;
        r.groupDone = true;
        double refCatch = tabS(rs, r.catchUs);
        double minC = refCatch, maxC = refCatch;
        double minF = tabS(rs, r.finishUs), maxF = minF;
        int n = 1;
        for (int k = 0; k < MAX_SEATS; k++) {
            if (k == ref || !online(seats[k], nowMs)) continue;
            double dist;
            const Rec* q = nearestCatch(seats[k], refCatch, dist);
            if (!q || dist >= MATCH_S) continue;
            double c = tabS(seats[k], q->catchUs), f = tabS(seats[k], q->finishUs);
            minC = fmin(minC, c);
            maxC = fmax(maxC, c);
            minF = fmin(minF, f);
            maxF = fmax(maxF, f);
            n++;
        }
        if (n < 2) continue;
        spreadC[spreadHead] = (float)((maxC - minC) * 1000.0);
        spreadF[spreadHead] = (float)((maxF - minF) * 1000.0);
        spreadHead = (spreadHead + 1) % AVG;
        if (spreadN < AVG) spreadN++;
    }
}

#if SIM_MODE
// Four sticks with fixed offsets from the stroke seat (the last one), so the screen can be tested at a desk.
// Each has its own clock, started at a different value, so the clock matching is exercised too.
constexpr int SIM_SEATS = 4;
constexpr float SIM_OFFSET_MS[SIM_SEATS] = {35.0f, -20.0f, 60.0f, 0.0f};
constexpr float SIM_DRIVE_FRAC = 0.38f;

struct SimSeat {
    float lastPhase = -1;
    bool haveCatch = false, announced = true;
    double catchT = 0, finishT = 0, prevFinishT = -1, recovery = 0;
    uint8_t strokeNo = 0;
};
SimSeat simSeats[SIM_SEATS];
uint32_t simLastMs = 0;

float simJitterS() { return ((rand() / (float)RAND_MAX) * 2.0f - 1.0f) * 0.008f; }

void simTick(uint32_t nowMs) {
    if (nowMs - simLastMs < 50) return;
    simLastMs = nowMs;
    double t = board::nowS();
    for (int k = 0; k < SIM_SEATS; k++) {
        if (!seats[k].id) continue;
        SimSeat& q = simSeats[k];
        double off = SIM_OFFSET_MS[k] * 1e-3;
        double ts = t - off;
        float ph = sim::strokePhase(ts);
        float period = 60.0f / sim::rateSpm(ts);
        if (q.lastPhase >= 0 && ph < q.lastPhase - 0.5f) {
            q.catchT = ts - ph * period + off + simJitterS();
            q.haveCatch = true;
            q.announced = false;
        }
        q.lastPhase = ph;
        if (q.haveCatch && !q.announced && ph >= SIM_DRIVE_FRAC && ph < 0.9f) {
            q.announced = true;
            q.strokeNo++;
            q.finishT = q.catchT + SIM_DRIVE_FRAC * period + simJitterS();
            q.recovery = q.prevFinishT > 0 ? q.catchT - q.prevFinishT : 0;
            q.prevFinishT = q.finishT;
        }
        float catchDeg = 52.0f + 2.0f * k, finishDeg = -34.0f - 1.0f * k;
        float a;
        if (ph < SIM_DRIVE_FRAC) a = finishDeg + (catchDeg - finishDeg) * 0.5f * (1.0f + cosf((float)M_PI * ph / SIM_DRIVE_FRAC));
        else a = catchDeg + (finishDeg - catchDeg) * 0.5f * (1.0f + cosf((float)M_PI * (ph - SIM_DRIVE_FRAC) / (1.0f - SIM_DRIVE_FRAC)));

        oarproto::Packet p{};
        p.magic = oarproto::MAGIC;
        p.version = oarproto::VERSION;
        p.id = seats[k].id;
        p.ms = (uint32_t)(t * 1000.0) + 50000u * (uint32_t)(k + 1);
        p.angleDeci = (int16_t)lroundf(a * 10.0f);
        p.battery = (uint8_t)(85 - 7 * k);
        p.flags = oarproto::FLAG_ZEROED;
        if (q.strokeNo > 0) {
            p.flags |= oarproto::FLAG_STROKE;
            p.strokeNo = q.strokeNo;
            p.catchAgoMs = (uint16_t)fmin(65000.0, (t - q.catchT) * 1000.0);
            p.finishAgoMs = (uint16_t)fmin(65000.0, (t - q.finishT) * 1000.0);
            p.recoveryMs = q.recovery > 0 ? (uint16_t)(q.recovery * 1000.0) : 0;
            p.catchDeci = (int16_t)lroundf(catchDeg * 10.0f);
            p.finishDeci = (int16_t)lroundf(finishDeg * 10.0f);
        }
        ingest(k, p, -55 - 3 * k, board::nowUs());
    }
}
#endif

}  // namespace

void begin() {
    mutex = xSemaphoreCreateMutex();
    for (auto& s : seats) resetSeat(s);
#if SIM_MODE
    for (int i = 0; i < SIM_SEATS; i++) seats[i].id = (uint16_t)(0x5100 + i);
    Serial.println("[oars] simulation: 4 sticks");
#else
    Preferences prefs;
    prefs.begin("oars", true);
    for (int i = 0; i < MAX_SEATS; i++) {
        char key[4];
        snprintf(key, sizeof(key), "s%d", i);
        seats[i].id = prefs.getUShort(key, 0);
        if (seats[i].id) {
            char id[5];
            oarproto::idText(seats[i].id, id);
            Serial.printf("[oars] seat %d stick %s\n", i + 1, id);
        }
    }
    prefs.end();
#endif
    countPaired();
}

void update() {
    uint32_t nowMs = millis();
    int ps = pairSeat.load();
    if (ps >= 0 && nowMs - pairStartMs > PAIR_TIMEOUT_MS) {
        pairSeat = -1;
        Serial.println("[oars] no stick pressed, pairing ended");
    }
    Lock lock;
#if SIM_MODE
    simTick(nowMs);
#endif
    int ref = refIndex(nowMs);
    resolveStrokes(nowMs, ref);
    takeSpread(nowMs, ref);
}

View view() {
    View v;
    uint32_t nowMs = millis();
    Lock lock;
    v.ref = refIndex(nowMs);
    v.pairingSeat = pairSeat.load();
    for (int k = 0; k < MAX_SEATS; k++) {
        const Seat& s = seats[k];
        SeatView& o = v.seat[k];
        if (!s.id) continue;
        o.paired = true;
        v.paired++;
        o.id = s.id;
        oarproto::idText(s.id, o.idText);
        o.online = online(s, nowMs);
        if (o.online) v.online++;
        o.ageMs = s.heard ? nowMs - s.lastRecvMs : 0xFFFFFFFF;
        o.rssi = s.rssi;
        o.battery = s.battery;
        o.zeroed = s.zeroed;
        o.angleDeg = s.angle;
        o.packetsPerS = o.online ? s.pps : 0;
        o.strokes = s.strokeTotal;
        if (s.hasLast) {
            o.hasStroke = true;
            o.catchDeg = s.lastCatchDeg;
            o.finishDeg = s.lastFinishDeg;
            o.arcDeg = s.last.arc;
            o.driveS = s.last.drive;
            o.recoveryS = s.last.recovery;
            o.hasOffset = s.last.hasOff;
            o.catchOffsetMs = s.last.catchOff;
            o.finishOffsetMs = s.last.finishOff;
        }
        if (s.histN) {
            int offN = 0;
            float c = 0, f = 0, arc = 0, drive = 0, rec = 0;
            int recN = 0;
            for (int i = 0; i < s.histN; i++) {
                const Hist& h = s.hist[i];
                arc += h.arc;
                drive += h.drive;
                if (h.recovery > 0) {
                    rec += h.recovery;
                    recN++;
                }
                if (h.hasOff) {
                    c += h.catchOff;
                    f += h.finishOff;
                    offN++;
                }
            }
            o.avgN = s.histN;
            o.avgArcDeg = arc / s.histN;
            o.avgDriveS = drive / s.histN;
            o.avgRecoveryS = recN ? rec / recN : 0;
            if (offN) {
                o.avgHasOffset = true;
                o.avgCatchOffsetMs = c / offN;
                o.avgFinishOffsetMs = f / offN;
            }
        }
    }
    if (spreadN) {
        float c = 0, f = 0;
        for (int i = 0; i < spreadN; i++) {
            c += spreadC[i];
            f += spreadF[i];
        }
        v.hasSpread = true;
        v.catchSpreadMs = c / spreadN;
        v.finishSpreadMs = f / spreadN;
    }
    return v;
}

void startPairing(int seat) {
    if (seat < 0 || seat >= MAX_SEATS) return;
    pairStartMs = millis();
    pairSeat = seat;
#if SIM_MODE
    // Nothing to press on the bench: the seat takes the next simulated stick.
    Lock lock;
    if (!seats[seat].id) {
        seats[seat].id = (uint16_t)(0x5200 + seat);
        resetSeat(seats[seat]);
        countPaired();
    }
    pairSeat = -1;
#else
    Serial.printf("[oars] press the button of the stick for seat %d\n", seat + 1);
#endif
}

void cancelPairing() { pairSeat = -1; }

void forget(int seat) {
    if (seat < 0 || seat >= MAX_SEATS) return;
    {
        Lock lock;
        seats[seat].id = 0;
        resetSeat(seats[seat]);
        countPaired();
        savePrefs();
    }
    Serial.printf("[oars] seat %d forgotten\n", seat + 1);
}

bool hasSaved() { return pairedCount.load() > 0; }
bool wantsScan() { return pairedCount.load() > 0 || pairSeat.load() >= 0; }
int pairingSeat() { return pairSeat.load(); }

size_t describe(char* out, size_t n) {
    View v = view();
    size_t w = 0;
    out[0] = 0;
    for (int k = 0; k < MAX_SEATS && w + 1 < n; k++) {
        const SeatView& s = v.seat[k];
        if (!s.paired) continue;
        w += snprintf(out + w, n - w, "%s%d:%s %s %d dBm %.1f/s", w ? ", " : "", k + 1, s.idText,
                      s.online ? "on" : "off", s.rssi, s.packetsPerS);
    }
    return w;
}

void onAdvert(const uint8_t* addr, uint8_t addrType, int rssi, const uint8_t* payload, size_t len) {
    (void)addr;
    (void)addrType;
    if (!mutex) return;
    oarproto::Packet p;
    if (!oarproto::parse(payload, len, p)) return;
    int64_t recvUs = board::nowUs();

    int ps = pairSeat.load();
    if (ps >= 0 && (p.flags & oarproto::FLAG_BUTTON) && rssi > PAIR_MIN_RSSI) {
        {
            Lock lock;
            for (int k = 0; k < MAX_SEATS; k++) {
                if (k != ps && seats[k].id == p.id) {
                    seats[k].id = 0;
                    resetSeat(seats[k]);
                }
            }
            seats[ps].id = p.id;
            resetSeat(seats[ps]);
            countPaired();
            savePrefs();
        }
        pairSeat = -1;
        char id[5];
        oarproto::idText(p.id, id);
        Serial.printf("[oars] seat %d is stick %s (rssi %d)\n", ps + 1, id, rssi);
        return;
    }

    Lock lock;
    for (int k = 0; k < MAX_SEATS; k++) {
        if (seats[k].id == p.id && p.id) {
            ingest(k, p, rssi, recvUs);
            return;
        }
    }
}

}  // namespace oars
