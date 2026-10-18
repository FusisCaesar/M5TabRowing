#pragma once

#include <stddef.h>
#include <stdint.h>

// Downloads OpenStreetMap water data for an area over Wi-Fi and converts it to
// /map/<name>.rmap, in a background task so the UI keeps running.
namespace mapjob {

enum class Phase : uint8_t { Idle, Waiting, Downloading, Converting, Done, Failed };

struct Status {
    Phase phase = Phase::Idle;
    uint32_t bytes = 0;      // downloaded so far
    uint32_t elapsedMs = 0;  // since the job started (frozen when it ends)
    unsigned features = 0;   // when Done
    char name[32] = "";
    char msg[64] = "";       // reason when Failed
};

// Largest area edge accepted, to keep the download and conversion within memory.
constexpr float MAX_EDGE_KM = 8.0f;

// Starts a job; false with `why` filled if it cannot start (busy, no Wi-Fi,
// session recording, area too large).
bool start(const char* name, double south, double west, double north, double east, char* why, size_t whyLen);
Status status();
bool busy();

// True once after a job finished successfully (the caller rescans /map).
bool takeCompleted();

// "/map/<safe name>.rmap" for a display name.
void pathFor(const char* name, char* out, size_t n);

}  // namespace mapjob
