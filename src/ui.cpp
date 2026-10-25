#include "ui.h"

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "bike.h"
#include "display.h"
#include "fonts/fonts_data.h"
#include "logger.h"
#include "nas.h"
#include "net.h"
#include "ride.h"
#include "storage.h"
#include "tcx.h"
#include "touch.h"

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

static SmoothFont fSpeed, fBig, fMid, fPaused, fTitle, fText18;
static SmoothFont fLabel12, fLabel13, fLabel15;
static SmoothFont fIcon14, fIcon18, fIcon20, fIcon22, fIcon26, fIcon40;

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
static constexpr int ROW_Y = BAR_H + 10;
static constexpr int ROW_H = 22;
static constexpr int VALUE_X = 92;

// Paused screen (design P1): header, two cards, one big button
static constexpr int P_CARD_Y = 92;
static constexpr int P_CARD_H = 50;
static constexpr int P_CARD_W = 148;
static constexpr int BTN_X = 8;
static constexpr int BTN_Y = 150;
static constexpr int BTN_W = 304;
static constexpr int BTN_H = 72;
static constexpr uint32_t CONFIRM_MS = 5000;

// Ride saved screen
static constexpr uint32_t SAVED_SHOW_MS = 3 * 60 * 1000;

enum class Screen { None, Idle, Ride, Paused, Saved };
static Screen shown = Screen::None;
static uint32_t lastActivity = 0;  // for the backlight timeout
static bool shownPaused = false;

static bool confirming = false;    // paused screen: first tap done, waiting for the second
static uint32_t confirmUntil = 0;
static int drawnButton = -1;       // which button face is on screen (-1 none, 0 end, 1 confirm)
static uint32_t dismissedFor = 0;  // endedMs of the ride whose saved screen was tapped away
static int drawnSavedState = -1;

static uint16_t C_BG, C_CARD, C_LINE, C_TEXT, C_DIM, C_GREEN, C_BLUE, C_VIOLET, C_CYAN, C_TEAL, C_AMBER, C_PINK,
    C_PAUSED, C_BAD, C_SOFT, C_OK_BG, C_CONFIRM_BG, C_CONFIRM_SUB;

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

