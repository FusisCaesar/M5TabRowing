#pragma once

#include <Print.h>
#include <Stream.h>
#include <stdint.h>

namespace logger {

enum class Stream : uint8_t { Strokes = 0, Seconds, Laps, Raw, Count };

bool begin();
bool sdReady();

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

// Erases the card: one FAT32 partition over the whole card. Restart afterwards to remount.
bool formatCard(Print& out);

}  // namespace logger
