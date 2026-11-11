#pragma once

#include <Print.h>
#include <Stream.h>
#include <stdint.h>

namespace logger {

// Raw must stay last: it is the only stream that is opened lazily.
enum class Stream : uint8_t { Strokes = 0, Seconds, Laps, Info, Raw, Count };

bool begin();
bool sdReady();

// The next session's files are named from this, then it is cleared.
// Spaces become underscores, and " race" is added, so a segment "Tromp - S2"
// is stored as Tromp_-_S2_race. Empty leaves the session unnamed.
void setSessionLabel(const char* name);

// Returns the session number, or -1 when no SD card is available.
int startSession();
void endSession();
bool sessionOpen();
int sessionNumber();

void setRawEnabled(bool on);
bool rawActive();

// Non-blocking; lines are dropped (and counted) if the queue is full.
void write(Stream s, const char* line);
uint32_t dropped();

// Lists the session files with sizes and their first `headLines` lines, plus any maps in /map.
void listFiles(Print& out, int headLines);

// Receives `size` raw bytes from `in` and writes them to `path` on the SD card,
// creating parent directories. Verifies the CRC32 and reports progress/result to
// `out`. Refuses while a session is open. Returns true on success.
bool receiveFile(::Stream& in, const char* path, uint32_t size, uint32_t expectCrc, Print& out);

// Sends a file from the SD card to `io` in CRC-checked chunks driven by the host
// (see tools/fetch.py). Refuses while a session is open.
bool sendFile(::Stream& io, const char* path);

// Erases the card: one FAT32 partition over the whole card. Restart afterwards to remount.
bool formatCard(Print& out);

}  // namespace logger
