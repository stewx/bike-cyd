#include "draw.h"

#include "fonts/fonts_data.h"

namespace draw {

LGFX lcd;
lgfx::LovyanGFX* g = &lcd;
int oy = 0;

SmoothFont fSpeed, fBig, fMid, fPaused, fTitle, fText18, fClock, fCard;
SmoothFont fLabel12, fLabel13, fLabel15;
SmoothFont fIcon14, fIcon18, fIcon20, fIcon22, fIcon26, fIcon40;

uint16_t C_BG, C_CARD, C_LINE, C_TEXT, C_DIM, C_GREEN, C_BLUE, C_VIOLET, C_CYAN, C_TEAL, C_AMBER, C_PINK, C_PAUSED,
    C_BAD, C_SOFT, C_OK_BG, C_CONFIRM_BG, C_CONFIRM_SUB, C_BAD_BG;

void begin() {
  lcd.init();
  lcd.setRotation(1);  // landscape, 320x240
  lcd.setBrightness(255);

  fSpeed.load(font_speed, sizeof font_speed);
  fBig.load(font_big, sizeof font_big);
  fMid.load(font_mid, sizeof font_mid);
  fPaused.load(font_paused, sizeof font_paused);
  fTitle.load(font_title, sizeof font_title);
  fText18.load(font_text18, sizeof font_text18);
  fClock.load(font_clock, sizeof font_clock);
  fCard.load(font_card, sizeof font_card);
  fLabel12.load(font_label12, sizeof font_label12);
  fLabel13.load(font_label13, sizeof font_label13);
  fLabel15.load(font_label15, sizeof font_label15);
  fIcon14.load(icons14, sizeof icons14);
  fIcon18.load(icons18, sizeof icons18);
  fIcon20.load(icons20, sizeof icons20);
  fIcon22.load(icons22, sizeof icons22);
  fIcon26.load(icons26, sizeof icons26);
  fIcon40.load(icons40, sizeof icons40);

  C_BG = lcd.color565(0x07, 0x08, 0x0d);
  C_CARD = lcd.color565(0x11, 0x13, 0x1c);
  C_LINE = lcd.color565(0x1f, 0x22, 0x30);
  C_TEXT = lcd.color565(0xf3, 0xf4, 0xf8);
  C_DIM = lcd.color565(0x8a, 0x90, 0xa2);
  C_GREEN = lcd.color565(0x3d, 0xdc, 0x97);
  C_BLUE = lcd.color565(0x4d, 0xa3, 0xff);
  C_VIOLET = lcd.color565(0xb1, 0x8c, 0xff);
  C_CYAN = lcd.color565(0x4d, 0xd4, 0xff);
  C_TEAL = lcd.color565(0x2e, 0xd3, 0xc6);
  C_AMBER = lcd.color565(0xff, 0xb5, 0x47);
  C_PINK = lcd.color565(0xff, 0x6b, 0x9a);
  C_PAUSED = lcd.color565(0xff, 0xc5, 0x3d);
  C_BAD = lcd.color565(0xff, 0x5c, 0x5c);
  C_SOFT = lcd.color565(0xc9, 0xcd, 0xd8);
  C_OK_BG = lcd.color565(0x17, 0x3a, 0x2b);
  C_CONFIRM_BG = lcd.color565(0x2a, 0x21, 0x0b);
  C_CONFIRM_SUB = lcd.color565(0xc9, 0xa7, 0x5a);
  C_BAD_BG = lcd.color565(0x2e, 0x10, 0x12);
}

void text(const char* s, int x, int y, textdatum_t datum, SmoothFont& f, uint16_t fg, uint16_t bg, int pad) {
  g->setFont(&f.font);
  g->setTextDatum(datum);
  g->setTextColor(fg, bg);
  g->setTextPadding(pad);
  g->drawString(s, x, y - oy);
}

int textWidth(const char* s, SmoothFont& f) {
  g->setFont(&f.font);
  return g->textWidth(s);
}

void fillRect(int x, int y, int w, int h, uint16_t c) { g->fillRect(x, y - oy, w, h, c); }

void roundRect(int x, int y, int w, int h, int r, uint16_t c, bool smooth) {
  if (smooth) g->fillSmoothRoundRect(x, y - oy, w, h, r, c);
  else g->fillRoundRect(x, y - oy, w, h, r, c);
}

void circle(int x, int y, int r, uint16_t c) { g->fillSmoothCircle(x, y - oy, r, c); }

void hline(int x, int y, int w, uint16_t c) { g->drawFastHLine(x, y - oy, w, c); }

void clip(int x, int y, int w, int h) { g->setClipRect(x, y - oy, w, h); }

void noClip() { g->clearClipRect(); }

// Copies s to out, cut short with "..." to fit width
void fit(const char* s, int width, SmoothFont& f, char* out, size_t len) {
  strlcpy(out, s, len);
  for (int keep = strlen(out) - 1; keep > 0 && textWidth(out, f) > width; keep--) {
    snprintf(out, len, "%.*s...", keep, s);
  }
}

// Draws s in lines of at most width; returns how many lines it took
int wrap(const char* s, int x, int y, int width, int lineH, SmoothFont& f, uint16_t fg, uint16_t bg,
         int maxLines) {
  char line[96];
  int lines = 0;
  while (*s && lines < maxLines) {
    while (*s == ' ') s++;
    size_t take = 0;
    for (size_t end = 0;;) {
      while (s[end] && s[end] != ' ') end++;  // the end of the next word
      if (end >= sizeof line) break;
      memcpy(line, s, end);
      line[end] = '\0';
      if (take && textWidth(line, f) > width) break;
      take = end;
      if (!s[end]) break;
      end++;
    }
    if (!take) take = strnlen(s, sizeof line - 1);
    memcpy(line, s, take);
    line[take] = '\0';
    text(line, x, y + lines * lineH, textdatum_t::top_left, f, fg, bg);
    s += take;
    lines++;
  }
  return lines;
}

}  // namespace draw
