#pragma once

#include <M5Unified.h>

#include "palette.h"

// On-screen keyboard with a text field, for naming maps and recordings. Draws into
// the caller's canvas from `top` down, across the canvas width.
namespace osk {

enum class Result : uint8_t { None, Save, Cancel };

// Name: first letter capital, for map and session titles.
// Secret: case stays as typed, for a Wi-Fi password (up to 63 characters).
enum class Kind : uint8_t { Name, Secret };

void start(const char* current, int maxLen, Kind kind = Kind::Name);
const char* text();  // names have trailing spaces trimmed on SAVE

void draw(M5Canvas& cv, int top, const Palette& c);
Result tap(int x, int y, int width, int top);

}  // namespace osk
