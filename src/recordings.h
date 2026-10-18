#pragma once

#include <stddef.h>
#include <stdint.h>

// The session recordings in /rowing on the SD card. A session's files are
// S####_<stream>.csv, or S####_<label>_<stream>.csv once it has been given a name;
// spaces in the label are stored as '_'.
namespace recordings {

constexpr int MAX_LABEL = 24;

struct Info {
    int number;
    char label[MAX_LABEL + 1];  // as shown, with spaces; empty if never named
    char start[20];             // "2026-10-18 21:30" (local time) or empty if unknown
    float durationS;            // 0 if unknown
    uint32_t bytes;             // all files of the session
};

// Re-reads the card. Newest session first.
void scan();
int count();
bool get(int i, Info& out);
// Full path of one of a scanned session's files, e.g. stream "seconds".
bool pathOf(int number, const char* stream, char* out, size_t len);

// Both refuse while a session is recording.
bool rename(int number, const char* label);
bool remove(int number);

}  // namespace recordings
