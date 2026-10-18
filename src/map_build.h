#pragma once

#include <Print.h>
#include <stddef.h>

// Builds an offline map (.rmap) on the device from an Overpass "out geom" JSON
// response: the same classification, clipping and simplification as
// tools/make_map.py, written in the format map_data reads.
namespace mapbuild {

struct Result {
    unsigned features = 0;
    unsigned bytes = 0;
};

// The Overpass query for the area south/west/north/east (same as make_map.py).
String query(double south, double west, double north, double east);

// Converts the NUL-terminated JSON `text` for the area into `outPath`, with `name`
// stored in the header. Reports counts and timing to `log`; on failure `err`
// holds a short reason. Takes a few seconds for a 6 x 6 km area.
bool build(const char* text, double south, double west, double north, double east, const char* name,
           const char* outPath, Print& log, Result& result, char* err, size_t errLen);

}  // namespace mapbuild
