#include "hr.h"

#include <Arduino.h>
#include <Preferences.h>

#include <atomic>

#if SIM_MODE
#include "board.h"
#include "sim.h"
#else
#include <BLEDevice.h>
#endif

namespace hr {
namespace {

constexpr uint32_t STALE_MS = 5000;

std::atomic<State> st{State::Off};
std::atomic<int> bpm{0};
std::atomic<int> rrMs{0};
std::atomic<uint32_t> lastMs{0};
std::atomic<bool> saved{false};
std::atomic<bool> pairingNow{false};

portMUX_TYPE nameMux = portMUX_INITIALIZER_UNLOCKED;
char savedName[24] = "";

void setName(const char* n) {
    portENTER_CRITICAL(&nameMux);
    strlcpy(savedName, n, sizeof(savedName));
    portEXIT_CRITICAL(&nameMux);
}

#if !SIM_MODE

constexpr uint32_t CONNECT_TIMEOUT_MS = 6000;
constexpr uint32_t PAIR_SCAN_S = 6;
constexpr uint32_t RESCAN_S = 4;
constexpr int FAILS_BEFORE_RESCAN = 3;
constexpr uint32_t RETRY_MIN_MS = 2000, RETRY_MAX_MS = 30000;

enum class Cmd : uint8_t { None, Pair, Forget };
std::atomic<Cmd> cmd{Cmd::None};

// Only touched by the task.
String savedAddr;
uint8_t savedType = 0;
BLEClient* client = nullptr;
std::atomic<bool> linkUp{false};

BLEUUID hrsUuid() { return BLEUUID((uint16_t)0x180D); }
BLEUUID measurementUuid() { return BLEUUID((uint16_t)0x2A37); }

class ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient*) override {}
    void onDisconnect(BLEClient*) override { linkUp = false; }
};
ClientCallbacks clientCallbacks;

void loadSaved() {
    Preferences prefs;
    prefs.begin("hr", true);
    savedAddr = prefs.getString("addr", "");
    savedType = prefs.getUChar("type", 0);
    String name = prefs.getString("name", "");
    prefs.end();
    setName(name.c_str());
    saved = savedAddr.length() > 0;
}

void store(const String& addr, uint8_t type, const String& name) {
    Preferences prefs;
    prefs.begin("hr", false);
    prefs.putString("addr", addr);
    prefs.putUChar("type", type);
    prefs.putString("name", name);
    prefs.end();
    savedAddr = addr;
    savedType = type;
    setName(name.c_str());
    saved = addr.length() > 0;
}

// Heart Rate Measurement (0x2A37): flags, 8- or 16-bit bpm, optional energy expended, then RR intervals in 1/1024 s.
void onMeasurement(BLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
    if (n < 2) return;
    uint8_t flags = d[0];
    size_t i;
    int v;
    if (flags & 0x01) {
        if (n < 3) return;
        v = d[1] | (d[2] << 8);
        i = 3;
    } else {
        v = d[1];
        i = 2;
    }
    if (flags & 0x08) i += 2;
    if (flags & 0x10) {
        for (; i + 1 < n; i += 2) rrMs = (int)(((d[i] | (d[i + 1] << 8)) * 1000 + 512) / 1024);
    }
    bpm = v;
    lastMs = millis();
}

bool isHrSensor(BLEAdvertisedDevice& d) {
    // Appearance category 13 (0x0340..0x037F) is "Heart Rate Sensor".
    return d.isAdvertisingService(hrsUuid()) || (d.haveAppearance() && (d.getAppearance() >> 6) == 13);
}

// Pairing: the strongest sensor in range wins. Rescan (byName): find the saved sensor again after
// it changed its address.
bool scanFor(uint32_t seconds, bool byName) {
    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    BLEScanResults* r = scan->start(seconds, false);
    String want = savedName;
    String addr, name;
    uint8_t type = 0;
    int bestRssi = -1000;
    for (int i = 0; r && i < r->getCount(); i++) {
        BLEAdvertisedDevice d = r->getDevice(i);
        if (!isHrSensor(d)) continue;
        String n = d.getName();
        if (!byName) {
            Serial.printf("[hr] found %s '%s' rssi %d\n", d.getAddress().toString().c_str(), n.c_str(), d.getRSSI());
        } else if (n != want) {
            continue;
        }
        if (d.getRSSI() > bestRssi) {
            bestRssi = d.getRSSI();
            addr = d.getAddress().toString();
            type = d.getAddressType();
            name = n.length() ? n : addr;
        }
    }
    scan->clearResults();
    if (addr.isEmpty()) return false;
    if (byName && addr == savedAddr) return false;
    store(addr, type, name);
    Serial.printf("[hr] %s %s '%s' rssi %d\n", byName ? "found again at" : "paired", addr.c_str(), name.c_str(),
                  bestRssi);
    return true;
}

// Many sensors (Garmin watches among them) advertise without a name; the Device Name
// characteristic (Generic Access 0x1800 / 0x2A00) still has it once connected.
void readDeviceName() {
    if (String(savedName) != savedAddr) return;
    String n = client->getValue(BLEUUID((uint16_t)0x1800), BLEUUID((uint16_t)0x2A00));
    n.trim();
    if (n.isEmpty()) return;
    store(savedAddr, savedType, n.substring(0, sizeof(savedName) - 1));
    Serial.printf("[hr] sensor name '%s'\n", n.c_str());
}

