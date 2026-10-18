#pragma once

#include <stdint.h>

// The Maps screen: download maps from a built-in library of rowing waters, draw
// an area on an OpenStreetMap background, and manage the maps on the SD card.
// ui.cpp owns the top bar and the bottom buttons; this draws the area between.
namespace mapsui {

struct Colors {
    uint16_t bg, fg, label, accent, dim, btn, btnText, warn, ok;
};

enum class View : uint8_t { Library, Draw, OnCard };

// Screen rectangle this screen draws into.
void begin(int x, int y, int w, int h);
void setColors(const Colors& c);

void enter();
void leave();
void draw(bool force);

View view();
void setView(View v);  // also closes a session review, even for the current view

// True while a recorded session is shown; the live map should not follow the GPS then.
bool reviewing();

// Screen coordinates. A tap is a touch without movement; drags pan or select.
void tap(int x, int y);
void dragStart(int x, int y);
void dragMove(int x, int y);
void dragEnd(int x, int y);

// Shows a short message in the header (e.g. why a download cannot start).
void message(const char* text);

}  // namespace mapsui
