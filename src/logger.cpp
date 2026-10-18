#include "logger.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_rom_crc.h>
#include <ff.h>

#include <atomic>

#include "config.h"

namespace logger {

namespace {

constexpr const char* DIR = "/rowing";
constexpr int QUEUE_LEN = 256;
constexpr int MAX_LINE_LEN = 200;
constexpr uint32_t FLUSH_MS = 2000;

enum class Kind : uint8_t { Line, Start, End };

struct Msg {
    Kind kind;
    Stream stream;
    int16_t session;
    char text[MAX_LINE_LEN];
};

const char* const SUFFIX[] = {"strokes", "seconds", "laps", "imu"};
const char* const HEADER[] = {
    "elapsed_s,stroke,rate_spm,interval_s,drive_s,recovery_s,ratio,split_s,speed_mps,dps_m,"
    "distance_m,peak_decel_g,check_mg,bounce_mg,lat,lon\n",
    "elapsed_s,utc,lat,lon,speed_mps,split_s,distance_m,rate_spm,hdop,sats,battery_pct\n",
    "lap,elapsed_s,lap_time_s,lap_distance_m,avg_split_s,avg_rate_spm,strokes\n",
    "t_s,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,surge_g,threshold_g,catch,gps_mps\n",
};

QueueHandle_t queue = nullptr;
bool sdOk = false;
std::atomic<bool> open{false};
std::atomic<bool> rawOn{false};
std::atomic<int> session{-1};
std::atomic<uint32_t> drops{0};
int nextSession = 1;

File files[(int)Stream::Count];

int scanNextSession() {
    int maxSeen = 0;
    File dir = SD_MMC.open(DIR);
    if (!dir) return 1;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char* name = f.name();
        const char* base = strrchr(name, '/');
        base = base ? base + 1 : name;
        int n = 0;
        if (sscanf(base, "S%d_", &n) == 1 && n > maxSeen) maxSeen = n;
        f.close();
    }
    dir.close();
    return maxSeen + 1;
}

void openFile(int sessionNo, Stream s) {
    char path[48];
    snprintf(path, sizeof(path), "%s/S%04d_%s.csv", DIR, sessionNo, SUFFIX[(int)s]);
    files[(int)s] = SD_MMC.open(path, FILE_WRITE, true);
    if (files[(int)s]) files[(int)s].print(HEADER[(int)s]);
}

void closeAll() {
    for (auto& f : files) {
        if (f) {
            f.flush();
            f.close();
        }
    }
}

void task(void*) {
    Msg msg;
    uint32_t lastFlush = millis();
    int current = -1;
    for (;;) {
        if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (msg.kind) {
                case Kind::Start:
                    closeAll();
                    current = msg.session;
                    for (int i = 0; i < (int)Stream::Raw; i++) openFile(current, (Stream)i);
                    break;
                case Kind::End:
                    closeAll();
                    current = -1;
                    break;
                case Kind::Line: {
                    if (current < 0) break;
                    File& f = files[(int)msg.stream];
                    if (!f && msg.stream == Stream::Raw) openFile(current, Stream::Raw);
                    if (f) f.print(msg.text);
                    break;
                }
            }
        }
        if (millis() - lastFlush > FLUSH_MS) {
            for (auto& f : files)
                if (f) f.flush();
            lastFlush = millis();
        }
    }
}

bool enqueue(const Msg& m, TickType_t wait) {
    if (!queue) return false;
    if (xQueueSend(queue, &m, wait) != pdTRUE) {
        drops++;
        return false;
    }
    return true;
}

}  // namespace

bool begin() {
    // The esp32p4 dev-board variant drives LDO channel 4 for the slot; the Tab5 powers it itself.
    SD_MMC.setPowerChannel(-1);
    SD_MMC.setPins(cfg::SD_CLK_PIN, cfg::SD_CMD_PIN, cfg::SD_D0_PIN, cfg::SD_D1_PIN, cfg::SD_D2_PIN,
                   cfg::SD_D3_PIN);
    sdOk = SD_MMC.begin("/sdcard", false, false, cfg::SD_FREQ_KHZ);
    if (sdOk) {
        if (!SD_MMC.exists(DIR)) SD_MMC.mkdir(DIR);
        nextSession = scanNextSession();
        Serial.printf("[log] SD ok, %llu MB, %llu MB used, next session %d\n",
                      SD_MMC.cardSize() / (1024ULL * 1024ULL), SD_MMC.usedBytes() / (1024ULL * 1024ULL),
                      nextSession);
    } else {
        Serial.println("[log] no SD card");
    }
    queue = xQueueCreate(QUEUE_LEN, sizeof(Msg));
    xTaskCreatePinnedToCore(task, "logger", 6144, nullptr, 2, nullptr, 0);
    return sdOk;
}

bool sdReady() { return sdOk; }

int startSession() {
    if (open) endSession();
    if (!sdOk) return -1;
    Msg m{};
    m.kind = Kind::Start;
    m.session = (int16_t)nextSession;
    enqueue(m, pdMS_TO_TICKS(100));
    session = nextSession++;
    open = true;
    return session;
}

