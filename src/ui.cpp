#include "ui.h"

#include <esp_heap_caps.h>
#include <time.h>

#include "bike.h"
#include "display.h"
#include "fonts/fonts_data.h"
#include "net.h"
#include "ride.h"
#include "storage.h"

// Ride screen layout is design C ("big and simple") from the Ride Screen
// Designs canvas: large speed, distance and time cards, one line of
// secondary metrics. Fonts and icons come from tools/makefonts.py.

namespace ui {

static LGFX lcd;

// Drawing target and vertical offset. Normally the panel; screenshots point
// these at a strip sprite so the same code renders the screen in bands.
static lgfx::LovyanGFX* g = &lcd;
static int oy = 0;

// An anti-aliased VLW font read straight from flash
struct SmoothFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  void load(const uint8_t* bytes, size_t len) {
    data.set(bytes, len);
    font.loadFont(&data);
  }
};

static SmoothFont fSpeed, fBig, fMid, fPaused;
static SmoothFont fLabel12, fLabel13, fLabel15;
static SmoothFont fIcon14, fIcon18, fIcon22, fIcon40;

static constexpr int W = 320;
static constexpr int H = 240;
static constexpr int BAR_H = 24;

// Ride screen
static constexpr int SPEED_TOP = BAR_H;
static constexpr int SPEED_H = 114;
static constexpr int SPEED_BASELINE = 122;
static constexpr int SPEED_RIGHT = 233;  // right edge of the speed digits
static constexpr int CARD_Y = 140;
static constexpr int CARD_H = 62;
static constexpr int CARD_W = 151;
static constexpr int CARD_R = 10;
static constexpr int LINE_Y = 206;

// Idle screen
static constexpr int ROW_Y = BAR_H + 12;
static constexpr int ROW_H = 24;
static constexpr int VALUE_X = 92;

enum class Screen { None, Idle, Ride };
static Screen shown = Screen::None;
static bool shownPaused = false;

static uint16_t C_BG, C_CARD, C_LINE, C_TEXT, C_DIM, C_GREEN, C_BLUE, C_VIOLET, C_CYAN, C_TEAL, C_AMBER, C_PINK,
    C_PAUSED, C_BAD;

static void text(const char* s, int x, int y, textdatum_t datum, SmoothFont& f, uint16_t fg, uint16_t bg, int pad = 0) {
  g->setFont(&f.font);
  g->setTextDatum(datum);
  g->setTextColor(fg, bg);
  g->setTextPadding(pad);
  g->drawString(s, x, y - oy);
}

static void fillRect(int x, int y, int w, int h, uint16_t c) { g->fillRect(x, y - oy, w, h, c); }

// ---------------------------------------------------------------- status bar

static void drawBarFrame() {
  fillRect(0, 0, W, BAR_H - 1, C_CARD);
  g->drawFastHLine(0, BAR_H - 1 - oy, W, C_LINE);
}

static void drawBar(const net::Status& ns, const storage::Status& ss, const bike::Data& bd) {
  char buf[32];
  const int mid = BAR_H / 2;

  text(ns.connected ? icon::WIFI : icon::WIFI_OFF, 8, mid, textdatum_t::middle_left, fIcon14,
       ns.connected ? C_GREEN : C_AMBER, C_CARD, 16);
  if (ns.connected) snprintf(buf, sizeof buf, "%d", ns.rssi);
  else snprintf(buf, sizeof buf, ns.configured ? "..." : "off");
  text(buf, 27, mid, textdatum_t::middle_left, fLabel12, ns.connected ? C_GREEN : C_AMBER, C_CARD, 40);

  if (net::timeSynced()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, sizeof buf, "%H:%M %Z", &t);
  } else {
    snprintf(buf, sizeof buf, "--:--");
  }
  text(buf, W / 2, mid, textdatum_t::middle_center, fLabel12, C_TEXT, C_CARD, 90);

  bool linked = bd.link == bike::Link::Connected;
  text(linked ? icon::BLUETOOTH : icon::BLUETOOTH_OFF, 238, mid, textdatum_t::middle_left, fIcon14,
       linked ? C_BLUE : C_DIM, C_CARD, 16);

  if (ss.mounted) snprintf(buf, sizeof buf, "%.1fG", ss.freeBytes / 1e9);
  else snprintf(buf, sizeof buf, "none");
  text(icon::SD, 266, mid, textdatum_t::middle_left, fIcon14, ss.mounted ? C_GREEN : C_BAD, C_CARD, 16);
  text(buf, W - 8, mid, textdatum_t::middle_right, fLabel12, ss.mounted ? C_GREEN : C_BAD, C_CARD, 34);
}

