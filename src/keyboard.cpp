#include "keyboard.h"

#include <ctype.h>

#include <algorithm>

namespace osk {
namespace {

constexpr char K_DEL = '\b', K_CAPS = 1, K_CANCEL = 2, K_SAVE = 3, K_CLEAR = 4, K_SYM = 5;
constexpr int KEY_W = 112, KEY_H = 68, KEY_GAP = 8;
constexpr int FIELD_DY = 14, FIELD_H = 64;

char editText[64] = "";
int editMax = 0;
bool caps = true;
bool symbols = false;
Kind kind = Kind::Name;

struct Key {
    int x, y, w, h;
    char code;
    bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Fills `keys`, returns the count. Five rows of ten key widths each.
int layoutKeys(Key* keys, int width, int top) {
    static const char* const LETTERS[] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm"};
    static const char* const SYMBOLS[] = {"!@#$%^&*()", "_+-=[]{}|~", ";:'\",.<>/?", "`\\"};
    const char* const* rows = symbols ? SYMBOLS : LETTERS;
    int x0 = (width - (10 * KEY_W + 9 * KEY_GAP)) / 2;
    int n = 0;
    auto add = [&](int row, int col, int span, char code) {
        int y = top + FIELD_DY + FIELD_H + 16 + row * (KEY_H + KEY_GAP);
        keys[n++] = {x0 + col * (KEY_W + KEY_GAP), y, span * KEY_W + (span - 1) * KEY_GAP, KEY_H, code};
    };
    for (int r = 0; r < 4; r++)
        for (int k = 0; rows[r][k]; k++) add(r, k, 1, rows[r][k]);
    if (!symbols) add(3, 7, 1, '.');
    add(3, 8, 2, K_DEL);
    add(4, 0, 2, K_CAPS);
    add(4, 2, 4, ' ');
    add(4, 6, 1, kind == Kind::Secret ? K_SYM : K_CLEAR);
    add(4, 7, 1, K_CANCEL);
    add(4, 8, 2, K_SAVE);
    return n;
}

}  // namespace

void start(const char* current, int maxLen, Kind k) {
    kind = k;
    symbols = false;
    editMax = std::min((int)sizeof(editText) - 1, maxLen);
    strlcpy(editText, current, editMax + 1);
    caps = kind == Kind::Name && editText[0] == '\0';
}

const char* text() { return editText; }

void draw(M5Canvas& cv, int top, const Palette& c) {
    int width = cv.width();
    cv.fillRect(0, top, width, cv.height() - top, c.bg);
    int fx = 40, fy = top + FIELD_DY, fw = width - 80;
    cv.fillRoundRect(fx, fy, fw, FIELD_H, 10, TFT_WHITE);
    cv.drawRoundRect(fx, fy, fw, FIELD_H, 10, c.accent);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    cv.setTextSize(1);
    cv.setTextColor(TFT_BLACK);
    cv.setTextDatum(middle_left);
    cv.drawString(editText, fx + 20, fy + FIELD_H / 2);
    int cx = fx + 20 + cv.textWidth(editText) + 3;
    cv.fillRect(cx, fy + 14, 3, FIELD_H - 28, c.accent);
    cv.setFont(&fonts::FreeSans12pt7b);
    cv.setTextColor(TFT_DARKGREY);
    cv.setTextDatum(middle_right);
    char left[24];
    snprintf(left, sizeof(left), "%d / %d", (int)strlen(editText), editMax);
    cv.drawString(left, fx + fw - 20, fy + FIELD_H / 2);

    Key keys[48];
    int n = layoutKeys(keys, width, top);
    cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextDatum(middle_center);
    for (int i = 0; i < n; i++) {
        char label[8];
        char code = keys[i].code;
        switch (code) {
            case K_DEL: strcpy(label, "DELETE"); break;
            case K_CAPS: strcpy(label, symbols ? "abc" : caps ? "abc" : "ABC"); break;
            case K_SYM: strcpy(label, symbols ? "abc" : "SYM"); break;
            case K_CLEAR: strcpy(label, "CLR"); break;
            case K_CANCEL: strcpy(label, "BACK"); break;
            case K_SAVE: strcpy(label, "SAVE"); break;
            case ' ': strcpy(label, "SPACE"); break;
            default:
                label[0] = caps && isalpha((unsigned char)code) ? toupper((unsigned char)code) : code;
                label[1] = '\0';
        }
        const Key& k = keys[i];
        bool lit = code == K_SAVE;
        cv.fillRoundRect(k.x, k.y, k.w, k.h, 12, lit ? c.accent : c.btn);
        cv.drawRoundRect(k.x, k.y, k.w, k.h, 12, c.dim);
        cv.setTextColor(lit ? c.bg : c.btnText);
        cv.drawString(label, k.x + k.w / 2, k.y + k.h / 2);
    }
}

Result tap(int x, int y, int width, int top) {
    Key keys[48];
    int n = layoutKeys(keys, width, top);
    for (int i = 0; i < n; i++) {
        if (!keys[i].hit(x, y)) continue;
        char code = keys[i].code;
        size_t len = strlen(editText);
        switch (code) {
            case K_DEL:
                if (len) editText[len - 1] = '\0';
                break;
            case K_CAPS:
                if (symbols) symbols = false;
                else caps = !caps;
                break;
            case K_SYM: symbols = !symbols; break;
            case K_CLEAR: editText[0] = '\0'; caps = kind == Kind::Name; break;
            case K_CANCEL: return Result::Cancel;
            case K_SAVE:
                if (kind == Kind::Name)
                    for (int k = (int)len - 1; k >= 0 && editText[k] == ' '; k--) editText[k] = '\0';
                return Result::Save;
            default:
                if ((int)len < editMax &&
                    !(kind == Kind::Name && code == ' ' && (len == 0 || editText[len - 1] == ' '))) {
                    editText[len] = !symbols && caps && isalpha((unsigned char)code) ? toupper((unsigned char)code) : code;
                    editText[len + 1] = '\0';
                    // Names start with one capital, like a phone keyboard. Passwords keep the case.
                    if (kind == Kind::Name && isalpha((unsigned char)code)) caps = false;
                }
        }
        return Result::None;
    }
    return Result::None;
}

}  // namespace osk
