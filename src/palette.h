#pragma once

#include <stdint.h>

// Theme colours handed from ui.cpp to the screens that draw their own content.
struct Palette {
    uint16_t bg, fg, label, accent, dim, btn, btnText, warn, ok;
};
