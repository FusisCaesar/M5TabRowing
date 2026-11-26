// Oar sensor for the M5Stack StickS3. See ../README.md ("Oar sensors") and oar_tracker.h.
//
// Fasten the stick to the oar, anywhere and in any direction, with its screen where the rower can see
// it. Hold the front button for two seconds with the oar square to the boat to set the angle to zero.
// The stick tells the Tab5 the angle of the oar and, for every stroke, when its catch and finish were,
// by Bluetooth advertisements: nothing is paired, nothing connects.

#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_timer.h>

#include "oar_protocol.h"
#include "oar_tracker.h"

namespace {

constexpr uint32_t IMU_PERIOD_US = 10000;  // 100 Hz
constexpr uint32_t ADV_PERIOD_MS = 50;
constexpr uint32_t DRAW_PERIOD_MS = 100;
constexpr uint32_t ZERO_HOLD_MS = 2000;
constexpr uint32_t BATTERY_PERIOD_MS = 5000;

oartrack::Tracker tracker;
M5Canvas canvas(&M5.Display);

uint16_t stickId = 0;
uint8_t seq = 0;
uint8_t batteryPct = 255;
uint32_t zeroFlashMs = 0;
bool zeroDone = false;  // the long press of this hold has already been used
int page = 0;           // 0 angle, 1 big id

inline uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }
inline double nowS() { return esp_timer_get_time() * 1e-6; }

// The id comes from the factory MAC, so it is the same after every restart, and is shown on the screen.
uint16_t makeId() {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    uint32_t h = 2166136261u;  // FNV-1a
    for (int i = 0; i < 6; i++) h = (h ^ mac[i]) * 16777619u;
    uint16_t id = (uint16_t)((h >> 16) ^ h);
    return id ? id : 1;
}

uint16_t agoMs(uint32_t ms, double t) {
    uint32_t at = (uint32_t)llround(t * 1000.0);
    uint32_t d = ms - at;
    return d >= 0xFFFF ? oarproto::AGO_NONE : (uint16_t)d;
}

int16_t deci(float deg) {
    float v = deg * 10.0f;
    if (v > 32000) v = 32000;
    if (v < -32000) v = -32000;
    return (int16_t)lroundf(v);
}

NimBLEAdvertising* adv = nullptr;

void bleBegin() {
    NimBLEDevice::init("OAR");
    NimBLEDevice::setPower(9);  // dBm: the stick may be a few metres from the Tab5, with a rower in between
    adv = NimBLEDevice::getAdvertising();
    adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    adv->enableScanResponse(false);
    adv->setMinInterval(80);  // 50 ms, in units of 0.625 ms
    adv->setMaxInterval(80);
}

void advertise(const oarproto::Packet& p) {
    uint8_t buf[2 + sizeof(oarproto::Packet)];
    buf[0] = oarproto::COMPANY_ID & 0xFF;
    buf[1] = oarproto::COMPANY_ID >> 8;
    memcpy(buf + 2, &p, sizeof(p));
    NimBLEAdvertisementData data;
    data.setFlags(0x06);  // general discoverable, no classic Bluetooth
    data.setManufacturerData(buf, sizeof(buf));
    adv->setAdvertisementData(data);
    if (!adv->isAdvertising()) adv->start();
}

void buildAndAdvertise(bool buttonDown) {
    oarproto::Packet p{};
    uint32_t ms = nowMs();
    p.magic = oarproto::MAGIC;
    p.version = oarproto::VERSION;
    p.id = stickId;
    p.seq = seq++;
    p.ms = ms;
    p.angleDeci = deci(tracker.angleDeg());
    p.battery = batteryPct;
    p.flags = (buttonDown ? oarproto::FLAG_BUTTON : 0) | (tracker.zeroed() ? oarproto::FLAG_ZEROED : 0);
    if (tracker.strokeCount() > 0) {
        const oartrack::Stroke& s = tracker.last();
        p.flags |= oarproto::FLAG_STROKE;
        p.strokeNo = s.no;
        p.catchAgoMs = agoMs(ms, s.catchT);
        p.finishAgoMs = agoMs(ms, s.finishT);
        float rec = s.recoveryS * 1000.0f;
        p.recoveryMs = rec > 0 && rec < 65000 ? (uint16_t)rec : 0;
        p.catchDeci = deci(s.catchDeg);
        p.finishDeci = deci(s.finishDeg);
    }
    advertise(p);
}

void readImu() {
    static uint32_t lastUs = 0;
    uint32_t us = (uint32_t)esp_timer_get_time();
    if ((uint32_t)(us - lastUs) < IMU_PERIOD_US) return;
    lastUs = us;
    if (!M5.Imu.update()) return;
    float a[3], w[3];
    M5.Imu.getAccel(&a[0], &a[1], &a[2]);
    M5.Imu.getGyro(&w[0], &w[1], &w[2]);
    tracker.update(nowS(), a, w);
}

