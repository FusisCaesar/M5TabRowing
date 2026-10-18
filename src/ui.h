#pragma once

namespace ui {

void begin();
void loop();

// Serial equivalents of the buttons: N next screen, G start, X stop, L lap, R reset, M maps.
void command(char c);

// Simulated touch input (serial testing), in screen coordinates.
void tap(int x, int y);
void hold(int x, int y);
void drag(int x0, int y0, int x1, int y1);

}  // namespace ui