static void drawBar(const net::Status& ns, const storage::Status& ss, const bike::Data& bd,
                    const nas::Status& us) {
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

  const char* cloud = icon::CLOUD_OFF;
  uint16_t cloudColor = C_DIM;
  switch (us.state) {
    case nas::State::Idle:
      cloud = us.failed ? icon::CLOUD_ALERT : icon::CLOUD_OK;
      cloudColor = us.failed ? C_AMBER : C_GREEN;
      break;
    case nas::State::Uploading: cloud = icon::CLOUD_UP; cloudColor = C_BLUE; break;
    case nas::State::Waiting: cloud = icon::CLOUD_ALERT; cloudColor = C_AMBER; break;
    default: break;
  }
  if (us.pending && us.state != nas::State::Uploading && us.state != nas::State::Waiting) {
    cloud = icon::CLOUD_UP;
  }
  text(cloud, 214, mid, textdatum_t::middle_left, fIcon14, cloudColor, C_CARD, 16);

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

  // Recording indicator in the top right corner of the speed area
  g->fillSmoothCircle(W - 14, SPEED_TOP + 14 - oy, 5, rs.recording ? C_BAD : C_BG);

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

static void formatDuration(char* buf, size_t len, uint32_t s) {
  if (s >= 3600) {
    snprintf(buf, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
             (unsigned long)(s % 60));
  } else {
    snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  }
}

// ---------------------------------------------------------------- paused screen

static int pCardX(int col) { return 8 + col * (P_CARD_W + 8); }

static void drawPausedStatic() {
  // Header: pause icon and PAUSED, centred, with a hint below
  g->setFont(&fTitle.font);
  int tw = g->textWidth("PAUSED");
  int x0 = W / 2 - (26 + 10 + tw) / 2;
  text(icon::PAUSE, x0, 54, textdatum_t::middle_left, fIcon26, C_PAUSED, C_BG);
  text("PAUSED", x0 + 36, 54, textdatum_t::middle_left, fTitle, C_PAUSED, C_BG);
  text("Pedal to resume", W / 2, 80, textdatum_t::middle_center, fLabel12, C_DIM, C_BG);

  for (int col = 0; col < 2; col++) {
    g->fillSmoothRoundRect(pCardX(col), P_CARD_Y - oy, P_CARD_W, P_CARD_H, 10, C_CARD);
  }
  const int cy = P_CARD_Y + P_CARD_H / 2;
  text(icon::DISTANCE, pCardX(0) + 12, cy, textdatum_t::middle_left, fIcon22, C_GREEN, C_CARD);
  text("km", pCardX(0) + 114, cy + 9, textdatum_t::baseline_left, fLabel13, C_DIM, C_CARD);
  text(icon::TIME, pCardX(1) + 12, cy, textdatum_t::middle_left, fIcon22, C_VIOLET, C_CARD);
  drawnButton = -1;
}

static void drawButton(bool confirm) {
  const int cy = BTN_Y + BTN_H / 2;
  if (!confirm) {
    g->fillSmoothRoundRect(BTN_X, BTN_Y - oy, BTN_W, BTN_H, 14, C_GREEN);
    const char* label = "End ride & upload";
    g->setFont(&fTitle.font);
    int x0 = W / 2 - (26 + 12 + g->textWidth(label)) / 2;
    text(icon::FLAG, x0, cy, textdatum_t::middle_left, fIcon26, C_BG, C_GREEN);
    text(label, x0 + 38, cy, textdatum_t::middle_left, fTitle, C_BG, C_GREEN);
  } else {
    g->fillSmoothRoundRect(BTN_X, BTN_Y - oy, BTN_W, BTN_H, 14, C_PAUSED);
    g->fillSmoothRoundRect(BTN_X + 2, BTN_Y + 2 - oy, BTN_W - 4, BTN_H - 4, 12, C_CONFIRM_BG);
    text("Tap again to end ride", W / 2, cy - 9, textdatum_t::middle_center, fTitle, C_PAUSED, C_CONFIRM_BG);
    text("Cancels by itself in 5 s", W / 2, cy + 18, textdatum_t::middle_center, fLabel12, C_CONFIRM_SUB,
         C_CONFIRM_BG);
  }
  drawnButton = confirm ? 1 : 0;
}

static void drawPaused(const ride::Stats& rs) {
  char buf[24];
  const int cy = P_CARD_Y + P_CARD_H / 2;
  // The 36 px font's padding box is taller than these cards; keep it inside
  g->setClipRect(0, P_CARD_Y + 4 - oy, W, P_CARD_H - 8);
  snprintf(buf, sizeof buf, "%.2f", rs.distanceM / 1000.0f);
  text(buf, pCardX(0) + 110, cy + 9, textdatum_t::baseline_right, fBig, C_TEXT, C_CARD, 70);
  formatDuration(buf, sizeof buf, rs.movingSeconds);
  text(buf, pCardX(1) + 42, cy + 9, textdatum_t::baseline_left, fBig, C_TEXT, C_CARD, 100);
  g->clearClipRect();

  if (confirming && (int32_t)(millis() - confirmUntil) > 0) confirming = false;
  if (drawnButton != (confirming ? 1 : 0)) drawButton(confirming);
}

// ---------------------------------------------------------------- ride saved screen

enum class Saved { Saving, Uploading, Uploaded, Offline, TooShort, Failed, NotRecorded };

static Saved savedState(const ride::Summary& last, const nas::Status& us, tcx::Result& result) {
  if (!last.csvName[0]) return Saved::NotRecorded;
  result = tcx::lastResult();
  if (strcmp(result.csv, last.csvName) != 0 || result.outcome == tcx::Outcome::None) return Saved::Saving;
  if (result.outcome == tcx::Outcome::TooShort) return Saved::TooShort;
  if (result.outcome == tcx::Outcome::Failed) return Saved::Failed;
  char uploaded[48];
  nas::lastUploaded(uploaded, sizeof uploaded);
  if (strcmp(uploaded, result.tcx) == 0) return Saved::Uploaded;
  if (us.state == nas::State::Waiting || us.state == nas::State::Offline ||
      us.state == nas::State::NotConfigured) {
    return Saved::Offline;
  }
  return Saved::Uploading;
}

static void drawSaved(const ride::Summary& last, const nas::Status& us) {
  tcx::Result result = {};
  Saved state = savedState(last, us, result);
  if ((int)state == drawnSavedState) return;
  drawnSavedState = (int)state;

  const bool kept = state == Saved::Saving || state == Saved::Uploading || state == Saved::Uploaded ||
                    state == Saved::Offline;
  fillRect(0, BAR_H, W, H - BAR_H, C_BG);

  // Big check (or a warning) and the title
  g->fillSmoothCircle(W / 2, 66 - oy, 26, kept ? C_OK_BG : C_CONFIRM_BG);
  text(kept ? icon::CHECK : icon::CLOUD_ALERT, W / 2, 66, textdatum_t::middle_center, kept ? fIcon26 : fIcon20,
       kept ? C_GREEN : C_AMBER, kept ? C_OK_BG : C_CONFIRM_BG);
  text(kept ? "Ride saved" : "Ride ended", W / 2, 112, textdatum_t::middle_center, fTitle, C_TEXT, C_BG);

  char buf[64], dur[16];
  formatDuration(dur, sizeof dur, last.movingSeconds);
  snprintf(buf, sizeof buf, "%.2f km \xC2\xB7 %s \xC2\xB7 %.1f km/h avg", last.distanceM / 1000.0f, dur, last.avgKmh);
  text(buf, W / 2, 140, textdatum_t::middle_center, fText18, C_SOFT, C_BG);

  // Upload status card
  const int cardY = 158, cardH = 54, cy = cardY + cardH / 2;
  g->fillSmoothRoundRect(12, cardY - oy, W - 24, cardH, 10, C_CARD);
  const char* ic = icon::CLOUD_UP;
  uint16_t color = C_BLUE;
  const char* line1 = "";
  String line2;
  switch (state) {
    case Saved::Saving: line1 = "Saving the ride..."; break;
    case Saved::Uploading:
      line1 = "Uploading to the NAS...";
      line2 = result.tcx;
      break;
    case Saved::Uploaded:
      ic = icon::CLOUD_OK;
      color = C_GREEN;
      line1 = "On the NAS";
      line2 = String("BikeRides\\") + result.tcx;
      break;
    case Saved::Offline:
      ic = icon::CLOUD_ALERT;
      color = C_AMBER;
      line1 = "NAS not reachable";
      line2 = "Saved on the SD card, uploads when it's back";
      break;
    case Saved::TooShort:
      ic = icon::CLOUD_ALERT;
      color = C_AMBER;
      line1 = "Too short to keep";
      line2 = "Rides need 2 minutes and 0.5 km";
      break;
    case Saved::Failed:
      ic = icon::CLOUD_ALERT;
      color = C_BAD;
      line1 = "Couldn't save the ride";
      line2 = "Its data is kept on the SD card";
      break;
    case Saved::NotRecorded:
      ic = icon::CLOUD_ALERT;
      color = C_BAD;
      line1 = "Not recorded";
      line2 = "No SD card was found";
      break;
  }
  text(ic, 24, cy, textdatum_t::middle_left, fIcon20, color, C_CARD);
  if (line2.length()) {
    text(line1, 54, cy - 9, textdatum_t::middle_left, fLabel15, color, C_CARD);
    text(line2.c_str(), 54, cy + 11, textdatum_t::middle_left, fLabel12, C_DIM, C_CARD);
  } else {
    text(line1, 54, cy, textdatum_t::middle_left, fLabel15, color, C_CARD);
  }
}

// ---------------------------------------------------------------- idle screen

static void row(int i, const char* label, const char* value, uint16_t color) {
  int y = ROW_Y + i * ROW_H;
  text(label, 10, y, textdatum_t::top_left, fLabel13, C_DIM, C_BG);
  text(value, VALUE_X, y, textdatum_t::top_left, fLabel13, color, C_BG, W - VALUE_X - 4);
}

static void drawIdle(const net::Status& ns, const storage::Status& ss, const bike::Data& bd,
                     const nas::Status& us) {
  char buf[64];
  if (!ns.configured) {
    row(0, "WiFi", "no settings stored", C_BAD);
  } else if (ns.connected) {
    snprintf(buf, sizeof buf, "%s, %s", ns.ssid.c_str(), ns.ip.c_str());
    row(0, "WiFi", buf, C_GREEN);
  } else {
    snprintf(buf, sizeof buf, "connecting to %s", ns.ssid.c_str());
    row(0, "WiFi", buf, C_AMBER);
  }

  // Live readings, for calibrating the light sensor and checking touch
  snprintf(buf, sizeof buf, "light %d, touch %s", analogRead(PIN_LDR), touch::touched() ? "yes" : "no");
  row(1, "Sensors", buf, C_TEXT);

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
  } else if (bd.link == bike::Link::Resting) {
    row(6, "Bike", "idle, letting the console sleep", C_DIM);
  } else {
    snprintf(buf, sizeof buf, "%s for I_EB", bike::linkText(bd.link));
    row(6, "Bike", buf, C_AMBER);
  }

  if (us.state == nas::State::Waiting && us.lastError[0]) {
    snprintf(buf, sizeof buf, "%s", us.lastError);
  } else if (us.pending) {
    snprintf(buf, sizeof buf, "%s, %d to upload", nas::stateText(us.state), us.pending);
  } else if (us.failed) {
    snprintf(buf, sizeof buf, "%s, %d failed", nas::stateText(us.state), us.failed);
  } else {
    snprintf(buf, sizeof buf, "%s", nas::stateText(us.state));
  }
  bool ok = us.state == nas::State::Idle && !us.failed;
  row(7, "NAS", buf, ok ? C_GREEN : us.state == nas::State::Uploading ? C_BLUE : C_AMBER);
  row(8, "Firmware", FW_VERSION, C_DIM);
}

