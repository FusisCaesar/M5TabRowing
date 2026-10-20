// Vector icons for buttons, drawn with M5GFX primitives so they scale to any size.
#pragma once

#include <M5GFX.h>

namespace icons {

enum class Icon {
    Pencil, Trash, Download, Refresh, Check, Plus, Minus, Sun, Moon, Compass, BoatBow, BoatHead, Locate
};

// Draws `icon` centred on (cx, cy) inside a box of size x size; `bg` is the colour behind it
// (used for cut-outs).
void draw(M5Canvas& c, Icon icon, int cx, int cy, int size, uint16_t color, uint16_t bg);

// On/off switch: a pill with a knob, lit in `on` colour when set.
void drawSwitch(M5Canvas& c, int x, int y, int w, int h, bool set, uint16_t on, uint16_t off, uint16_t knob);

}  // namespace icons