// ---------------------------------------------------------------- ride screen

static int cardX(int col) { return 6 + col * (CARD_W + 6); }

static void drawRideStatic() {
  g->fillSmoothRoundRect(cardX(0), CARD_Y - oy, CARD_W, CARD_H, CARD_R, C_CARD);
  g->fillSmoothRoundRect(cardX(1), CARD_Y - oy, CARD_W, CARD_H, CARD_R, C_CARD);
  const int cy = CARD_Y + CARD_H / 2;
  text(icon::DISTANCE, cardX(0) + 14, cy, textdatum_t::middle_left, fIcon22, C_GREEN, C_CARD);
  text("km", cardX(0) + 118, cy + 9, textdatum_t::baseline_left, fLabel13, C_DIM, C_CARD);
  text(icon::TIME, cardX(1) + 14, cy, textdatum_t::middle_left, fIcon22, C_VIOLET, C_CARD);

  const int ly = LINE_Y + 17;
  text(icon::CADENCE, 14, ly, textdatum_t::middle_left, fIcon14, C_TEAL, C_BG);
  text(icon::POWER, 92, ly, textdatum_t::middle_left, fIcon14, C_AMBER, C_BG);
  text(icon::RESISTANCE, 178, ly, textdatum_t::middle_left, fIcon14, C_PINK, C_BG);
  text(icon::AVERAGE, 242, ly, textdatum_t::middle_left, fIcon14, C_CYAN, C_BG);
}

static void drawSpeed(const bike::Data& bd, bool paused) {
  if (paused != shownPaused) {
    fillRect(0, SPEED_TOP, W, SPEED_H, C_BG);
    shownPaused = paused;
    if (!paused) {
      text(icon::SPEED, SPEED_RIGHT + 6, SPEED_BASELINE - 30, textdatum_t::baseline_left, fIcon18, C_BLUE, C_BG);
      text("km/h", SPEED_RIGHT + 6, SPEED_BASELINE - 4, textdatum_t::baseline_left, fLabel15, C_DIM, C_BG);
    }
  }
  if (paused) {
    const int cy = SPEED_TOP + SPEED_H / 2;
    text(icon::PAUSE, 64, cy, textdatum_t::middle_left, fIcon40, C_PAUSED, C_BG);
    text("PAUSED", 116, cy, textdatum_t::middle_left, fPaused, C_PAUSED, C_BG);
    return;
  }
  char buf[16];
  snprintf(buf, sizeof buf, "%.1f", bd.speedKmh);
  // The padding box spans the font's full line height, which is taller than
  // the speed area; clip it so it doesn't erase the status bar or the cards.
  g->setClipRect(0, SPEED_TOP - oy, SPEED_RIGHT + 4, SPEED_H);
  text(buf, SPEED_RIGHT, SPEED_BASELINE, textdatum_t::baseline_right, fSpeed, C_TEXT, C_BG, SPEED_RIGHT - 4);
  g->clearClipRect();
}