// ---------------------------------------------------------------- screens

static void showScreen(Screen s) {
  if (s == shown) return;
  shown = s;
  fillRect(0, BAR_H, W, H - BAR_H, C_BG);
  if (s == Screen::Ride) {
    shownPaused = true;  // forces drawSpeed to lay out the speed area
    drawRideStatic();
  } else if (s == Screen::Paused) {
    confirming = false;
    drawPausedStatic();
  } else if (s == Screen::Saved) {
    drawnSavedState = -1;
  }
}

void begin() {
  lcd.init();
  lcd.setRotation(1);  // landscape, 320x240
  lcd.setBrightness(255);
  lastActivity = millis();

  fSpeed.load(font_speed, sizeof font_speed);
  fBig.load(font_big, sizeof font_big);
  fMid.load(font_mid, sizeof font_mid);
  fPaused.load(font_paused, sizeof font_paused);
  fTitle.load(font_title, sizeof font_title);
  fText18.load(font_text18, sizeof font_text18);
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

  lcd.fillScreen(C_BG);
  drawBarFrame();
}

struct Snapshot {
  net::Status ns;
  storage::Status ss;
  bike::Data bd;
  ride::Stats rs;
  nas::Status us;
};

static Snapshot snapshot() {
  Snapshot s = {net::status(), storage::status(), bike::get(), ride::stats(), nas::status()};
#ifdef UI_DEMO
  // Sample ride from the design mockups; pauses for 5 s every 10 s
  s.bd.link = bike::Link::Connected;
  s.bd.speedKmh = 18.4f;
  s.bd.cadenceRpm = 56;
  s.bd.watts = 131;
  s.bd.resistance = 6;
  s.rs = {true, (millis() / 5000) % 2 == 1, true, 21 * 60 + 43 + millis() / 1000, 6270, 17.3f, 22.0f};
#endif
  return s;
}

