#include "map_job.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <esp_heap_caps.h>
#include <math.h>

#include <atomic>

#include "logger.h"
#include "map_build.h"
#include "net.h"

namespace mapjob {
namespace {

// overpass-api.de only offers TLS 1.3, which the prebuilt Arduino libraries lack; the data is public.
constexpr const char* OVERPASS_URL = "http://overpass-api.de/api/interpreter";
constexpr const char* USER_AGENT = "M5TabRowing/1.0 (on-device rowing map)";
constexpr size_t MAX_DOWNLOAD = 10u << 20;

// Collects the HTTP response body in PSRAM.
class PsramSink : public Stream {
public:
    explicit PsramSink(std::atomic<uint32_t>& progress) : progress_(progress) {}
    ~PsramSink() { free(buf); }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* d, size_t n) override {
        if (len + n + 1 > cap) {
            size_t nc = max(len + n + 1, cap ? cap + cap / 2 : (size_t)1 << 20);
            if (nc > MAX_DOWNLOAD) {
                overflow = true;
                return 0;
            }
            void* nb = heap_caps_realloc(buf, nc, MALLOC_CAP_SPIRAM);
            if (!nb) {
                overflow = true;
                return 0;
            }
            buf = (char*)nb;
            cap = nc;
        }
        memcpy(buf + len, d, n);
        len += n;
        buf[len] = 0;
        progress_ = len;
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    char* buf = nullptr;
    size_t len = 0, cap = 0;
    bool overflow = false;

private:
    std::atomic<uint32_t>& progress_;
};

struct Job {
    char name[32];
    char path[48];
    double s, w, n, e;
};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Status st;
std::atomic<uint32_t> bytes{0};
std::atomic<bool> running{false};
std::atomic<bool> completed{false};
uint32_t startMs = 0;
Job job;

void setPhase(Phase p) {
    portENTER_CRITICAL(&mux);
    st.phase = p;
    portEXIT_CRITICAL(&mux);
}

String urlEncode(const String& s) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    out.reserve(s.length() * 2);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Runs the whole job; returns the feature count, or 0 with `err` filled.
unsigned doJob(char* err, size_t errLen) {
    setPhase(Phase::Waiting);
    Serial.printf("[mapjob] %s: querying %.4f,%.4f .. %.4f,%.4f\n", job.name, job.s, job.w, job.n, job.e);
    PsramSink sink(bytes);
    {
        String body = "data=" + urlEncode(mapbuild::query(job.s, job.w, job.n, job.e));
        NetworkClient client;
        HTTPClient http;
        http.setUserAgent(USER_AGENT);
        http.setConnectTimeout(15000);
        http.setTimeout(65000);  // uint16_t ms; Overpass usually answers in 30-45 s
        if (!http.begin(client, OVERPASS_URL)) {
            snprintf(err, errLen, "cannot reach the map server");
            return 0;
        }
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        int code = http.POST(body);
        if (code != HTTP_CODE_OK) {
            if (code == 429 || code == 502 || code == 503)
                snprintf(err, errLen, "map server busy, try again in a minute");
            else if (code == 504) snprintf(err, errLen, "map server timed out, try a smaller area");
            else if (code < 0) snprintf(err, errLen, "no connection to the map server");
            else snprintf(err, errLen, "map server error %d", code);
            http.end();
            return 0;
        }
        setPhase(Phase::Downloading);
        int got = http.writeToStream(&sink);
        http.end();
        if (got < 0 || !sink.buf) {
            snprintf(err, errLen, "%s", sink.overflow ? "area too detailed, draw a smaller one" : "download interrupted");
            return 0;
        }
    }
    Serial.printf("[mapjob] downloaded %u bytes in %lu ms\n", (unsigned)sink.len, (unsigned long)(millis() - startMs));
    setPhase(Phase::Converting);
    mapbuild::Result r;
    if (!mapbuild::build(sink.buf, job.s, job.w, job.n, job.e, job.name, job.path, Serial, r, err, errLen)) return 0;
    return r.features;
}

void run(void*) {
    char err[64] = "";
    unsigned features = doJob(err, sizeof(err));
    portENTER_CRITICAL(&mux);
    st.phase = features ? Phase::Done : Phase::Failed;
    st.features = features;
    st.elapsedMs = millis() - startMs;
    strlcpy(st.msg, err, sizeof(st.msg));
    portEXIT_CRITICAL(&mux);
    Serial.printf("[mapjob] %s %s in %lu ms%s%s\n", job.name, features ? "OK" : "ERR",
                  (unsigned long)(millis() - startMs), features ? "" : ": ", err);
    if (features) completed = true;
    running = false;
    vTaskDelete(nullptr);
}

}  // namespace

void pathFor(const char* name, char* out, size_t n) {
    char safe[25];
    size_t k = 0;
    bool sep = false;
    for (const char* p = name; *p && k < sizeof(safe) - 1; p++) {
        char c = *p;
        if (isalnum((unsigned char)c) && !(c & 0x80)) {
            if (sep && k) safe[k++] = '_';
            if (k < sizeof(safe) - 1) safe[k++] = tolower((unsigned char)c);
            sep = false;
        } else {
            sep = true;
        }
    }
    safe[k] = 0;
    snprintf(out, n, "/map/%s.rmap", k ? safe : "map");
}

bool start(const char* name, double south, double west, double north, double east, char* why, size_t whyLen) {
    if (running) {
        snprintf(why, whyLen, "already downloading a map");
        return false;
    }
    if (logger::sessionOpen()) {
        snprintf(why, whyLen, "stop the session first");
        return false;
    }
    if (!logger::sdReady()) {
        snprintf(why, whyLen, "no SD card");
        return false;
    }
    if (!net::connected()) {
        snprintf(why, whyLen, "connect to Wi-Fi first");
        return false;
    }
    double hKm = (north - south) * 111.32;
    double wKm = (east - west) * 111.32 * cos((south + north) / 2 * M_PI / 180.0);
    if (hKm > MAX_EDGE_KM + 0.05 || wKm > MAX_EDGE_KM + 0.05) {
        snprintf(why, whyLen, "area too large (max %.0f km)", MAX_EDGE_KM);
        return false;
    }
    if (hKm < 0.2 || wKm < 0.2) {
        snprintf(why, whyLen, "area too small");
        return false;
    }

    strlcpy(job.name, name, sizeof(job.name));
    pathFor(name, job.path, sizeof(job.path));
    job.s = south;
    job.w = west;
    job.n = north;
    job.e = east;
    startMs = millis();
    bytes = 0;
    portENTER_CRITICAL(&mux);
    st = Status();
    st.phase = Phase::Waiting;
    strlcpy(st.name, name, sizeof(st.name));
    portEXIT_CRITICAL(&mux);
    running = true;
    if (xTaskCreatePinnedToCore(run, "mapjob", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
        running = false;
        setPhase(Phase::Idle);
        snprintf(why, whyLen, "out of memory");
        return false;
    }
    return true;
}

Status status() {
    portENTER_CRITICAL(&mux);
    Status s = st;
    portEXIT_CRITICAL(&mux);
    s.bytes = bytes;
    if (running) s.elapsedMs = millis() - startMs;
    return s;
}

bool busy() { return running; }

bool takeCompleted() { return completed.exchange(false); }

}  // namespace mapjob