void endSession() {
    if (!open) return;
    open = false;
    Msg m{};
    m.kind = Kind::End;
    enqueue(m, pdMS_TO_TICKS(100));
}

bool sessionOpen() { return open; }
int sessionNumber() { return session; }

void setRawEnabled(bool on) { rawOn = on; }
bool rawActive() { return open && rawOn; }

void write(Stream s, const char* line) {
    if (!open) return;
    Msg m;
    m.kind = Kind::Line;
    m.stream = s;
    m.session = 0;
    strlcpy(m.text, line, sizeof(m.text));
    enqueue(m, 0);
}

uint32_t dropped() { return drops; }

bool formatCard(Print& out) {
    if (!sdOk || open) {
        out.println("[format] needs a mounted card and no open session");
        return false;
    }
    // SD_MMC registers the card as FatFs drive 0; make sure that is really this volume.
    FATFS* fs = nullptr;
    DWORD freeClusters = 0;
    if (f_getfree("0:", &freeClusters, &fs) != FR_OK || !fs ||
        (uint64_t)(fs->n_fatent - 2) * fs->csize * 512 != SD_MMC.totalBytes()) {
        out.println("[format] drive 0: is not the SD card, aborting");
        return false;
    }
    constexpr UINT WORK_LEN = 32768;
    void* work = malloc(WORK_LEN);
    if (!work) return false;
    // No partition number in the path, so f_mkfs writes a new MBR with a single partition.
    const MKFS_PARM opt = {FM_FAT32, 0, 0, 0, 32768};
    out.printf("[format] formatting %llu MB card as FAT32...\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
    FRESULT r = f_mkfs("0:", &opt, work, WORK_LEN);
    free(work);
    out.printf("[format] %s (FatFs result %d)\n", r == FR_OK ? "done" : "failed", (int)r);
    return r == FR_OK;
}

void listDir(Print& out, const char* path, int headLines) {
    File dir = SD_MMC.open(path);
    if (!dir) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        out.printf("[files] %-24s %10lu bytes\n", f.name(), (unsigned long)f.size());
        for (int i = 0; i < headLines && f.available(); i++) {
            String line = f.readStringUntil('\n');
            out.printf("          %s\n", line.c_str());
        }
        f.close();
    }
    dir.close();
}

void listFiles(Print& out, int headLines) {
    if (!sdOk) {
        out.println("[files] no SD card");
        return;
    }
    out.printf("[files] %llu of %llu MB used\n", SD_MMC.usedBytes() / (1024ULL * 1024ULL),
               SD_MMC.totalBytes() / (1024ULL * 1024ULL));
    listDir(out, DIR, headLines);
    if (SD_MMC.exists("/map")) listDir(out, "/map", 0);
}

// Creates every parent directory of `path` (which starts with '/').
void makeParentDirs(const char* path) {
    char buf[96];
    strlcpy(buf, path, sizeof(buf));
    for (char* p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!SD_MMC.exists(buf)) SD_MMC.mkdir(buf);
        *p = '/';
    }
}

bool receiveFile(::Stream& in, const char* path, uint32_t size, uint32_t expectCrc, Print& out) {
    // Transfer is chunked: the sender waits for an ACK byte after each chunk so no
    // more than CHUNK bytes are ever in flight. This keeps the USB CDC receive
    // buffer from overflowing on big files. CHUNK must stay under that buffer.
    constexpr size_t CHUNK = 240;
    constexpr uint8_t ACK = 0x06;
    if (!sdOk) {
        out.println("[upload] ERR no SD card");
        return false;
    }
    if (open) {
        out.println("[upload] ERR session open");
        return false;
    }
    makeParentDirs(path);
    File f = SD_MMC.open(path, FILE_WRITE, true);
    if (!f) {
        out.println("[upload] ERR cannot create file");
        return false;
    }
    in.setTimeout(2000);
    out.println("[upload] READY");

    static uint8_t chunk[CHUNK];
    uint32_t received = 0, crc = 0;
    bool ok = true;
    while (received < size) {
        size_t want = min((size_t)(size - received), CHUNK);
        size_t got = 0;
        uint32_t lastMs = millis();
        while (got < want) {
            size_t n = in.readBytes(chunk + got, want - got);
            got += n;
            if (n == 0 && millis() - lastMs > 3000) break;  // sender stalled
            if (n) lastMs = millis();
        }
        if (got != want) {
            ok = false;
            break;
        }
        if (f.write(chunk, got) != got) {
            out.println("[upload] ERR write failed (card full?)");
            f.close();
            return false;
        }
        crc = esp_rom_crc32_le(crc, chunk, got);
        received += got;
        in.write(ACK);
    }
    f.close();
    if (!ok || received != size) {
        out.printf("[upload] ERR got %lu of %lu bytes\n", (unsigned long)received, (unsigned long)size);
        return false;
    }
    if (crc != expectCrc) {
        out.printf("[upload] ERR crc %08lx expected %08lx\n", (unsigned long)crc, (unsigned long)expectCrc);
        return false;
    }
    out.printf("[upload] OK %s %lu bytes\n", path, (unsigned long)size);
    return true;
}

}  // namespace logger