static void render(const Snapshot& s) {
  drawBar(s.ns, s.ss, s.bd, s.us);
  const ride::Summary last = ride::lastRide();
  if (s.rs.active && s.rs.paused) {
    showScreen(Screen::Paused);
    drawPaused(s.rs);
  } else if (s.rs.active) {
    showScreen(Screen::Ride);
    drawRide(s.bd, s.rs);
  } else if (last.valid && last.endedMs != dismissedFor && millis() - last.endedMs < SAVED_SHOW_MS) {
    showScreen(Screen::Saved);
    drawSaved(last, s.us);
  } else {
    showScreen(Screen::Idle);
    drawIdle(s.ns, s.ss, s.bd, s.us);
  }
}

// ---------------------------------------------------------------- backlight

static constexpr uint32_t FULL_MS = 2 * 60 * 1000;
static constexpr uint32_t DIM_MS = 30 * 60 * 1000;
static constexpr uint8_t FULL = 255;
static constexpr uint8_t DIMMED = 40;
static constexpr uint8_t NIGHT_FULL = 80;     // "full" brightness in a dark room
static constexpr float DARK_READING = 600;    // light sensor value counted as dark
static int brightness = -1;

void wake() { lastActivity = millis(); }

static void updateBacklight(const Snapshot& s) {
  static bool wasLinked = false;
  bool linked = s.bd.link == bike::Link::Connected;
  // Riding, or the console waking up (it connects when someone gets on)
  if (s.rs.active || (linked && !wasLinked)) wake();
  wasLinked = linked;

  // The light sensor reads 0 in any daylight and up to ~1000 in the dark, so
  // it only matters at night: full brightness fades to ~30% as it gets dark.
  static float ambient = 0;
  ambient += (analogRead(PIN_LDR) - ambient) * 0.1f;  // smooth over ~10 s
  float darkness = constrain(ambient / DARK_READING, 0.0f, 1.0f);
  int full = FULL - (int)(darkness * (FULL - NIGHT_FULL));

  uint32_t idle = millis() - lastActivity;
  int level = idle < FULL_MS ? full : idle < DIM_MS ? min((int)DIMMED, full) : 0;
  if (level != brightness) {
    lcd.setBrightness(level);
    brightness = level;
  }
}

