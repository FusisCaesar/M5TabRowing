#pragma once

#include <math.h>

// Rowing waters offered on the Maps screen. Each is downloaded from OpenStreetMap
// as a square of 2 x radiusKm around the centre (at most mapjob::MAX_EDGE_KM wide).
namespace maplib {

struct Entry {
    const char* name;
    const char* region;
    double lat, lon;
    float radiusKm;
};

constexpr Entry ENTRIES[] = {
    {"Bosbaan", "Amsterdam, NL", 52.3263, 4.8376, 3.0f},
    {"Amstel", "Amsterdam - Ouderkerk, NL", 52.3300, 4.9050, 3.5f},
    {"Willem-Alexander Baan", "Rotterdam, NL", 51.9976, 4.5614, 2.5f},
    {"Kralingse Plas", "Rotterdam, NL", 51.9347, 4.5151, 2.0f},
    {"Delftse Schie", "Delft, NL", 51.9900, 4.3800, 3.0f},
    {"Galgewater / Oude Rijn", "Leiden, NL", 52.1610, 4.4950, 3.0f},
    {"Zegerplas", "Alphen aan den Rijn, NL", 52.1358, 4.6839, 1.5f},
    {"Reeuwijkse Plassen", "Gouda, NL", 52.0387, 4.7450, 3.0f},
    {"Vinkeveense Plassen", "Vinkeveen, NL", 52.2339, 4.9435, 3.0f},
    {"Spaarne", "Haarlem, NL", 52.3785, 4.6378, 3.0f},
    {"Vecht", "Utrecht, NL", 52.1050, 5.0850, 3.5f},
    {"Beatrixkanaal", "Eindhoven, NL", 51.4544, 5.4065, 3.0f},
    {"Paterswoldsemeer", "Groningen, NL", 53.1700, 6.5600, 2.5f},
    {"IJssel", "Deventer, NL", 52.2550, 6.1500, 3.0f},
    {"Maas", "Maastricht, NL", 50.8500, 5.6950, 3.0f},
    {"Watersportbaan", "Gent, BE", 51.0503, 3.6862, 2.5f},
    {"Regattabahn Wedau", "Duisburg, DE", 51.4043, 6.7914, 2.5f},
    {"Beetzsee", "Brandenburg an der Havel, DE", 52.4520, 12.5700, 3.0f},
    {"Dorney Lake", "Eton, UK", 51.4938, -0.6640, 2.0f},
    {"Henley Reach", "Henley-on-Thames, UK", 51.5480, -0.8950, 3.0f},
    {"Tideway", "London, UK", 51.4700, -0.2350, 3.5f},
    {"Rotsee", "Lucerne, CH", 47.0697, 8.3142, 2.0f},
    {"Lake Bled", "Bled, SI", 46.3639, 14.0938, 2.0f},
};

constexpr int COUNT = sizeof(ENTRIES) / sizeof(ENTRIES[0]);

inline void bounds(const Entry& e, double& s, double& w, double& n, double& east) {
    double dLat = e.radiusKm / 111.32;
    double dLon = e.radiusKm / (111.32 * cos(e.lat * M_PI / 180.0));
    s = e.lat - dLat;
    n = e.lat + dLat;
    w = e.lon - dLon;
    east = e.lon + dLon;
}

}  // namespace maplib
