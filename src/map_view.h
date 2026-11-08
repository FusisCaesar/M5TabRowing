#pragma once

#include <M5Unified.h>

// Draws the loaded waterway map, the recorded track and the boat into a canvas.
// The caller sizes the canvas and pushes it (via fb::push). Pure drawing: it reads
// map_data and track but keeps no screen state.
namespace mapview {

// What is at the top of the map. Cycle order while rowing: Head, Bow, North.
// HeadUp is what the rower sees (stern / looking direction up). BowUp is the
// direction of travel up. NorthUp keeps north at the top.
enum Orientation : uint8_t { HeadUp = 0, BowUp, NorthUp, ORIENTATION_COUNT };
const char* orientationName(uint8_t o);  // "HEAD UP", "BOW UP", "NORTH UP"

struct View {
    double lat = 0, lon = 0;   // boat position
    float course = 0;          // deg, course over ground
    bool haveFix = false;
    float metersPerPixel = 1;  // zoom
    uint8_t orientation = NorthUp;
    bool showControls = true;   // draw the on-map zoom buttons
    bool largeControls = false; // session: double-size zoom and an orientation toggle
    // When followBoat is false, the point at the screen centre is (centerLat, centerLon)
    // instead of the boat, and showRecenter draws a locate button.
    bool followBoat = true;
    double centerLat = 0, centerLon = 0;
    bool showRecenter = false;
    bool day = false;  // paper-map colours; false is the night palette

    // Review: draw this recorded track (one colour per point) centred on lat/lon instead of
    // the live track and boat. Works without a map file for the area.
    const double* trackLat = nullptr;
    const double* trackLon = nullptr;
    const uint16_t* trackColor = nullptr;
    int trackCount = 0;

    // Live targets. The selected one is drawn larger, with a line from the boat
    // unless a segment route is set, which is drawn instead. The pointers stay
    // valid until the next frame starts.
    struct Pin {
        double lat = 0, lon = 0;
        bool selected = false;
        bool editing = false;  // a waypoint being placed or moved; drawn at triple size
        char name[40] = "";
    };
    const Pin* pins = nullptr;
    int pinCount = 0;

    // Selected segment, start to finish through the via points.
    const double* routeLat = nullptr;
    const double* routeLon = nullptr;
    int routeCount = 0;
    int routeNext = 0;  // point the boat is heading for

    // Start line across the first point, and finish line across the last, in lat/lon.
    bool showStartLine = false;
    double lineLat[2] = {};
    double lineLon[2] = {};
    bool showFinishLine = false;
    double finishLat[2] = {};
    double finishLon[2] = {};

    // While a waypoint is being dragged the map underneath is left as it was.
    // That frame is rendered without this pin and without the course, which are
    // drawn afterwards by paintCourse.
    int omitPin = -1;
    bool omitRoute = false;
};

void begin();  // allocates scratch buffers in PSRAM
void render(M5Canvas& c, const View& v);

// Course line and the open sight, drawn on a map frame that omitted them.
void paintCourse(M5Canvas& c, const View& v);

// Rectangles of the on-map buttons, in canvas coordinates, for hit testing.
bool zoomInRect(const M5Canvas& c, int& x, int& y, int& w, int& h, bool large = false);
bool zoomOutRect(const M5Canvas& c, int& x, int& y, int& w, int& h, bool large = false);
bool orientationRect(const M5Canvas& c, int& x, int& y, int& w, int& h, bool large = false);
bool recenterRect(const M5Canvas& c, int& x, int& y, int& w, int& h, bool large = false);

// Move the view centre with a finger drag (screen pixels, y down).
void panCenter(double& lat, double& lon, float dxPx, float dyPx, float metersPerPixel, uint8_t orientation,
               float course);

// Lat/lon under a point on the canvas this view was drawn into. False when there is no map.
bool latLonAt(const View& v, int canvasW, int canvasH, float sx, float sy, double& lat, double& lon);

}  // namespace mapview