void draw() {
  Snapshot s = snapshot();
  updateBacklight(s);
  // A ride just ended: have the uploader export and upload it right away
  static uint32_t kickedFor = 0;
  const ride::Summary last = ride::lastRide();
  if (last.valid && last.endedMs != kickedFor) {
    kickedFor = last.endedMs;
    nas::kick();
  }
  render(s);
}

// ---------------------------------------------------------------- touch

void press(bool havePosition, int x, int y) {
  bool wasDark = millis() - lastActivity >= FULL_MS;
  wake();
  if (wasDark) return;  // a touch on a dimmed screen only wakes it

  if (shown == Screen::Saved) {
    dismissedFor = ride::lastRide().endedMs;
    draw();
  } else if (shown == Screen::Paused && havePosition) {
    bool onButton = x >= BTN_X - 4 && x < BTN_X + BTN_W + 4 && y >= BTN_Y - 10 && y < BTN_Y + BTN_H + 10;
    if (!onButton) return;
    if (!confirming) {
      confirming = true;
      confirmUntil = millis() + CONFIRM_MS;
      drawButton(true);
    } else {
      confirming = false;
      ride::endNow();
      draw();
    }
  }
}

static void drawTarget(int x, int y) {
  lcd.drawCircle(x, y, 14, C_GREEN);
  lcd.drawCircle(x, y, 13, C_GREEN);
  lcd.fillCircle(x, y, 3, C_GREEN);
  lcd.drawFastHLine(x - 22, y, 44, C_GREEN);
  lcd.drawFastVLine(x, y - 22, 44, C_GREEN);
}

