#pragma once

// Points dropped on the map. One of them can be selected; the DTT, TTT and
// FINISH tiles follow that one. Kept in flash.
namespace targets {

constexpr int NAME_LEN = 40;  // a map name plus " - T12"

struct Target {
    char name[NAME_LEN];
    double lat, lon;
};

void begin();

int count();
const Target* get(int index);
int selected();                 // -1 if none
const char* selectedName();     // empty if none
void select(int index);         // -1 clears the selection
// On a named map the name is "Map name - T1"; otherwise just "T1". Selects the new one.
bool add(double lat, double lon);
bool rename(int index, const char* name);  // false if the name is empty
// Moves the point. save is false while a finger is still dragging, then true on release.
void move(int index, double lat, double lon, bool save);
void remove(int index);

// Metres from (lat, lon) to the selected target, or a negative value if none is selected.
float distanceFrom(double lat, double lon);
float metresBetween(double lat, double lon, double lat2, double lon2);

}  // namespace targets
