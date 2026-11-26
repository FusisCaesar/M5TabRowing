#include "remote.h"

#include <Arduino.h>
#include <Preferences.h>

#include <atomic>

namespace remote {
namespace {

constexpr uint16_t BTHOME_UUID = 0xFCD2;
constexpr uint8_t OBJ_PID = 0x00, OBJ_BUTTON = 0x3A;
constexpr int QUEUE = 8;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t savedAddr[6] = {0};
uint8_t savedType = 0;
char savedName[24] = "";
std::atomic<bool> saved{false};
std::atomic<bool> pairingNow{false};
std::atomic<bool> pairReq{false};
int lastPid = -1;  // only touched by the Bluetooth callback
uint32_t lastHoldMs = 0;
constexpr uint32_t HOLD_REPEAT_MS = 2500;  // the button repeats a hold about once a second

// Pairing candidate: the strongest button that was heard being pressed.
bool pairActive = false;
bool candFound = false;
bool sawEncrypted = false;
uint8_t candAddr[6] = {0};
uint8_t candType = 0;
int candRssi = -1000;

std::atomic<uint8_t> qHead{0}, qTail{0};
Event queue[QUEUE];

void push(Event e) {
    uint8_t h = qHead.load();
    uint8_t next = (h + 1) % QUEUE;
    if (next == qTail.load()) return;  // full: drop
    queue[h] = e;
    qHead = next;
}

void store(const uint8_t* addr, uint8_t type, const char* name) {
    Preferences prefs;
    prefs.begin("remote", false);
    prefs.putBytes("addr", addr, 6);
    prefs.putUChar("type", type);
    prefs.putString("name", name);
    prefs.end();
    portENTER_CRITICAL(&mux);
    memcpy(savedAddr, addr, 6);
    savedType = type;
    strlcpy(savedName, name, sizeof(savedName));
    portEXIT_CRITICAL(&mux);
    lastPid = -1;
    saved = true;
}

void clearSaved() {
    Preferences prefs;
    prefs.begin("remote", false);
    prefs.clear();
    prefs.end();
    portENTER_CRITICAL(&mux);
    memset(savedAddr, 0, sizeof(savedAddr));
    savedName[0] = 0;
    portEXIT_CRITICAL(&mux);
    lastPid = -1;
    saved = false;
}

Event eventFromCode(int code) {
    switch (code) {
        case 1: return Event::Single;
        case 2: return Event::Double;
        case 3: return Event::Triple;
        case 4:
        case 0x80: return Event::Long;  // this button reports a long press as "hold press" (0x80)
        case 5: return Event::DoubleLong;
        case 6: return Event::TripleLong;
    }
    return Event::None;
}

// Payload bytes of the BTHome object with this id, -1 when the id is not known (parsing stops there).
int objectSize(uint8_t id) {
    if (id <= 0x01 || id == 0x09 || id == 0x0F || (id >= 0x10 && id <= 0x2D)) return 1;
    if (id == 0x2E || id == 0x2F || id == 0x3A || id == 0x46) return 1;
    switch (id) {
        case 0x02: case 0x03: case 0x06: case 0x07: case 0x08: case 0x0C: case 0x0D: case 0x0E:
        case 0x3C: case 0x3D: case 0x3F: case 0x40: case 0x41: case 0x43: case 0x44: case 0x45:
        case 0x47: case 0x48: case 0x49: case 0x4A: case 0x51: case 0x52:
            return 2;
        case 0x04: case 0x05: case 0x0A: case 0x0B: case 0x42: case 0x4B:
            return 3;
        case 0x3E: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50:
            return 4;
    }
    return -1;
}

struct Packet {
    bool bthome = false;
    bool encrypted = false;
    int pid = -1;
    int button = -1;  // -1 = the packet has no button object
    char name[24] = "";
};

Packet parse(const uint8_t* p, size_t len) {
    Packet r;
    for (size_t i = 0; i + 1 < len;) {
        size_t l = p[i];
        if (l == 0 || i + 1 + l > len) break;
        uint8_t type = p[i + 1];
        const uint8_t* d = p + i + 2;
        size_t n = l - 1;
        if ((type == 0x08 || type == 0x09) && n > 0) {
            size_t m = n < sizeof(r.name) - 1 ? n : sizeof(r.name) - 1;
            memcpy(r.name, d, m);
            r.name[m] = 0;
        } else if (type == 0x16 && n >= 3 && (d[0] | (d[1] << 8)) == BTHOME_UUID) {
            r.bthome = true;
            if (d[2] & 0x01) {
                r.encrypted = true;
            } else {
                for (size_t k = 3; k < n;) {
                    uint8_t id = d[k++];
                    int sz = objectSize(id);
                    if (sz < 0 || k + sz > n) break;
                    if (id == OBJ_PID) r.pid = d[k];
                    else if (id == OBJ_BUTTON) r.button = d[k];
                    k += sz;
                }
            }
        }
        i += 1 + l;
    }
    return r;
}
}  // namespace

void begin() {
    Preferences prefs;
    prefs.begin("remote", true);
    uint8_t addr[6] = {0};
    bool have = prefs.getBytes("addr", addr, 6) == 6;
    String n = prefs.getString("name", "");
    uint8_t type = prefs.getUChar("type", 0);
    prefs.end();
    if (!have) return;
    portENTER_CRITICAL(&mux);
    memcpy(savedAddr, addr, 6);
    savedType = type;
    strlcpy(savedName, n.c_str(), sizeof(savedName));
    portEXIT_CRITICAL(&mux);
    saved = true;
    Serial.printf("[remote] saved button '%s'\n", n.c_str());
}

void startPairing() {
    pairingNow = true;
#if SIM_MODE
    const uint8_t addr[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    store(addr, 0, "SIM button");
    pairingNow = false;
#else
    pairReq = true;
#endif
}

void forget() { clearSaved(); }

bool hasSaved() { return saved; }
bool pairing() { return pairingNow; }

const char* name(char* out, size_t n) {
    portENTER_CRITICAL(&mux);
    strlcpy(out, savedName, n);
    portEXIT_CRITICAL(&mux);
    return out;
}

bool popEvent(Event& e) {
    uint8_t t = qTail.load();
    if (t == qHead.load()) return false;
    e = queue[t];
    qTail = (t + 1) % QUEUE;
    return true;
}

const char* eventName(Event e) {
    switch (e) {
        case Event::Single: return "single";
        case Event::Double: return "double";
        case Event::Triple: return "triple";
        case Event::Long: return "long";
        case Event::DoubleLong: return "double long";
        case Event::TripleLong: return "triple long";
        case Event::None: break;
    }
    return "none";
}

void inject(Event e) {
    if (e != Event::None) push(e);
}

void onAdvert(const uint8_t* addr, uint8_t addrType, int rssi, const uint8_t* payload, size_t len) {
    Packet pk = parse(payload, len);
    if (!pk.bthome) return;

    if (pairActive) {
        if (pk.encrypted) {
            if (!sawEncrypted) Serial.println("[remote] a button sends encrypted data: turn encryption off in the Shelly app");
            sawEncrypted = true;
        } else if (pk.button > 0 && rssi > candRssi) {
            memcpy(candAddr, addr, 6);
            candType = addrType;
            candRssi = rssi;
            candFound = true;
        }
        return;
    }

    if (!saved) return;
    bool mine;
    portENTER_CRITICAL(&mux);
    mine = memcmp(savedAddr, addr, 6) == 0;
    portEXIT_CRITICAL(&mux);
    if (!mine) return;
    if (pk.encrypted || pk.button < 0) return;
    if (pk.pid >= 0) {
        if (pk.pid == lastPid) return;  // the button repeats each packet a few times
        lastPid = pk.pid;
    }
    if (pk.button == 0) return;
    if (pk.button == 0x80 || pk.button == 4) {
        // Holding the button sends a hold event (0x80) when the hold starts, more of them about every
        // second while it lasts, and a long press (4) at the end. Only the first one counts; each
        // later one extends the hold.
        uint32_t now = millis();
        bool repeat = now - lastHoldMs < HOLD_REPEAT_MS;
        lastHoldMs = now;
        if (repeat) return;
    }
    Event e = eventFromCode(pk.button);
    Serial.printf("[remote] pid %d button code %d (%s) rssi %d\n", pk.pid, pk.button, eventName(e), rssi);
    if (e != Event::None) push(e);
}

bool takePairRequest() { return pairReq.exchange(false); }
void pairBegin() {
    candFound = false;
    sawEncrypted = false;
    candRssi = -1000;
    pairActive = true;
}

bool pairFound() { return candFound; }

void pairEnd() {
    pairActive = false;
    if (candFound) {
        char n[24];
        snprintf(n, sizeof(n), "Button %02X%02X", candAddr[4], candAddr[5]);
        store(candAddr, candType, n);
        Serial.printf("[remote] paired button %02x:%02x:%02x:%02x:%02x:%02x rssi %d\n", candAddr[0], candAddr[1],
                      candAddr[2], candAddr[3], candAddr[4], candAddr[5], candRssi);
    } else {
        Serial.println("[remote] no button press heard");
    }
    pairingNow = false;
}

}  // namespace remote
