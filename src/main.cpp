#include <M5Unified.h>

#include "board.h"
#include "camera.h"
#include "display_fb.h"
#include "gps_nav.h"
#include "hr.h"
#include "imu_stroke.h"
#include "logger.h"
#include "map_data.h"
#include "metrics.h"
#include "net.h"
#include "rtclock.h"
#include "segments.h"
#include "settings.h"
#include "targets.h"
#include "track.h"
#include "ui.h"

void setup() {
    auto c = M5.config();
    c.serial_baudrate = 115200;
    c.internal_imu = true;
    M5.begin(c);
    // With the USB cable in but no program reading the port, every print would wait for the timeout.
    Serial.setTxTimeoutMs(0);
    board::begin();

    Serial.println("\n[boot] Tab5 rowing monitor");
#if SIM_MODE
    Serial.println("[boot] SIMULATION build: synthetic strokes and GPS");
#endif

    // 5 V on the Grove port powers the GPS; its two signal pins become a UART.
    M5.Power.setExtOutput(true);
    M5.Ex_I2C.release();

    rtclock::begin();
    settings::begin();
    targets::begin();
    segments::begin();
    const Settings& s = settings::get();
    logger::begin();
    logger::setRawEnabled(s.rawLog);
    mapdata::begin();  // scan the SD card before the GPS task (the simulator centres on the map)
    track::begin();
    imu::begin(s.strokeParams(), s.fusion);
    gps::begin();
    hr::begin();
    metrics::begin();
    ui::begin();
}

static void printStatus() {
    static uint32_t last = 0;
    if (millis() - last < 5000) return;
    last = millis();
    imu::Live l = imu::live();
    gps::Snapshot g = gps::get();
    hr::Snapshot h = hr::get();
    Serial.printf("[status] imu %.1f Hz ovr %lu acc(%+.4f %+.4f %+.4f) rms %.4f thr %.3f skew %+.2f sign %+d "
                  "dir %s corr %+.2f axis(%+.2f %+.2f %+.2f) rate %.1f | gps conn %d fix %d %.1f Hz %.2f m/s "
                  "%d sats dist %.1f | hr %s %d bpm | cam %d %.1f fps | map %s trk %d | heap %lu psram %lu | "
                  "up %lus rst %d\n",
                  l.sampleHz, (unsigned long)l.overruns, l.accel[0], l.accel[1], l.accel[2], l.rms, l.threshold,
                  l.skew, l.sign, l.directionFromGps ? "gps" : "skew", l.gpsCorrelation, l.axis[0], l.axis[1],
                  l.axis[2], l.rate, g.connected, g.fix, g.updateHz,
                  g.speed, g.sats, g.distance, hr::stateName(h.state), h.bpm, (int)camera::status(), camera::fps(),
                  mapdata::loaded() ? mapdata::current().name : "-", track::count(),
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram(), millis() / 1000,
                  (int)esp_reset_reason());
}

// Binary transfers need writes to wait for buffer space; with no timeout the USB driver drops what doesn't fit.
static constexpr uint32_t BINARY_TX_TIMEOUT_MS = 200;

// 'S' sends the screen as raw RGB565 (see tools/screenshot.py), 't' opens the target list,
// 's' opens the segment list,
// 'D' lists the SD session files,
// 'U path size crc' uploads a file (see tools/send_file.py), 'W ssid password' joins Wi-Fi,
// 'T x y' / 'P x y' / 'Z x0 y0 x1 y1' simulate a tap / hold / drag, 'F' then 'Y' within 10 s formats the SD card,
// 'B path' downloads a file (see tools/fetch.py), 'h' pairs a heart rate sensor; other keys go to ui::command.
static void serialCommands() {
    static uint32_t formatArmedMs = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == 'h') {
            hr::startPairing();
            continue;
        }
        if (c == 'D') {
            logger::listFiles(Serial, 3);
            continue;
        }
        if (c == 'U') {
            // Rest of the line: " <path> <size> <crc32hex>", then size raw bytes follow.
            String line = Serial.readStringUntil('\n');
            char path[80];
            unsigned long size = 0, crc = 0;
            if (sscanf(line.c_str(), " %79s %lu %lx", path, &size, &crc) == 3) {
                bool ok = logger::receiveFile(Serial, path, size, crc, Serial);
                // Pick up a newly uploaded map without a reboot.
                if (ok && strstr(path, ".rmap")) mapdata::begin();
            } else {
                Serial.println("[upload] ERR bad header");
            }
            continue;
        }
        if (c == 'B') {
            String line = Serial.readStringUntil('\n');
            line.trim();
            Serial.setTxTimeoutMs(BINARY_TX_TIMEOUT_MS);
            logger::sendFile(Serial, line.c_str());
            Serial.setTxTimeoutMs(0);
            continue;
        }
        if (c == 'W') {
            // " <ssid> <password>" or " <ssid>|<password>"; empty reconnects with the stored network.
            String line = Serial.readStringUntil('\n');
            line.trim();
            int sep = line.indexOf('|');
            if (sep < 0) sep = line.indexOf(' ');
            String ssid = sep < 0 ? line : line.substring(0, sep);
            String pass = sep < 0 ? String() : line.substring(sep + 1);
            net::connect(ssid.c_str(), pass.c_str(), Serial);
            continue;
        }
        if (c == 'T' || c == 'P' || c == 'Z') {
            // Simulated touch: "T x y" taps, "P x y" presses and holds, "Z x0 y0 x1 y1" drags.
            String line = Serial.readStringUntil('\n');
            int v[4];
            int n = sscanf(line.c_str(), " %d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
            if (c == 'T' && n == 2) ui::tap(v[0], v[1]);
            else if (c == 'P' && n == 2) ui::hold(v[0], v[1]);
            else if (c == 'Z' && n == 4) ui::drag(v[0], v[1], v[2], v[3]);
            continue;
        }
        if (c == 'F') {
            formatArmedMs = millis();
            Serial.println("[format] erases the whole SD card; send Y within 10 s to confirm");
            continue;
        }
        if (c == 'Y') {
            bool armed = formatArmedMs && millis() - formatArmedMs < 10000;
            formatArmedMs = 0;
            if (armed && logger::formatCard(Serial)) {
                Serial.println("[format] restarting");
                Serial.flush();
                delay(200);
                ESP.restart();
            }
            continue;
        }
        if (c != 'S') {
            ui::command(c);
            continue;
        }
        int w = M5.Display.width(), h = M5.Display.height();
        static uint16_t row[1280];
        fb::syncForRead();
        Serial.setTxTimeoutMs(BINARY_TX_TIMEOUT_MS);
        Serial.printf("SCREENSHOT %d %d\n", w, h);
        for (int y = 0; y < h; y++) {
            M5.Display.readRect(0, y, w, 1, row);
            const uint8_t* p = (const uint8_t*)row;
            size_t left = w * 2;
            while (left) {
                size_t n = Serial.write(p, left);
                p += n;
                left -= n;
                if (!n) delay(1);
            }
        }
        Serial.flush();
        Serial.setTxTimeoutMs(0);
    }
}

void loop() {
    {
        board::I2CLock lock;
        M5.update();
    }
    rtclock::update();
    metrics::update();
    ui::loop();
    printStatus();
    serialCommands();
    net::loop(Serial);
    vTaskDelay(pdMS_TO_TICKS(10));
}
