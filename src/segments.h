#pragma once

#include <stdint.h>

#include "targets.h"

// A course made by joining targets in order: the first is the start, the last
// is the finish, and anything between is a via point. The length follows that
// path. One segment can be selected. Before its start line the distance tiles
// count down to that line; after the boat crosses it they follow the distance
// still left along the points.
namespace segments {

constexpr int MAX = 8;
constexpr int NAME_LEN = 40;  // a map name plus " - S12"
constexpr int MAX_POINTS = targets::MAX;
// Half the drawn start line, each side of the start pin, square to the first leg.
constexpr float START_LINE_HALF_M = 40;

struct Segment {
    char name[NAME_LEN];
    uint8_t count;
    int8_t point[MAX_POINTS];  // target indexes; [0] start, last finish
};

void begin();

int count();
const Segment* get(int index);
int selected();              // -1 if none
const char* selectedName();  // empty if none
void select(int index);      // -1 clears it

// points are target indexes, at least a start and a finish, each used once.
// A new segment is named "Map name - S1" when the start sits on a named map.
bool add(const int8_t* points, int npoints);
bool update(int index, const int8_t* points, int npoints);  // keeps the name
bool rename(int index, const char* name);
void remove(int index);

// A target was deleted at this index. Drops it from every segment and closes
// the gap; a segment with no start and finish left is removed.
void onTargetRemoved(int targetIndex);

// Metres along the points, or a negative value if a point is missing.
float lengthAlong(const int8_t* points, int npoints);
float length(int index);

// Advance along the selected segment from this position, then the metres still
// left along the points to the finish.
void track(double lat, double lon);
float remaining(double lat, double lon);
int nextPoint();  // index into the selected segment, or -1

// Lat/lon of the selected segment, in order. Returns how many were written.
int selectedRoute(double* lat, double* lon, int cap);

// The start line: through the start pin, square to the first leg, START_LINE_HALF_M each side.
bool startLine(double& lat1, double& lon1, double& lat2, double& lon2);

// True once the boat is past the start line. Before that, distanceToStart is the
// metres to the nearest point on that line.
bool onCourse();
float distanceToStart(double lat, double lon);

// Watch a fix. True on the sample where the boat crosses the start line along the
// course, moving fast enough that the GPS heading is real. travelled is the
// monotonic metres from gps. reliable is false for a held or rejected fix, which
// still updates the distance but cannot start a piece.
bool pollStart(double lat, double lon, float speedMps, float courseDeg, double travelled, bool reliable);

}  // namespace segments
