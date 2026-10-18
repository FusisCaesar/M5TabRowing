#include "tiles.h"

#include <HTTPClient.h>
#include <M5Unified.h>
#include <NetworkClientSecure.h>

#include <atomic>

#include "net.h"

namespace tiles {
namespace {

constexpr int SLOTS = 32;  // 4 MB of PSRAM; a screen needs up to 28
constexpr int QUEUE = 32;
constexpr int FAILS = 16;
constexpr uint32_t RETRY_MS = 15000;
constexpr size_t TILE_BYTES = SIZE * SIZE * 2;
constexpr const char* USER_AGENT = "M5TabRowing/1.0 (rowing monitor; area picker)";

struct Key {
    int z = -1, x = 0, y = 0;
    bool operator==(const Key& o) const { return z == o.z && x == o.x && y == o.y; }
};

struct Slot {
    Key k;
    uint32_t used = 0;
    uint16_t* px = nullptr;
};

struct Fail {
    Key k;
    uint32_t at = 0;
};

Slot slots[SLOTS];
Fail fails[FAILS];
int failNext = 0;
Key queue[QUEUE];
int queued = 0;
Key inflight;
uint32_t useCounter = 0;
SemaphoreHandle_t mtx = nullptr;
TaskHandle_t task = nullptr;
std::atomic<uint32_t> ver{0};

Slot* find(const Key& k) {
    for (Slot& s : slots)
        if (s.px && s.k == k) return &s;
    return nullptr;
}

bool recentlyFailed(const Key& k) {
    for (const Fail& f : fails)
        if (f.k == k && millis() - f.at < RETRY_MS) return true;
    return false;
}

// Downloads and decodes one tile into `dec`. False on any error.
bool fetch(HTTPClient& http, NetworkClientSecure& client, M5Canvas& dec, const Key& k) {
    char url[80];
    snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", k.z, k.x, k.y);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        String body = http.getString();
        ok = body.length() > 0 && dec.drawPng((const uint8_t*)body.c_str(), body.length(), 0, 0);
    } else {
        Serial.printf("[tiles] %s -> %d\n", url, code);
    }
    http.end();
    return ok;
}

void run(void*) {
    M5Canvas dec;
    dec.setColorDepth(16);
    dec.setPsram(true);
    dec.createSprite(SIZE, SIZE);
    NetworkClientSecure client;
    client.setInsecure();  // public map imagery; nothing secret is sent
    HTTPClient http;
    http.setReuse(true);
    http.setUserAgent(USER_AGENT);
    http.setConnectTimeout(8000);
    http.setTimeout(10000);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        while (net::connected()) {
            Key k;
            xSemaphoreTake(mtx, portMAX_DELAY);
            bool have = queued > 0;
            if (have) k = queue[--queued];  // newest request first
            inflight = have ? k : Key();
            xSemaphoreGive(mtx);
            if (!have) break;

            dec.fillSprite(0xC618);
            if (!fetch(http, client, dec, k)) {
                xSemaphoreTake(mtx, portMAX_DELAY);
                fails[failNext] = {k, millis()};
                failNext = (failNext + 1) % FAILS;
                inflight = Key();
                xSemaphoreGive(mtx);
                continue;
            }

            xSemaphoreTake(mtx, portMAX_DELAY);
            inflight = Key();
            Slot* s = find(k);
            if (!s) {
                s = &slots[0];
                for (Slot& c : slots) {
                    if (!c.px) {
                        s = &c;
                        break;
                    }
                    if (c.used < s->used) s = &c;
                }
                if (!s->px) s->px = (uint16_t*)heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM);
            }
            if (s->px) {
                memcpy(s->px, dec.getBuffer(), TILE_BYTES);
                s->k = k;
                s->used = ++useCounter;
            }
            xSemaphoreGive(mtx);
            ver++;
        }
    }
}

}  // namespace

void begin() {
    mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(run, "tiles", 12288, nullptr, 1, &task, 0);
}

void beginFrame() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    queued = 0;
    xSemaphoreGive(mtx);
}

void lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mtx); }

const uint16_t* get(int z, int x, int y) {
    int n = 1 << z;
    if (y < 0 || y >= n) return nullptr;
    x = ((x % n) + n) % n;
    Key k{z, x, y};
    if (Slot* s = find(k)) {
        s->used = ++useCounter;
        return s->px;
    }
    bool already = inflight == k;
    for (int i = 0; i < queued; i++) already |= queue[i] == k;
    if (!already && queued < QUEUE && !recentlyFailed(k)) {
        queue[queued++] = k;
        xTaskNotifyGive(task);
    }
    return nullptr;
}

uint32_t version() { return ver; }

void clear() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    queued = 0;
    for (Slot& s : slots) {
        free(s.px);
        s = Slot();
    }
    xSemaphoreGive(mtx);
}

}  // namespace tiles
