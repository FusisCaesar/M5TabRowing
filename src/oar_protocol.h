#pragma once

// What an oar sensor (M5Stack StickS3, see stick/) broadcasts, and what the Tab5 listens for.
// Shared by both firmwares: the stick build adds ../src to its include path.
//
// The stick never connects. It puts this struct in the manufacturer data of a non-connectable
// advertisement every 50 ms. Everything about the last stroke is repeated in every packet until the
// next stroke, so a lost packet costs nothing. All fields are little endian.
//
// The stick keeps its own clock (`ms`, milliseconds since it started). Stroke times are sent as
// "this many ms before `ms`", so the Tab5 can place them on its own clock once it knows the offset
// between the two (see oars.cpp). The Tab5 never has to trust how quickly a packet got through.

#include <stddef.h>
#include <stdint.h>

namespace oarproto {

// Company id 0xFFFF is the one the Bluetooth SIG reserves for testing and internal use.
constexpr uint16_t COMPANY_ID = 0xFFFF;
constexpr uint8_t MAGIC = 0x4F;  // 'O'
constexpr uint8_t VERSION = 1;

// Bits of Packet::flags.
constexpr uint8_t FLAG_BUTTON = 0x01;   // button A is down right now: used to pair the stick
constexpr uint8_t FLAG_ZEROED = 0x02;   // the oar angle has been zeroed square to the boat
constexpr uint8_t FLAG_STROKE = 0x04;   // the stroke fields below hold a real stroke

constexpr uint16_t AGO_NONE = 0xFFFF;   // time too long ago to send

#pragma pack(push, 1)
struct Packet {
    uint8_t magic;         // MAGIC
    uint8_t version;       // VERSION
    uint16_t id;           // identifies the stick, from its MAC; shown on its screen
    uint8_t seq;           // counts packets, wraps
    uint8_t flags;         // FLAG_*
    uint32_t ms;           // stick clock when the packet was built
    int16_t angleDeci;     // oar angle now, 0.1 degree, 0 = square to the boat
    uint8_t battery;       // percent, 255 = unknown
    uint8_t strokeNo;      // counts strokes, wraps; a new value means a new stroke
    uint16_t catchAgoMs;   // catch of the last stroke, this long before `ms`
    uint16_t finishAgoMs;  // finish of the last stroke, this long before `ms`
    uint16_t recoveryMs;   // from the finish before it to this catch, 0 = unknown
    int16_t catchDeci;     // oar angle at the catch, 0.1 degree
    int16_t finishDeci;    // oar angle at the finish, 0.1 degree
};
#pragma pack(pop)

// Manufacturer data is a 2 byte company id and then the packet. With the 3 byte flags field a legacy
// advertisement has 31 - 3 - 4 = 24 bytes left for the packet.
static_assert(sizeof(Packet) == 24, "the packet must fit a legacy advertisement");

// Finds the packet in a raw advertisement payload (a list of length, type, data fields).
// Returns false when it is not one of ours. `out` is only written on success.
inline bool parse(const uint8_t* p, size_t len, Packet& out) {
    for (size_t i = 0; i + 1 < len;) {
        size_t l = p[i];
        if (l == 0 || i + 1 + l > len) break;
        uint8_t type = p[i + 1];
        const uint8_t* d = p + i + 2;
        size_t n = l - 1;
        if (type == 0xFF && n == 2 + sizeof(Packet) && (d[0] | (d[1] << 8)) == COMPANY_ID &&
            d[2] == MAGIC && d[3] == VERSION) {
            // memcpy, because the payload is not aligned.
            uint8_t* o = reinterpret_cast<uint8_t*>(&out);
            for (size_t k = 0; k < sizeof(Packet); k++) o[k] = d[2 + k];
            return true;
        }
        i += 1 + l;
    }
    return false;
}

// Four hex digits, the way the stick shows its id and the Tab5 lists it. `out` needs 5 bytes.
inline void idText(uint16_t id, char* out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = hex[(id >> 12) & 15];
    out[1] = hex[(id >> 8) & 15];
    out[2] = hex[(id >> 4) & 15];
    out[3] = hex[id & 15];
    out[4] = 0;
}

}  // namespace oarproto
