#include "icons.h"

#include <math.h>

namespace icons {

namespace {

void pencil(M5Canvas& c, float cx, float cy, float u, uint16_t col, uint16_t bg) {
    // Body along the diagonal from bottom-left (tip) to top-right (eraser).
    const float k = 0.70710678f;
    float dx = k, dy = -k;     // along the pencil
    float nx = k, ny = k;      // across it
    float half = 0.24f * u;
    float tipX = cx - 0.85f * u, tipY = cy + 0.85f * u;
    auto pt = [&](float along, float across, int& x, int& y) {
        x = (int)lroundf(tipX + dx * along + nx * across);
        y = (int)lroundf(tipY + dy * along + ny * across);
    };
    int ax, ay, bx, by, ex, ey, fx, fy, tx, ty;
    float bodyStart = 0.55f * u, bodyEnd = 2.35f * u;
    pt(bodyStart, -half, ax, ay);
    pt(bodyStart, half, bx, by);
    pt(bodyEnd, -half, ex, ey);
    pt(bodyEnd, half, fx, fy);
    c.fillTriangle(ax, ay, bx, by, ex, ey, col);
    c.fillTriangle(bx, by, ex, ey, fx, fy, col);
    pt(0, 0, tx, ty);
    c.fillTriangle(tx, ty, ax, ay, bx, by, col);
    // Gap between body and eraser.
    int gx0, gy0, gx1, gy1;
    pt(1.95f * u, -half - 1, gx0, gy0);
    pt(1.95f * u, half + 1, gx1, gy1);
    c.drawWideLine(gx0, gy0, gx1, gy1, 0.06f * u + 1, bg);
}

void trash(M5Canvas& c, float cx, float cy, float u, uint16_t col, uint16_t bg) {
    c.fillRect(cx - 0.3f * u, cy - 0.95f * u, 0.6f * u, 0.22f * u, col);  // handle
    c.fillRect(cx - 0.85f * u, cy - 0.78f * u, 1.7f * u, 0.2f * u, col);  // lid
    int top = cy - 0.48f * u, bot = cy + 0.95f * u;
    int l0 = cx - 0.65f * u, r0 = cx + 0.65f * u, l1 = cx - 0.5f * u, r1 = cx + 0.5f * u;
    c.fillTriangle(l0, top, r0, top, l1, bot, col);
    c.fillTriangle(r0, top, l1, bot, r1, bot, col);
    for (int i = -1; i <= 1; i++) {
        int x = cx + i * 0.3f * u;
        c.drawWideLine(x, cy - 0.25f * u, x, cy + 0.72f * u, 0.06f * u + 0.5f, bg);
    }
}

void download(M5Canvas& c, float cx, float cy, float u, uint16_t col) {
    c.fillRect(cx - 0.13f * u, cy - 0.95f * u, 0.26f * u, 0.85f * u, col);
    c.fillTriangle(cx - 0.55f * u, cy - 0.2f * u, cx + 0.55f * u, cy - 0.2f * u, cx, cy + 0.4f * u, col);
    c.fillRect(cx - 0.95f * u, cy + 0.72f * u, 1.9f * u, 0.22f * u, col);
    c.fillRect(cx - 0.95f * u, cy + 0.3f * u, 0.22f * u, 0.5f * u, col);
    c.fillRect(cx + 0.73f * u, cy + 0.3f * u, 0.22f * u, 0.5f * u, col);
}

void refresh(M5Canvas& c, float cx, float cy, float u, uint16_t col) {
    float r0 = 0.78f * u, r1 = 0.52f * u, end = 330;
    c.fillArc(cx, cy, r0, r1, 40, end, col);
    float a = end * (float)M_PI / 180, rm = (r0 + r1) / 2;
    float px = cx + cosf(a) * rm, py = cy + sinf(a) * rm;
    float rx = cosf(a), ry = sinf(a);    // radial
    float tx = -sinf(a), ty = cosf(a);   // tangent (direction of travel)
    float w = 0.42f * u, len = 0.45f * u;
    c.fillTriangle(px + rx * w, py + ry * w, px - rx * w, py - ry * w, px + tx * len, py + ty * len, col);
}

void check(M5Canvas& c, float cx, float cy, float u, uint16_t col) {
    float r = 0.13f * u;
    c.drawWideLine(cx - 0.75f * u, cy, cx - 0.25f * u, cy + 0.5f * u, r, col);
    c.drawWideLine(cx - 0.25f * u, cy + 0.5f * u, cx + 0.8f * u, cy - 0.6f * u, r, col);
}

void sun(M5Canvas& c, float cx, float cy, float u, uint16_t col) {
    c.fillSmoothCircle(cx, cy, 0.42f * u, col);
    for (int i = 0; i < 8; i++) {
        float a = i * (float)M_PI / 4;
        c.drawWideLine(cx + cosf(a) * 0.62f * u, cy + sinf(a) * 0.62f * u, cx + cosf(a) * 0.92f * u,
                       cy + sinf(a) * 0.92f * u, 0.07f * u + 0.5f, col);
    }
}

void moon(M5Canvas& c, float cx, float cy, float u, uint16_t col, uint16_t bg) {
    c.fillSmoothCircle(cx, cy, 0.78f * u, col);
    c.fillSmoothCircle(cx + 0.38f * u, cy - 0.3f * u, 0.62f * u, bg);
}

void compass(M5Canvas& c, float cx, float cy, float u, uint16_t col) {
    c.drawCircle(cx, cy, 0.88f * u, col);
    c.drawCircle(cx, cy, 0.88f * u - 1, col);
    c.fillTriangle(cx, cy - 0.72f * u, cx - 0.22f * u, cy + 0.08f * u, cx + 0.22f * u, cy + 0.08f * u, col);
    c.fillTriangle(cx, cy + 0.55f * u, cx - 0.14f * u, cy + 0.12f * u, cx + 0.14f * u, cy + 0.12f * u, col);
}

// Boat hull pointing `up` (bow at the top) or down (bow at the bottom, a head at the stern).
void boat(M5Canvas& c, float cx, float cy, float u, uint16_t col, bool bowUp) {
    float dir = bowUp ? -1.0f : 1.0f;
    float tipY = cy + dir * 0.78f * u;
    float backY = cy - dir * 0.55f * u;
    float half = 0.42f * u;
    c.fillTriangle(cx, tipY, cx - half, backY, cx + half, backY, col);
    if (!bowUp) c.fillSmoothCircle(cx, backY - 0.22f * u, 0.16f * u, col);
}

}  // namespace

void draw(M5Canvas& c, Icon icon, int cx, int cy, int size, uint16_t color, uint16_t bg) {
    float u = size / 2.0f;
    switch (icon) {
        case Icon::Pencil: pencil(c, cx, cy, u, color, bg); break;
        case Icon::Trash: trash(c, cx, cy, u, color, bg); break;
        case Icon::Download: download(c, cx, cy, u, color); break;
        case Icon::Refresh: refresh(c, cx, cy, u, color); break;
        case Icon::Check: check(c, cx, cy, u, color); break;
        case Icon::Plus:
            c.fillRect(cx - 0.8f * u, cy - 0.12f * u, 1.6f * u, 0.24f * u, color);
            c.fillRect(cx - 0.12f * u, cy - 0.8f * u, 0.24f * u, 1.6f * u, color);
            break;
        case Icon::Minus: c.fillRect(cx - 0.8f * u, cy - 0.12f * u, 1.6f * u, 0.24f * u, color); break;
        case Icon::Sun: sun(c, cx, cy, u, color); break;
        case Icon::Moon: moon(c, cx, cy, u, color, bg); break;
        case Icon::Compass: compass(c, cx, cy, u, color); break;
        case Icon::BoatBow: boat(c, cx, cy, u, color, true); break;
        case Icon::BoatHead: boat(c, cx, cy, u, color, false); break;
        case Icon::Locate:
            c.drawCircle(cx, cy, 0.72f * u, color);
            c.drawCircle(cx, cy, 0.72f * u - 1, color);
            c.fillSmoothCircle(cx, cy, 0.16f * u, color);
            c.drawWideLine(cx, cy - 0.92f * u, cx, cy - 0.58f * u, 0.08f * u + 0.5f, color);
            c.drawWideLine(cx, cy + 0.58f * u, cx, cy + 0.92f * u, 0.08f * u + 0.5f, color);
            c.drawWideLine(cx - 0.92f * u, cy, cx - 0.58f * u, cy, 0.08f * u + 0.5f, color);
            c.drawWideLine(cx + 0.58f * u, cy, cx + 0.92f * u, cy, 0.08f * u + 0.5f, color);
            break;
    }
}

void drawSwitch(M5Canvas& c, int x, int y, int w, int h, bool set, uint16_t on, uint16_t off, uint16_t knob) {
    c.fillSmoothRoundRect(x, y, w, h, h / 2, set ? on : off);
    int r = h / 2 - 6;
    int kx = set ? x + w - h / 2 : x + h / 2;
    c.fillSmoothCircle(kx, y + h / 2, r, knob);
}

}  // namespace icons
