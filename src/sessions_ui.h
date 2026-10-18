#pragma once

#include "palette.h"

// The Sessions screen: the recordings on the SD card (rename, delete) and the review of
// one session (track on the map when it has GPS, totals, graphs of split, rate and heart
// rate). ui.cpp owns the top bar and the bottom buttons; this draws the area between.
namespace sessionsui {

void begin(int x, int y, int w, int h);
void setColors(const Palette& c);

void enter();
void leave();
void draw(bool force);

// True while a recorded session is shown; the live map should not follow the GPS then.
bool reviewing();
bool editing();
// Closes the review or the rename keyboard.
void back();

// Screen coordinates. A tap is a touch without movement; drags scroll the list or pan the map.
void tap(int x, int y);
void dragStart(int x, int y);
void dragMove(int x, int y);
void dragEnd(int x, int y);

}  // namespace sessionsui
