#pragma once

#include <stddef.h>
#include <stdint.h>

// Oar sensors (experimental): one M5Stack StickS3 on the oar of each rower, firmware in stick/.
//
// The sticks never connect. Each broadcasts its oar angle and, for every stroke, when its catch and
// finish were (oar_protocol.h). The Tab5 listens with the scan that already runs in the hr task, which
// owns the Bluetooth stack; this module parses the packets, keeps the clocks of the sticks and the
// Tab5 together, and compares every seat with the reference seat: the highest numbered seat that is
// online, the stroke seat in a crew boat (seat 1 is the bow).
//
// "Catch" and "finish" here are the turning points of the oar at the two ends of its arc, not the blade
// entering and leaving the water. Every stick measures them the same way, so seats can be compared.

namespace oars {

constexpr int MAX_SEATS = 8;
constexpr uint32_t ONLINE_MS = 1500;  // a stick not heard for this long is offline

struct SeatView {
    bool paired = false;
    bool online = false;
    uint16_t id = 0;
    char idText[5] = "";
    uint32_t ageMs = 0;  // since the last packet
    int rssi = 0;
    int battery = -1;  // percent, -1 unknown
    bool zeroed = false;
    float angleDeg = 0;  // oar angle now, positive towards the stern
    float packetsPerS = 0;
    int strokes = 0;  // strokes received since the app started

    // The last stroke that was compared with the reference seat.
    bool hasStroke = false;
    float catchDeg = 0, finishDeg = 0, arcDeg = 0;
    float driveS = 0, recoveryS = 0;  // recovery 0 = unknown
    bool hasOffset = false;
    float catchOffsetMs = 0, finishOffsetMs = 0;  // positive = later than the reference seat

    // Over the last five strokes.
    int avgN = 0;
    bool avgHasOffset = false;
    float avgCatchOffsetMs = 0, avgFinishOffsetMs = 0;
    float avgArcDeg = 0, avgDriveS = 0, avgRecoveryS = 0;
};

struct View {
    int paired = 0;  // seats with a stick
    int online = 0;
    int ref = -1;  // reference seat index, -1 while no stick is online
    SeatView seat[MAX_SEATS];
    // How far apart the seats are: latest minus earliest, averaged over the last five strokes.
    bool hasSpread = false;
    float catchSpreadMs = 0, finishSpreadMs = 0;
    int pairingSeat = -1;  // seat waiting for a stick to be pressed, -1 when not pairing
};

void begin();
// Call often (main loop): compares new strokes with the reference seat, writes the log, runs the simulator.
void update();

View view();

// Settings side. Safe from any task.
void startPairing(int seat);  // the next stick that has its button pressed becomes this seat
void cancelPairing();
void forget(int seat);
bool hasSaved();          // at least one seat has a stick
bool wantsScan();         // the Bluetooth task should listen: a stick is paired or one is being paired
int pairingSeat();

// One line for the serial status: per seat id, rssi and packets per second. Returns n.
size_t describe(char* out, size_t n);

// Bluetooth task side.
void onAdvert(const uint8_t* addr, uint8_t addrType, int rssi, const uint8_t* payload, size_t len);

}  // namespace oars
