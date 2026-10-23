#include "gps_nav.h"

#include <Arduino.h>
#include <TinyGPSPlus.h>

#include "board.h"
#include "config.h"
#include "map_data.h"
#include "sim.h"

namespace gps {

namespace {

HardwareSerial& port = Serial1;
TinyGPSPlus parser;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Snapshot state;

double committed = 0;
double anchorLat = 0, anchorLon = 0;
bool haveAnchor = false;
bool wasMoving = false;
float lastCourse = 0;  // held when too slow for a reliable course over ground
uint32_t hzCount = 0;
int64_t hzWindowStart = 0;

// ---- receiver configuration --------------------------------------------

void sendNmea(const char* body) {
    uint8_t cs = 0;
    for (const char* p = body; *p; p++) cs ^= (uint8_t)*p;
    char buf[96];
    snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, cs);
    port.print(buf);
}

void sendUbx(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
    uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; i++) { a += hdr[i]; b += a; }
    for (uint16_t i = 0; i < len; i++) { a += payload[i]; b += a; }
    port.write(hdr, 6);
    if (len) port.write(payload, len);
    port.write(a);
    port.write(b);
}

// u-blox M9/M10 configuration interface (UBX-CFG-VALSET, RAM layer).
void ubxValset(uint32_t key, uint32_t value, uint8_t size) {
    uint8_t p[4 + 4 + 4] = {0x00, 0x01, 0x00, 0x00};
    memcpy(p + 4, &key, 4);
    memcpy(p + 8, &value, size);
    sendUbx(0x06, 0x8A, p, 8 + size);
}

void ubxLegacyMsgRate(uint8_t msgId, uint8_t rate) {
    uint8_t p[3] = {0xF0, msgId, rate};
    sendUbx(0x06, 0x01, p, 3);
}

void sendBaudCommands(uint32_t baud) {
    ubxValset(0x40520001, baud, 4);  // CFG-UART1-BAUDRATE
    uint8_t prt[20] = {0x01, 0x00, 0x00, 0x00, 0xD0, 0x08, 0x00, 0x00};
    memcpy(prt + 8, &baud, 4);
    prt[12] = 0x07;  // in: UBX+NMEA+RTCM
    prt[14] = 0x03;  // out: UBX+NMEA
    sendUbx(0x06, 0x00, prt, sizeof(prt));  // legacy UBX-CFG-PRT (M8 and older)

    int casic = baud >= 115200 ? 5 : baud >= 57600 ? 4 : baud >= 38400 ? 3 : 1;
    char buf[32];
    snprintf(buf, sizeof(buf), "PCAS01,%d", casic);  // CASIC (AT6558, AT6668)
    sendNmea(buf);
    snprintf(buf, sizeof(buf), "PMTK251,%lu", (unsigned long)baud);  // MediaTek
    sendNmea(buf);
}