static void drawRide(const bike::Data& bd, const ride::Stats& rs) {
  char buf[24];
  drawSpeed(bd, rs.paused);

  const int cy = CARD_Y + CARD_H / 2;
  snprintf(buf, sizeof buf, "%.2f", rs.distanceM / 1000.0f);
  text(buf, cardX(0) + 114, cy + 9, textdatum_t::baseline_right, fBig, C_TEXT, C_CARD, 72);

  uint32_t s = rs.movingSeconds;
  if (s >= 3600) {
    snprintf(buf, sizeof buf, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
             (unsigned long)(s % 60));
  } else {
    snprintf(buf, sizeof buf, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  }
  text(buf, cardX(1) + 44, cy + 9, textdatum_t::baseline_left, fBig, C_TEXT, C_CARD, 100);

  const int ly = LINE_Y + 17;
  snprintf(buf, sizeof buf, "%u", bd.cadenceRpm);
  text(buf, 32, ly, textdatum_t::middle_left, fMid, C_TEXT, C_BG, 44);
  snprintf(buf, sizeof buf, "%u W", bd.watts);
  text(buf, 110, ly, textdatum_t::middle_left, fMid, C_TEXT, C_BG, 56);
  snprintf(buf, sizeof buf, "%u", bd.resistance);
  text(buf, 196, ly, textdatum_t::middle_left, fMid, C_TEXT, C_BG, 36);
  snprintf(buf, sizeof buf, "%.1f", rs.avgKmh);
  text(buf, 260, ly, textdatum_t::middle_left, fMid, C_TEXT, C_BG, 54);
}

// ---------------------------------------------------------------- idle screen

static void row(int i, const char* label, const char* value, uint16_t color) {
  int y = ROW_Y + i * ROW_H;
  text(label, 10, y, textdatum_t::top_left, fLabel13, C_DIM, C_BG);
  text(value, VALUE_X, y, textdatum_t::top_left, fLabel13, color, C_BG, W - VALUE_X - 4);
}

static void drawIdle(const net::Status& ns, const storage::Status& ss, const bike::Data& bd) {
  char buf[64];
  if (!ns.configured) row(0, "WiFi", "no settings stored", C_BAD);
  else if (ns.connected) row(0, "WiFi", ns.ssid.c_str(), C_GREEN);
  else {
    snprintf(buf, sizeof buf, "connecting to %s", ns.ssid.c_str());
    row(0, "WiFi", buf, C_AMBER);
  }

  row(1, "IP", ns.connected ? ns.ip.c_str() : "-", C_TEXT);

  if (net::timeSynced()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M %Z", &t);
    row(2, "Clock", buf, C_TEXT);
  } else {
    row(2, "Clock", "waiting for NTP", C_AMBER);
  }

  if (ss.mounted) {
    snprintf(buf, sizeof buf, "%s, %.1f GB free of %.1f GB", ss.type, ss.freeBytes / 1e9, ss.totalBytes / 1e9);
    row(3, "SD card", buf, C_GREEN);
  } else {
    row(3, "SD card", "not found", C_BAD);
  }

  snprintf(buf, sizeof buf, "%u KB free, largest %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
  row(4, "Memory", buf, C_TEXT);

  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof buf, "%lud %02lu:%02lu:%02lu", (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
           (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  row(5, "Uptime", buf, C_TEXT);

  if (bd.link == bike::Link::Connected) {
    row(6, "Bike", "connected, start pedalling to ride", C_GREEN);
  } else {
    snprintf(buf, sizeof buf, "%s for I_EB", bike::linkText(bd.link));
    row(6, "Bike", buf, C_AMBER);
  }
  row(7, "Firmware", FW_VERSION, C_DIM);
}

// ---------------------------------------------------------------- screens

static void showScreen(Screen s) {
  if (s == shown) return;
  shown = s;
  fillRect(0, BAR_H, W, H - BAR_H, C_BG);
  if (s == Screen::Ride) {
    shownPaused = true;  // forces drawSpeed to lay out the speed area
    drawRideStatic();
  }
}

void begin() {
  lcd.init();
  lcd.setRotation(1);  // landscape, 320x240
  lcd.setBrightness(255);

  fSpeed.load(font_speed, sizeof font_speed);
  fBig.load(font_big, sizeof font_big);
  fMid.load(font_mid, sizeof font_mid);
  fPaused.load(font_paused, sizeof font_paused);
  fLabel12.load(font_label12, sizeof font_label12);
  fLabel13.load(font_label13, sizeof font_label13);
  fLabel15.load(font_label15, sizeof font_label15);
  fIcon14.load(icons14, sizeof icons14);
  fIcon18.load(icons18, sizeof icons18);
  fIcon22.load(icons22, sizeof icons22);
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

  lcd.fillScreen(C_BG);
  drawBarFrame();
}

struct Snapshot {
  net::Status ns;
  storage::Status ss;
  bike::Data bd;
  ride::Stats rs;
};

static Snapshot snapshot() {
  Snapshot s = {net::status(), storage::status(), bike::get(), ride::stats()};
#ifdef UI_DEMO
  // Sample ride from the design mockups; pauses for 5 s every 10 s
  s.bd.link = bike::Link::Connected;
  s.bd.speedKmh = 18.4f;
  s.bd.cadenceRpm = 56;
  s.bd.watts = 131;
  s.bd.resistance = 6;
  s.rs = {true, (millis() / 5000) % 2 == 1, 21 * 60 + 43 + millis() / 1000, 6270, 17.3f, 22.0f};
#endif
  return s;
}

static void render(const Snapshot& s) {
  drawBar(s.ns, s.ss, s.bd);
  if (s.rs.active) {
    showScreen(Screen::Ride);
    drawRide(s.bd, s.rs);
  } else {
    showScreen(Screen::Idle);
    drawIdle(s.ns, s.ss, s.bd);
  }
}

void draw() { render(snapshot()); }

void screenshot(Print& out) {
  // The panel's read-back returns corrupted pixels on this board, so the
  // screen is re-rendered into a strip sprite one band at a time instead.
  static constexpr int BAND = 40;
  LGFX_Sprite strip(&lcd);
  strip.setColorDepth(16);
  if (!strip.createSprite(W, BAND)) {
    out.print("SHOT 0 0\n");
    return;
  }

  Screen savedShown = shown;
  bool savedPaused = shownPaused;
  const Snapshot snap = snapshot();  // every band shows the same moment
  static uint8_t line[W * 3];        // R, G, B per pixel

  // Rows go out as text lines ("R<y> <base64> <sum>") so log output from
  // other tasks landing in the stream can't corrupt the image.
  static constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static char enc[(sizeof line + 2) / 3 * 4 + 1];

  out.printf("\nSHOT %d %d\n", W, H);
  for (int top = 0; top < H; top += BAND) {
    g = &strip;
    oy = top;
    shown = Screen::None;
    strip.fillScreen(C_BG);
    drawBarFrame();
    render(snap);
    for (int y = 0; y < BAND; y++) {
      for (int x = 0; x < W; x++) {
        auto c = strip.readPixelRGB(x, y);
        line[x * 3] = c.r;
        line[x * 3 + 1] = c.g;
        line[x * 3 + 2] = c.b;
      }
      const uint8_t* b = line;
      uint16_t sum = 0;
      size_t n = 0;
      for (size_t i = 0; i < sizeof line; i += 3) {
        uint32_t v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        sum += b[i] + b[i + 1] + b[i + 2];
        enc[n++] = B64[(v >> 18) & 63];
        enc[n++] = B64[(v >> 12) & 63];
        enc[n++] = B64[(v >> 6) & 63];
        enc[n++] = B64[v & 63];
      }
      enc[n] = '\0';
      out.printf("R%d %s %04x\n", top + y, enc, sum);
    }
  }
  out.print("END\n");

  g = &lcd;
  oy = 0;
  shown = savedShown;
  shownPaused = savedPaused;
  strip.deleteSprite();
}

}  // namespace ui
