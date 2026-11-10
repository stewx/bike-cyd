#pragma once

#include "display.h"
#include "fonts/icons.h"

// The panel, fonts, colours and drawing helpers shared by the screens
// (ui.cpp) and the settings screens (menu.cpp).
namespace draw {

constexpr int W = 320;
constexpr int H = 240;

extern LGFX lcd;

// Drawing target and vertical offset. Normally the panel; screenshots point
// these at a strip sprite so the same code renders the screen in bands.
extern lgfx::LovyanGFX* g;
extern int oy;

// An anti-aliased VLW font read straight from flash
struct SmoothFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  void load(const uint8_t* bytes, size_t len) {
    data.set(bytes, len);
    font.loadFont(&data);
  }
};

extern SmoothFont fSpeed, fBig, fMid, fPaused, fTitle, fText18, fClock, fCard;
extern SmoothFont fLabel12, fLabel13, fLabel15;
extern SmoothFont fIcon14, fIcon18, fIcon20, fIcon22, fIcon26, fIcon40;

extern uint16_t C_BG, C_CARD, C_LINE, C_TEXT, C_DIM, C_GREEN, C_BLUE, C_VIOLET, C_CYAN, C_TEAL, C_AMBER, C_PINK,
    C_PAUSED, C_BAD, C_SOFT, C_OK_BG, C_CONFIRM_BG, C_CONFIRM_SUB, C_BAD_BG;

// Starts the panel and loads the fonts and colours
void begin();

void text(const char* s, int x, int y, textdatum_t datum, SmoothFont& f, uint16_t fg, uint16_t bg, int pad = 0);
int textWidth(const char* s, SmoothFont& f);
void fillRect(int x, int y, int w, int h, uint16_t c);
// Smooth corners blend with the pixels already there (read back from the
// panel); plain ones are quicker for things redrawn often, like keys
void roundRect(int x, int y, int w, int h, int r, uint16_t c, bool smooth = true);
void circle(int x, int y, int r, uint16_t c);
void hline(int x, int y, int w, uint16_t c);
// Copies s to out, cut short with "..." to fit width
void fit(const char* s, int width, SmoothFont& f, char* out, size_t len);
// Draws s (top left at x, y) in lines of at most width; returns the lines used
int wrap(const char* s, int x, int y, int width, int lineH, SmoothFont& f, uint16_t fg, uint16_t bg, int maxLines);
void clip(int x, int y, int w, int h);
void noClip();

}  // namespace draw
