#include "recordings.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "logger.h"
#include "rtclock.h"

namespace recordings {

namespace {

constexpr const char* DIR = "/rowing";
constexpr int MAX_SESSIONS = 400;
constexpr int MAX_FILES_PER_SESSION = 8;

struct Entry {
    Info info;
    char raw[MAX_LABEL + 1];  // label as in the file names
};

Entry* entries = nullptr;
int n = 0;

// "S0007_strokes.csv" -> 7, "", "strokes"; "S0007_Bosbaan_intervals_imu.csv" -> 7, "Bosbaan_intervals", "imu".
bool parseName(const char* name, int& number, char* raw, size_t rawLen, char* stream, size_t streamLen) {
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    size_t len = strlen(base);
    if (len < 11 || base[0] != 'S' || base[5] != '_' || strcasecmp(base + len - 4, ".csv") != 0) return false;
    for (int i = 1; i < 5; i++)
        if (!isdigit((unsigned char)base[i])) return false;
    number = atoi(base + 1);
    const char* rest = base + 6;
    const char* end = base + len - 4;
    const char* lastSep = nullptr;
    for (const char* p = rest; p < end; p++)
        if (*p == '_') lastSep = p;
    const char* streamStart = lastSep ? lastSep + 1 : rest;
    snprintf(stream, streamLen, "%.*s", (int)(end - streamStart), streamStart);
    if (lastSep) snprintf(raw, rawLen, "%.*s", (int)(lastSep - rest), rest);
    else raw[0] = '\0';
    return true;
}

void sessionPath(const Entry& e, const char* stream, char* out, size_t len) {
    if (e.raw[0]) snprintf(out, len, "%s/S%04d_%s_%s.csv", DIR, e.info.number, e.raw, stream);
    else snprintf(out, len, "%s/S%04d_%s.csv", DIR, e.info.number, stream);
}

// "2026-10-18T19:30:21Z" -> "2026-10-18 21:30" in local time
void fmtUtc(const char* iso, char* out, size_t len) {
    time_t t = rtclock::parseIsoUtc(iso);
    if (t) rtclock::localText(t, out, len);
    else out[0] = '\0';
}

void readDetails(Entry& e) {
    char path[96];
    sessionPath(e, "info", path, sizeof(path));
    File f = SD_MMC.open(path);
    if (f) {
        while (f.available()) {
            String line = f.readStringUntil('\n');
            if (line.startsWith("start_utc,")) {
                fmtUtc(line.c_str() + 10, e.info.start, sizeof(e.info.start));
                break;
            }
        }
        f.close();
    }

    // Seconds file: the first data row has the UTC time, the last one the duration.
    sessionPath(e, "seconds", path, sizeof(path));
    f = SD_MMC.open(path);
    if (!f) return;
    f.readStringUntil('\n');
    String first = f.readStringUntil('\n');
    int c1 = first.indexOf(','), c2 = first.indexOf(',', c1 + 1);
    if (!e.info.start[0] && c1 > 0 && c2 > c1) {
        fmtUtc(first.substring(c1 + 1, c2).c_str(), e.info.start, sizeof(e.info.start));
    }
    size_t size = f.size();
    if (size > 2) {
        char tail[160];
        size_t from = size > sizeof(tail) - 1 ? size - (sizeof(tail) - 1) : 0;
        f.seek(from);
        int got = f.read((uint8_t*)tail, size - from);
        if (got > 0) {
            tail[got] = '\0';
            while (got > 0 && (tail[got - 1] == '\n' || tail[got - 1] == '\r')) tail[--got] = '\0';
            const char* last = strrchr(tail, '\n');
            e.info.durationS = (float)atof(last ? last + 1 : tail);
        }
    }
    f.close();
}

// Full paths of every file of this session.
int sessionFiles(int number, char paths[][96]) {
    int count = 0;
    File dir = SD_MMC.open(DIR);
    if (!dir) return 0;
    for (File f = dir.openNextFile(); f && count < MAX_FILES_PER_SESSION; f = dir.openNextFile()) {
        int num;
        char raw[64], stream[24];
        const char* name = f.name();
        if (parseName(name, num, raw, sizeof(raw), stream, sizeof(stream)) && num == number) {
            const char* base = strrchr(name, '/');
            snprintf(paths[count++], 96, "%s/%s", DIR, base ? base + 1 : name);
        }
        f.close();
    }
    dir.close();
    return count;
}

}  // namespace

void scan() {
    if (!entries) entries = (Entry*)heap_caps_calloc(MAX_SESSIONS, sizeof(Entry), MALLOC_CAP_SPIRAM);
    n = 0;
    if (!entries || !logger::sdReady()) return;
    File dir = SD_MMC.open(DIR);
    if (!dir) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        int number;
        char raw[64], stream[24];
        if (parseName(f.name(), number, raw, sizeof(raw), stream, sizeof(stream))) {
            Entry* e = nullptr;
            for (int i = 0; i < n; i++)
                if (entries[i].info.number == number) e = &entries[i];
            if (!e && n < MAX_SESSIONS) {
                e = &entries[n++];
                memset(e, 0, sizeof(*e));
                e->info.number = number;
            }
            if (e) {
                e->info.bytes += f.size();
                if (raw[0] && !e->raw[0]) strlcpy(e->raw, raw, sizeof(e->raw));
            }
        }
        f.close();
    }
    dir.close();
    std::sort(entries, entries + n, [](const Entry& a, const Entry& b) { return a.info.number > b.info.number; });
    for (int i = 0; i < n; i++) {
        Entry& e = entries[i];
        strlcpy(e.info.label, e.raw, sizeof(e.info.label));
        for (char* p = e.info.label; *p; p++)
            if (*p == '_') *p = ' ';
        readDetails(e);
    }
}