// Waits for one firm press and returns its averaged raw reading
static bool waitForPress(uint16_t raw[2]) {
  uint32_t start = millis();
  while (touch::touched()) {
    esp_task_wdt_reset();
    delay(10);
  }
  while (!touch::touched()) {
    if (millis() - start > 30000) return false;
    esp_task_wdt_reset();
    delay(10);
  }
  delay(80);  // let the reading settle
  uint32_t sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < 12; i++) {
    uint16_t rx, ry;
    if (touch::readRaw(rx, ry)) {
      sx += rx;
      sy += ry;
      n++;
    }
    delay(10);
  }
  while (touch::touched()) {
    esp_task_wdt_reset();
    delay(10);
  }
  if (n < 4) return false;
  raw[0] = sx / n;
  raw[1] = sy / n;
  return true;
}

bool calibrateTouch() {
  static const int targets[3][2] = {{30, 54}, {290, 54}, {290, 210}};
  uint16_t raw[3][2];
  lcd.setBrightness(FULL);
  brightness = FULL;
  bool ok = true;
  for (int i = 0; i < 3 && ok; i++) {
    lcd.fillScreen(C_BG);
    text("Touch calibration", W / 2, 112, textdatum_t::middle_center, fTitle, C_TEXT, C_BG);
    char buf[48];
    snprintf(buf, sizeof buf, "Press the green target firmly (%d of 3)", i + 1);
    text(buf, W / 2, 142, textdatum_t::middle_center, fLabel13, C_DIM, C_BG);
    drawTarget(targets[i][0], targets[i][1]);
    ok = waitForPress(raw[i]);
    logger::printf("ui: calibration point %d raw %u,%u%s\n", i + 1, raw[i][0], raw[i][1], ok ? "" : " (timed out)");
  }
  if (ok) ok = touch::calibrate(raw, targets);
  lcd.fillScreen(C_BG);
  text(ok ? "Touch calibrated" : "Calibration failed", W / 2, 120, textdatum_t::middle_center, fTitle,
       ok ? C_GREEN : C_BAD, C_BG);
  logger::printf("ui: touch calibration %s\n", ok ? "saved" : "failed");
  delay(1500);
  lcd.fillScreen(C_BG);
  drawBarFrame();
  shown = Screen::None;
  wake();
  return ok;
}

void otaProgress(int percent) {
  static int shownPercent = -2;
  if (percent < 0) {
    // Update failed: redraw the normal screen
    shown = Screen::None;
    shownPercent = -2;
    return;
  }
  if (percent == shownPercent) return;
  if (shownPercent == -2) {
    lcd.setBrightness(FULL);
    brightness = FULL;
    fillRect(0, BAR_H, W, H - BAR_H, C_BG);
    text("Updating firmware", W / 2, 110, textdatum_t::middle_center, fLabel15, C_TEXT, C_BG);
  }
  shownPercent = percent;
  char buf[8];
  snprintf(buf, sizeof buf, "%d%%", percent);
  text(buf, W / 2, 150, textdatum_t::middle_center, fBig, C_BLUE, C_BG, 120);
}

void screenshot(Print& out) {
  // The panel's read-back returns corrupted pixels on this board, so the
  // screen is re-rendered into a strip sprite one band at a time instead.
  // 20 rows = 12.8 KB, small enough to find even right after an upload
  static constexpr int BAND = 20;
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