bool connectSaved() {
    st = State::Connecting;
    if (!client) {
        client = BLEDevice::createClient();
        client->setClientCallbacks(&clientCallbacks);
    }
    if (!client->connect(BLEAddress(savedAddr, savedType), savedType, CONNECT_TIMEOUT_MS)) return false;
    BLERemoteService* svc = client->getService(hrsUuid());
    BLERemoteCharacteristic* ch = svc ? svc->getCharacteristic(measurementUuid()) : nullptr;
    bpm = 0;
    rrMs = 0;
    lastMs = millis();
    if (!ch || !ch->canNotify() || !ch->subscribe(true, onMeasurement)) {
        client->disconnect();
        return false;
    }
    linkUp = true;
    st = State::Connected;
    readDeviceName();
    return true;
}

void dropLink() {
    if (client && client->isConnected()) client->disconnect();
    linkUp = false;
    bpm = 0;
}

void task(void*) {
    bool bleUp = false, bleFailed = false;
    uint32_t nextTry = 0, retryMs = RETRY_MIN_MS;
    int fails = 0;
    for (;;) {
        Cmd c = cmd.exchange(Cmd::None);
        if (c != Cmd::None) {
            dropLink();
            if (c == Cmd::Forget) {
                store("", 0, "");
                st = State::Off;
                Serial.println("[hr] sensor forgotten");
            }
            if (c == Cmd::Pair) bleFailed = false;
            nextTry = millis();
            retryMs = RETRY_MIN_MS;
            fails = 0;
        }

        if (!bleUp && !bleFailed && (c == Cmd::Pair || saved)) {
            bleUp = BLEDevice::init("TabRowing");
            bleFailed = !bleUp;
            if (bleFailed) {
                Serial.println("[hr] ERR Bluetooth did not start");
                st = State::Off;
                pairingNow = false;
            }
        }

        if (c == Cmd::Pair && bleUp) {
            st = State::Scanning;
            if (!scanFor(PAIR_SCAN_S, false)) {
                Serial.println("[hr] no heart rate sensor found");
                st = saved ? State::Lost : State::Off;
            }
            pairingNow = false;
        }

        if (saved && bleUp) {
            bool up = linkUp && client && client->isConnected();
            if (up && millis() - lastMs > STALE_MS) {
                Serial.println("[hr] no readings, reconnecting");
                dropLink();
                up = false;
            }
            if (!up) {
                if (st == State::Connected) {
                    Serial.println("[hr] connection lost");
                    st = State::Lost;
                    bpm = 0;
                    nextTry = millis();
                }
                if ((int32_t)(millis() - nextTry) >= 0) {
                    if (fails >= FAILS_BEFORE_RESCAN && savedName[0]) {
                        st = State::Scanning;
                        scanFor(RESCAN_S, true);
                    }
                    if (connectSaved()) {
                        char n[24];
                        portENTER_CRITICAL(&nameMux);
                        strlcpy(n, savedName, sizeof(n));
                        portEXIT_CRITICAL(&nameMux);
                        Serial.printf("[hr] connected to '%s'\n", n);
                        fails = 0;
                        retryMs = RETRY_MIN_MS;
                    } else {
                        fails++;
                        st = State::Lost;
                        nextTry = millis() + retryMs;
                        retryMs = min(retryMs * 2, RETRY_MAX_MS);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

#endif  // !SIM_MODE

}  // namespace

void begin() {
#if SIM_MODE
    setName("SIM HR");
    saved = true;
    st = State::Connected;
#else
    loadSaved();
    if (saved) {
        char n[24];
        strlcpy(n, savedName, sizeof(n));
        Serial.printf("[hr] saved sensor '%s', connecting\n", n);
    }
    xTaskCreate(task, "hr", 6144, nullptr, 1, nullptr);
#endif
}

void startPairing() {
#if !SIM_MODE
    pairingNow = true;
    cmd = Cmd::Pair;
#endif
}

void forget() {
#if !SIM_MODE
    saved = false;
    setName("");
    cmd = Cmd::Forget;
#endif
}

bool hasSaved() { return saved; }

bool pairing() { return pairingNow; }

Snapshot get() {
    Snapshot s;
#if SIM_MODE
    // Follows the simulated rate with a 20 s lag: about 110 bpm at 20 spm, 170 bpm at 32 spm.
    float rate = sim::rateSpm(board::nowS() - 20.0);
    bpm = (int)lroundf(110.0f + (rate - 20.0f) * 5.0f);
    rrMs = 60000 / bpm;
    lastMs = millis();
#endif
    s.state = st;
    s.ageMs = millis() - lastMs;
    s.bpm = s.state == State::Connected && s.ageMs < STALE_MS ? bpm.load() : 0;
    s.rrMs = s.bpm ? rrMs.load() : 0;
    portENTER_CRITICAL(&nameMux);
    strlcpy(s.name, savedName, sizeof(s.name));
    portEXIT_CRITICAL(&nameMux);
    return s;
}

const char* stateName(State s) {
    switch (s) {
        case State::Off: return "off";
        case State::Scanning: return "scanning";
        case State::Connecting: return "connecting";
        case State::Connected: return "connected";
        case State::Lost: return "lost";
    }
    return "?";
}

}  // namespace hr