int count() { return n; }

bool get(int i, Info& out) {
    if (i < 0 || i >= n) return false;
    out = entries[i].info;
    return true;
}

bool pathOf(int number, const char* stream, char* out, size_t len) {
    for (int i = 0; i < n; i++) {
        if (entries[i].info.number != number) continue;
        sessionPath(entries[i], stream, out, len);
        return SD_MMC.exists(out);
    }
    return false;
}

bool rename(int number, const char* label) {
    if (logger::sessionOpen()) return false;
    char raw[MAX_LABEL + 1];
    size_t k = 0;
    bool gap = false;
    for (const char* p = label; *p && k < MAX_LABEL; p++) {
        char c = *p;
        if (isalnum((unsigned char)c) || c == '-') {
            if (gap && k) raw[k++] = '_';
            if (k < MAX_LABEL) raw[k++] = c;
            gap = false;
        } else {
            gap = true;
        }
    }
    raw[k] = '\0';

    char paths[MAX_FILES_PER_SESSION][96];
    int count = sessionFiles(number, paths);
    bool ok = count > 0;
    for (int i = 0; i < count; i++) {
        int num;
        char oldRaw[64], stream[24], to[96];
        parseName(paths[i], num, oldRaw, sizeof(oldRaw), stream, sizeof(stream));
        if (raw[0]) snprintf(to, sizeof(to), "%s/S%04d_%s_%s.csv", DIR, number, raw, stream);
        else snprintf(to, sizeof(to), "%s/S%04d_%s.csv", DIR, number, stream);
        if (strcmp(to, paths[i]) == 0) continue;
        if (!SD_MMC.rename(paths[i], to)) ok = false;
    }
    Serial.printf("[rec] rename S%04d to '%s': %s\n", number, raw, ok ? "OK" : "failed");
    scan();
    return ok;
}

bool remove(int number) {
    if (logger::sessionOpen()) return false;
    char paths[MAX_FILES_PER_SESSION][96];
    int count = sessionFiles(number, paths);
    bool ok = count > 0;
    for (int i = 0; i < count; i++)
        if (!SD_MMC.remove(paths[i])) ok = false;
    Serial.printf("[rec] delete S%04d (%d files): %s\n", number, count, ok ? "OK" : "failed");
    scan();
    return ok;
}

}  // namespace recordings