void draw(bool buttonDown) {
    const int W = canvas.width(), H = canvas.height();
    char idText[5];
    oarproto::idText(stickId, idText);
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);

    if (page == 1) {
        canvas.setTextDatum(middle_center);
        canvas.setTextSize(1);
        canvas.setFont(&fonts::Font4);
        canvas.drawString("OAR", W / 2, 18);
        canvas.setFont(&fonts::Font8);
        canvas.drawString(idText, W / 2, H / 2 + 12);
        canvas.pushSprite(0, 0);
        return;
    }

    // Header: id and battery.
    canvas.setFont(&fonts::Font4);
    canvas.setTextDatum(top_left);
    canvas.drawString(idText, 4, 2);
    char b[12];
    if (batteryPct <= 100) snprintf(b, sizeof(b), "%d%%", batteryPct);
    else snprintf(b, sizeof(b), "--");
    canvas.setTextDatum(top_right);
    canvas.setTextColor(batteryPct <= 15 ? TFT_RED : TFT_LIGHTGREY, TFT_BLACK);
    canvas.drawString(b, W - 4, 2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);

    // The oar angle on a scale of -90 to +90 degrees, with the last stroke's catch and finish.
    const int sx0 = 12, sx1 = W - 12, sy = 62;
    auto xOf = [&](float deg) {
        if (deg < -90) deg = -90;
        if (deg > 90) deg = 90;
        return sx0 + (int)((deg + 90.0f) / 180.0f * (sx1 - sx0));
    };
    canvas.drawFastHLine(sx0, sy, sx1 - sx0, TFT_DARKGREY);
    canvas.drawFastVLine(xOf(0), sy - 10, 21, TFT_DARKGREY);
    if (tracker.strokeCount() > 0) {
        const oartrack::Stroke& s = tracker.last();
        canvas.fillRect(xOf(s.finishDeg), sy - 3, xOf(s.catchDeg) - xOf(s.finishDeg), 7, 0x0320);  // dark green arc
        canvas.drawFastVLine(xOf(s.catchDeg), sy - 14, 29, TFT_GREEN);
        canvas.drawFastVLine(xOf(s.finishDeg), sy - 14, 29, TFT_CYAN);
    }
    canvas.fillTriangle(xOf(tracker.angleDeg()), sy - 8, xOf(tracker.angleDeg()) - 7, sy - 24, xOf(tracker.angleDeg()) + 7,
                        sy - 24, TFT_YELLOW);

    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(top_left);
    char line[40];
    if (tracker.strokeCount() > 0) {
        const oartrack::Stroke& s = tracker.last();
        float rate = tracker.rateSpm(nowS());
        snprintf(line, sizeof(line), "%.0f deg  %.0f..%.0f", s.catchDeg - s.finishDeg, s.finishDeg, s.catchDeg);
        canvas.drawString(line, 6, 84);
        if (rate > 0) snprintf(line, sizeof(line), "#%lu  %.0f spm", (unsigned long)tracker.strokeCount(), rate);
        else snprintf(line, sizeof(line), "#%lu", (unsigned long)tracker.strokeCount());
        canvas.drawString(line, 6, 102);
    } else {
        canvas.drawString(tracker.still() ? "still: rock the oar" : "no strokes yet", 6, 84);
    }
    canvas.setTextDatum(top_right);
    if (millis() - zeroFlashMs < 1500 && zeroFlashMs) {
        canvas.setTextColor(TFT_GREEN, TFT_BLACK);
        canvas.drawString("ZERO SET", W - 6, 102);
    } else if (buttonDown) {
        canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
        canvas.drawString("hold: zero", W - 6, 102);
    } else if (!tracker.zeroed()) {
        canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
        canvas.drawString("A hold: zero", W - 6, 102);
    }
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.pushSprite(0, 0);
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.internal_imu = true;
    M5.begin(cfg);
    Serial.setTxTimeoutMs(0);
    M5.Imu.setCalibration(0, 0, 0);  // the tracker finds the gyro bias itself

    M5.Display.setRotation(1);  // landscape
    M5.Display.setBrightness(80);
    canvas.setColorDepth(16);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    stickId = makeId();
    char idText[5];
    oarproto::idText(stickId, idText);
    Serial.printf("\n[oar] stick %s, imu %d\n", idText, (int)M5.Imu.getType());
    bleBegin();
}

void loop() {
    M5.update();
    readImu();

    bool down = M5.BtnA.isPressed();
    if (down && !zeroDone && M5.BtnA.pressedFor(ZERO_HOLD_MS)) {
        tracker.zero();
        zeroDone = true;
        zeroFlashMs = millis() ? millis() : 1;
        Serial.println("[oar] zero set");
    }
    if (!down) zeroDone = false;
    if (M5.BtnB.wasClicked()) page = 1 - page;

    static uint32_t lastAdv = 0, lastDraw = 0, lastBat = 0, lastLog = 0;
    uint32_t now = millis();
    if (now - lastBat >= BATTERY_PERIOD_MS || lastBat == 0) {
        lastBat = now ? now : 1;
        int lvl = M5.Power.getBatteryLevel();
        batteryPct = lvl >= 0 && lvl <= 100 ? (uint8_t)lvl : 255;
    }
    if (now - lastAdv >= ADV_PERIOD_MS) {
        lastAdv = now;
        buildAndAdvertise(down);
    }
    if (now - lastDraw >= DRAW_PERIOD_MS) {
        lastDraw = now;
        draw(down);
    }
    if (now - lastLog >= 5000) {
        lastLog = now;
        Serial.printf("[oar] angle %.1f strokes %lu rate %.1f still %d zero %d bat %u\n", tracker.angleDeg(),
                      (unsigned long)tracker.strokeCount(), tracker.rateSpm(nowS()), tracker.still(), tracker.zeroed(),
                      batteryPct);
    }
    delay(1);
}