void sendRateCommands(uint16_t rateMs) {
    ubxValset(0x30210001, rateMs, 2);  // CFG-RATE-MEAS
    uint8_t rate[6] = {(uint8_t)(rateMs & 0xFF), (uint8_t)(rateMs >> 8), 0x01, 0x00, 0x01, 0x00};
    sendUbx(0x06, 0x08, rate, sizeof(rate));  // legacy UBX-CFG-RATE

    // Only RMC (speed) and GGA (HDOP, satellites) are needed.
    ubxValset(0x209100c5, 0, 1);  // GSV
    ubxValset(0x209100ca, 0, 1);  // GLL
    ubxValset(0x209100b1, 0, 1);  // VTG
    ubxValset(0x209100c0, 0, 1);  // GSA
    ubxLegacyMsgRate(0x03, 0);
    ubxLegacyMsgRate(0x01, 0);
    ubxLegacyMsgRate(0x05, 0);
    ubxLegacyMsgRate(0x02, 0);

    // Sea dynamic model: assumes zero vertical velocity, which suits a boat.
    ubxValset(0x20110021, 5, 1);  // CFG-NAVSPG-DYNMODEL
    uint8_t nav5[36] = {0x01, 0x00, 0x05};
    sendUbx(0x06, 0x24, nav5, sizeof(nav5));  // legacy UBX-CFG-NAV5

    char buf[32];
    snprintf(buf, sizeof(buf), "PCAS02,%u", rateMs);
    sendNmea(buf);
    sendNmea("PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0,,,,0");
    snprintf(buf, sizeof(buf), "PMTK220,%u", rateMs);
    sendNmea(buf);
    sendNmea("PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
}

int rxPin = cfg::GPS_RX_PIN;
int txPin = cfg::GPS_TX_PIN;

void openPort(uint32_t baud) {
    port.end();
    port.setRxBufferSize(4096);
    port.begin(baud, SERIAL_8N1, rxPin, txPin);
}

bool probe(uint32_t baud, uint32_t ms = 1500) {
    openPort(baud);
    TinyGPSPlus p;
    uint32_t start = millis();
    while (millis() - start < ms) {
        while (port.available()) p.encode(port.read());
        if (p.passedChecksum() >= 3) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

uint32_t detectAndConfigure() {
    static const uint32_t bauds[] = {115200, 9600, 38400, 57600};
    // Grove cable conventions differ between units, so try both pin orders.
    static const int pinPairs[][2] = {{cfg::GPS_RX_PIN, cfg::GPS_TX_PIN}, {cfg::GPS_TX_PIN, cfg::GPS_RX_PIN}};
    uint32_t found = 0;
    for (const auto& pins : pinPairs) {
        rxPin = pins[0];
        txPin = pins[1];
        for (uint32_t b : bauds) {
            if (probe(b)) {
                found = b;
                break;
            }
        }
        if (found) break;
    }
    if (!found) return 0;
    Serial.printf("[gps] NMEA on RX=G%d TX=G%d\n", rxPin, txPin);

    if (found != cfg::GPS_TARGET_BAUD) {
        sendBaudCommands(cfg::GPS_TARGET_BAUD);
        port.flush();
        vTaskDelay(pdMS_TO_TICKS(200));
        if (probe(cfg::GPS_TARGET_BAUD)) {
            found = cfg::GPS_TARGET_BAUD;
        } else {
            openPort(found);
        }
    }
    uint16_t rateMs = found >= 38400 ? cfg::GPS_RATE_MS_FAST : cfg::GPS_RATE_MS_SLOW;
    sendRateCommands(rateMs);
    Serial.printf("[gps] receiver at %lu baud, requested %u ms rate\n", (unsigned long)found, rateMs);
    return found;
}

// ---- fix processing -----------------------------------------------------

void publishFix(double lat, double lon, float speed, float course, float hdop, int sats, bool good,
                bool timeOk, uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    int64_t now = board::nowUs();
    if (speed < cfg::GPS_SPEED_ZERO_MPS) speed = 0;
    // Course over ground is only meaningful when moving; otherwise keep the last heading.
    if (good && speed >= cfg::GPS_DIR_MIN_SPEED_MPS) lastCourse = course;

    double shown = committed;
    if (good) {
        if (!haveAnchor) {
            anchorLat = lat;
            anchorLon = lon;
            haveAnchor = true;
        }
        double dAnchor = TinyGPSPlus::distanceBetween(anchorLat, anchorLon, lat, lon);
        bool moving = speed >= cfg::GPS_MOVING_SPEED_MPS;
        if (moving) {
            if (dAnchor >= cfg::GPS_MIN_STEP_M) {
                committed += dAnchor;
                anchorLat = lat;
                anchorLon = lon;
                dAnchor = 0;
            }
            shown = committed + dAnchor;
        } else {
            if (wasMoving && dAnchor >= 1.0) committed += dAnchor;
            anchorLat = lat;
            anchorLon = lon;
            shown = committed;
        }
        wasMoving = moving;
    }

    hzCount++;
    if (hzWindowStart == 0) hzWindowStart = now;

    portENTER_CRITICAL(&mux);
    state.fix = good;
    state.speed = good ? speed : 0;
    state.course = lastCourse;
    if (good) {
        state.lat = lat;
        state.lon = lon;
    }
    state.hdop = hdop;
    state.sats = sats;
    if (shown > state.distance) state.distance = shown;
    if (good) state.lastFixUs = now;
    state.updates++;
    if (now - hzWindowStart >= 2000000) {
        state.updateHz = hzCount * 1e6f / (float)(now - hzWindowStart);
        hzCount = 0;
        hzWindowStart = now;
    }
    state.timeValid = timeOk;
    state.year = y;
    state.month = mo;
    state.day = d;
    state.hour = h;
    state.minute = mi;
    state.second = s;
    portEXIT_CRITICAL(&mux);
}

void onSentence() {
    // RMC only commits speed once there is a fix, so a receiver that is still
    // searching would otherwise never update the satellite count.
    bool speedNew = parser.speed.isUpdated();
    bool locNew = parser.location.isUpdated();
    bool satsNew = parser.satellites.isUpdated();
    bool hdopNew = parser.hdop.isUpdated();
    if (!speedNew && !locNew && !satsNew && !hdopNew) return;
    float speed = speedNew && parser.speed.isValid() ? (float)parser.speed.mps() : 0;
    bool locOk = locNew && parser.location.isValid() && parser.location.age() < 1500;
    double lat = locOk ? parser.location.lat() : state.lat;
    double lon = locOk ? parser.location.lng() : state.lon;
    float hdop = hdopNew && parser.hdop.isValid() ? (float)parser.hdop.hdop() : state.hdop;
    int sats = satsNew && parser.satellites.isValid() ? (int)parser.satellites.value() : state.sats;
    bool good = locOk && speedNew && hdop <= cfg::GPS_MAX_HDOP && sats >= cfg::GPS_MIN_SATS;
    bool timeOk = parser.date.isValid() && parser.time.isValid();
    float course = parser.course.isValid() ? (float)parser.course.deg() : 0;
    publishFix(lat, lon, speed, course, hdop, sats, good, timeOk, parser.date.year(), parser.date.month(),
               parser.date.day(), parser.time.hour(), parser.time.minute(), parser.time.second());
}

void setConnected(bool c, uint32_t baud) {
    portENTER_CRITICAL(&mux);
    state.connected = c;
    state.baud = baud;
    if (!c) {
        state.fix = false;
        state.speed = 0;
        state.updateHz = 0;
    }
    portEXIT_CRITICAL(&mux);
}

#if SIM_MODE
void simTask(void*) {
    setConnected(true, 0);
    // Start at the loaded map's centre so the map screen shows the boat on the water.
    double lat = 52.0, lon = 4.5;
    mapdata::centerLatLon(lat, lon);
    const double headingDeg = 30.0;
    const double heading = headingDeg * M_PI / 180.0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(cfg::GPS_RATE_MS_FAST));
        double t = board::nowS();
        float v = sim::boatSpeed(t);
        double step = v * cfg::GPS_RATE_MS_FAST / 1000.0;
        lat += step * cos(heading) / 111320.0;
        lon += step * sin(heading) / (111320.0 * cos(lat * M_PI / 180.0));
        uint32_t secs = (uint32_t)t;
        publishFix(lat, lon, v, (float)headingDeg, 0.8f, 12, true, true, 2026, 10, 18,
                   (8 + secs / 3600) % 24, (secs / 60) % 60, secs % 60);
    }
}
#endif

void task(void*) {
    for (;;) {
        setConnected(false, 0);
        uint32_t baud = detectAndConfigure();
        if (!baud) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        setConnected(true, baud);
        uint32_t lastByte = millis();
        for (;;) {
            bool any = false;
            while (port.available()) {
                any = true;
                if (parser.encode(port.read())) onSentence();
            }
            if (any) {
                lastByte = millis();
                static uint32_t lastReport = 0;
                if (millis() - lastReport > 10000) {
                    lastReport = millis();
                    Snapshot s = get();
                    Serial.printf("[gps] baud %lu  sentences %lu  sats %d  hdop %.1f  fix %d\n",
                                  (unsigned long)baud, (unsigned long)parser.passedChecksum(), s.sats, s.hdop,
                                  s.fix);
                }
            } else if (millis() - lastByte > cfg::GPS_SILENCE_RESCAN_MS) {
                Serial.println("[gps] silent, rescanning");
                break;
            }

            int64_t age = board::nowUs() - state.lastFixUs;
            if (state.fix && age > (int64_t)cfg::GPS_FIX_TIMEOUT_MS * 1000) {
                portENTER_CRITICAL(&mux);
                state.fix = false;
                state.speed = 0;
                portEXIT_CRITICAL(&mux);
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

}  // namespace

void begin() {
#if SIM_MODE
    xTaskCreatePinnedToCore(simTask, "gps", 4096, nullptr, 3, nullptr, 0);
#else
    xTaskCreatePinnedToCore(task, "gps", 6144, nullptr, 3, nullptr, 0);
#endif
}

Snapshot get() {
    portENTER_CRITICAL(&mux);
    Snapshot s = state;
    portEXIT_CRITICAL(&mux);
    return s;
}

}  // namespace gps
