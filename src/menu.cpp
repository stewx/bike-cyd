#include "menu.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <time.h>

#include <algorithm>

#include "bike.h"
#include "board.h"
#include "draw.h"
#include "logger.h"
#include "nas.h"
#include "net.h"
#include "settings.h"
#include "storage.h"
#include "touch.h"

// Each page is drawn in full when it opens (drawPage) and its changing
// parts are refreshed once a second (refresh). A tap acts as soon as the
// touch position is steady, without waiting for the finger to lift.

namespace menu {

using namespace draw;

static constexpr uint32_t IDLE_MS = 2 * 60 * 1000;  // back to the home screen after this long untouched
static constexpr uint32_t CONFIRM_MS = 5000;        // "tap again" buttons give up after this
static constexpr uint32_t SCAN_STALE_MS = 30000;    // the WiFi page scans again if its list is older
static constexpr uint32_t STEADY_MS = 150;          // a tap acts this long after it lands, at the latest
static constexpr int STEADY_PX = 12;                // two readings this close count as steady
static constexpr uint32_t REPEAT_DELAY_MS = 500;    // a held backspace starts repeating after this
static constexpr uint32_t REPEAT_MS = 90;

// Layout
static constexpr int HEADER_H = 42;
static constexpr int ROW_Y = 46;  // option rows (bike and screen pages)
static constexpr int ROW_H = 38;
static constexpr int STATUS_Y = 56;  // status line under the header
static constexpr int LIST_TOP = 68;  // scrolling lists, and the NAS page's rows
static constexpr int LIST_ROW_H = 42;
static constexpr int LIST_ROWS = 4;
static constexpr int LIST_W = 268;  // scroll buttons to the right of this
static constexpr int TILE_W = 151;
static constexpr int TILE_H = 58;

enum class Page { Calibrate, Main, Wifi, Join, Bike, Screen, Clock, Nas, Device, Diagnostics, Picker, Keyboard };

static bool opened = false;
static Page page = Page::Main;
static uint32_t lastTouch = 0;
static bool waitRelease = false;  // the tap that opened the menu hasn't lifted yet

// Text last drawn in each changing spot, so refresh() only redraws changes
static char drawn[10][112];

static bool changed(int slot, const char* s) {
  if (strcmp(drawn[slot], s) == 0) return false;
  strlcpy(drawn[slot], s, sizeof drawn[slot]);
  return true;
}

static bool inside(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void drawPage();
static void go(Page p);

// ---------------------------------------------------------------- options

enum class Opt { BikeName, Release, RideEnd, MinSec, MinM, Full, Dim, Off, Night, Saved };

struct OptInfo {
  const char* label;
  const char* help;
  uint16_t choices[8];
};

static const OptInfo OPTS[] = {
    {"Bluetooth name", nullptr, {}},
    {"Let the bike sleep after",
     "Disconnects from the bike after this long without pedalling, so the console can go to sleep.",
     {1, 2, 3, 5, 10, 15, 30, 60}},
    {"End a paused ride after", "A paused ride is saved and uploaded once it has been paused this long.",
     {1, 2, 3, 5, 10, 15, 30, 60}},
    {"Shortest ride kept", "Rides with less riding time than this aren't saved.",
     {0, 30, 60, 120, 180, 300, 600, 900}},
    {"Shortest distance kept", "Rides shorter than this aren't saved.", {0, 100, 250, 500, 1000, 2000, 3000, 5000}},
    {"Full brightness for",
     "After a touch, or while riding, the screen stays at full brightness this long before it dims.",
     {1, 2, 3, 5, 10, 15, 30, 60}},
    {"Dimmed brightness", "How bright the screen is once it has dimmed.", {5, 10, 16, 25, 35, 50, 75, 100}},
    {"Screen off after", "The screen turns off this long after the last activity. A touch wakes it.",
     {0, 5, 10, 15, 30, 60, 120, 240}},
    {"Night dimming", nullptr, {}},
    {"Ride summary for", "How long the Ride saved screen stays up after a ride ends.", {1, 2, 3, 5, 10, 15, 30, 60}},
};

static const OptInfo& info(Opt o) { return OPTS[(int)o]; }

static long valueOf(const settings::Values& v, Opt o) {
  switch (o) {
    case Opt::Release: return v.releaseMin;
    case Opt::RideEnd: return v.rideEndMin;
    case Opt::MinSec: return v.minRideSec;
    case Opt::MinM: return v.minRideM;
    case Opt::Full: return v.fullMin;
    case Opt::Dim: return v.dimPct;
    case Opt::Off: return v.offMin;
    case Opt::Night: return v.nightDim;
    case Opt::Saved: return v.savedMin;
    default: return 0;
  }
}

static void setValue(settings::Values& v, Opt o, long n) {
  switch (o) {
    case Opt::Release: v.releaseMin = n; break;
    case Opt::RideEnd: v.rideEndMin = n; break;
    case Opt::MinSec: v.minRideSec = n; break;
    case Opt::MinM: v.minRideM = n; break;
    case Opt::Full: v.fullMin = n; break;
    case Opt::Dim: v.dimPct = n; break;
    case Opt::Off: v.offMin = n; break;
    case Opt::Night: v.nightDim = n; break;
    case Opt::Saved: v.savedMin = n; break;
    default: break;
  }
}

static void formatValue(Opt o, long n, char* buf, size_t len) {
  switch (o) {
    case Opt::MinSec:
      if (n == 0) snprintf(buf, len, "Any");
      else if (n < 60) snprintf(buf, len, "%ld s", n);
      else if (n % 60 == 0) snprintf(buf, len, "%ld min", n / 60);
      else snprintf(buf, len, "%.1f min", n / 60.0f);
      break;
    case Opt::MinM:
      if (n == 0) snprintf(buf, len, "Any");
      else if (n % 1000 == 0) snprintf(buf, len, "%ld km", n / 1000);
      else if (n % 100 == 0) snprintf(buf, len, "%.1f km", n / 1000.0f);
      else snprintf(buf, len, "%.2f km", n / 1000.0f);
      break;
    case Opt::Dim: snprintf(buf, len, "%ld%%", n); break;
    case Opt::Night: snprintf(buf, len, n ? "On" : "Off"); break;
    default:
      if (o == Opt::Off && n == 0) snprintf(buf, len, "Never");
      else if (n >= 120 && n % 60 == 0) snprintf(buf, len, "%ld h", n / 60);
      else snprintf(buf, len, "%ld min", n);
      break;
  }
}

static char saveError[64];

static bool save(const settings::Values& v) {
  const settings::Values old = settings::get();
  if (!settings::set(v, saveError, sizeof saveError)) return false;
  if (strcmp(old.timeZone, v.timeZone) != 0) net::setTimeZone();
  logger::printf("menu: settings saved\n");
  return true;
}

// ---------------------------------------------------------------- drawing helpers

static void header(const char* title, const char* action = nullptr, uint16_t actionColor = 0) {
  fillRect(0, 0, W, HEADER_H, C_BG);
  roundRect(6, 5, 40, 32, 8, C_CARD);
  text(icon::BACK, 26, 21, textdatum_t::middle_center, fIcon18, C_TEXT, C_CARD);
  char buf[48];
  fit(title, W - (action ? 116 : 66), fText18, buf, sizeof buf);
  text(buf, 58, 21, textdatum_t::middle_left, fText18, C_TEXT, C_BG);
  if (action) {
    roundRect(W - 46, 5, 40, 32, 8, C_CARD, false);
    text(action, W - 26, 21, textdatum_t::middle_center, fIcon18, actionColor ? actionColor : C_TEXT, C_CARD);
  }
  hline(0, HEADER_H, W, C_LINE);
}

static bool backHit(int x, int y) { return x < 64 && y < HEADER_H + 4; }
static bool actionHit(int x, int y) { return x >= W - 64 && y < HEADER_H + 4; }

// One line of status under the header, redrawn only when it changes
static void statusLine(int slot, const char* s, uint16_t color) {
  if (!changed(slot, s)) return;
  char buf[80];
  fit(s, W - 28, fLabel13, buf, sizeof buf);
  buf[0] = toupper(buf[0]);
  fillRect(0, STATUS_Y - 10, W, 21, C_BG);
  text(buf, 14, STATUS_Y, textdatum_t::middle_left, fLabel13, color, C_BG);
}

static void toggle(int x, int cy, bool on) {
  fillRect(x, cy - 12, 46, 24, C_BG);
  roundRect(x, cy - 12, 46, 24, 12, on ? C_GREEN : C_LINE, false);
  circle(on ? x + 34 : x + 12, cy, 9, on ? C_BG : C_DIM);
}

// A row with a label on the left and a value (and an arrow) on the right
static void valueRow(int y, int h, bool line, const char* label, const char* value) {
  const int cy = y + h / 2;
  fillRect(0, y + 1, W, h - 1, C_BG);
  if (line) hline(14, y, W - 28, C_LINE);
  text(label, 14, cy, textdatum_t::middle_left, fLabel15, C_TEXT, C_BG);
  char buf[64];
  fit(value, W - 46 - 14 - textWidth(label, fLabel15) - 12, fLabel15, buf, sizeof buf);
  text(buf, W - 34, cy, textdatum_t::middle_right, fLabel15, C_SOFT, C_BG);
  text(icon::NEXT, W - 28, cy, textdatum_t::middle_left, fIcon14, C_DIM, C_BG);
}

static void button(int x, int y, int w, int h, const char* label, uint16_t fg, uint16_t bg) {
  fillRect(x, y, w, h, C_BG);
  roundRect(x, y, w, h, 10, bg, false);
  char buf[48];
  fit(label, w - 12, fLabel15, buf, sizeof buf);
  text(buf, x + w / 2, y + h / 2, textdatum_t::middle_center, fLabel15, fg, bg);
}

// ---------------------------------------------------------------- scrolling list

static int listFirst = 0;
static int listRight = W;  // right edge of the rows; scroll buttons beyond it

static void drawList(int count, void (*item)(int i, int y)) {
  const bool scrolls = count > LIST_ROWS;
  listRight = scrolls ? LIST_W : W;
  listFirst = constrain(listFirst, 0, max(0, count - LIST_ROWS));
  for (int r = 0; r < LIST_ROWS; r++) {
    const int i = listFirst + r, y = LIST_TOP + r * LIST_ROW_H;
    fillRect(0, y, listRight, LIST_ROW_H, C_BG);
    if (r && i < count) hline(14, y, listRight - 28, C_LINE);
    if (i < count) item(i, y);
  }
  if (!scrolls) return;
  const int h = LIST_ROWS * LIST_ROW_H / 2 - 6;
  const int x = LIST_W + 4, w = W - LIST_W - 10;
  fillRect(LIST_W, LIST_TOP, W - LIST_W, H - LIST_TOP, C_BG);
  roundRect(x, LIST_TOP + 2, w, h, 8, C_CARD, false);
  text(icon::UP, x + w / 2, LIST_TOP + 2 + h / 2, textdatum_t::middle_center, fIcon18,
       listFirst > 0 ? C_TEXT : C_LINE, C_CARD);
  roundRect(x, LIST_TOP + h + 10, w, h, 8, C_CARD, false);
  text(icon::DOWN, x + w / 2, LIST_TOP + h + 10 + h / 2, textdatum_t::middle_center, fIcon18,
       listFirst + LIST_ROWS < count ? C_TEXT : C_LINE, C_CARD);
}

// The item tapped, or -1. A tap on a scroll button moves the list and sets scrolled.
static int listPress(int x, int y, int count, bool& scrolled) {
  scrolled = false;
  if (y < LIST_TOP || y >= LIST_TOP + LIST_ROWS * LIST_ROW_H) return -1;
  if (count > LIST_ROWS && x >= LIST_W) {
    const bool up = y < LIST_TOP + LIST_ROWS * LIST_ROW_H / 2;
    const int to = constrain(listFirst + (up ? 1 - LIST_ROWS : LIST_ROWS - 1), 0, count - LIST_ROWS);
    scrolled = to != listFirst;
    listFirst = to;
    return -1;
  }
  const int i = listFirst + (y - LIST_TOP) / LIST_ROW_H;
  return i < count ? i : -1;
}

// ---------------------------------------------------------------- time zones

static const struct {
  const char* tz;
  const char* name;
} ZONES[] = {
    {"AST4ADT,M3.2.0,M11.1.0", "Atlantic: Halifax"},
    {"NST3:30NDT,M3.2.0,M11.1.0", "Newfoundland: St. John's"},
    {"EST5EDT,M3.2.0,M11.1.0", "Eastern: Toronto, New York"},
    {"CST6CDT,M3.2.0,M11.1.0", "Central: Winnipeg, Chicago"},
    {"CST6", "Saskatchewan (no daylight time)"},
    {"MST7MDT,M3.2.0,M11.1.0", "Mountain: Edmonton, Denver"},
    {"MST7", "Arizona (no daylight time)"},
    {"PST8PDT,M3.2.0,M11.1.0", "Pacific: Vancouver, Los Angeles"},
    {"AKST9AKDT,M3.2.0,M11.1.0", "Alaska"},
    {"HST10", "Hawaii"},
    {"GMT0BST,M3.5.0/1,M10.5.0", "UK: London"},
    {"CET-1CEST,M3.5.0,M10.5.0/3", "Central Europe: Paris, Berlin"},
    {"UTC0", "UTC"},
};
static constexpr int ZONE_COUNT = sizeof ZONES / sizeof ZONES[0];

static int zoneIndex(const char* tz) {
  for (int i = 0; i < ZONE_COUNT; i++) {
    if (strcmp(ZONES[i].tz, tz) == 0) return i;
  }
  return -1;
}

// "Atlantic" for "Atlantic: Halifax", "Saskatchewan" for "Saskatchewan (...)"
static void zoneShort(const char* tz, char* buf, size_t len) {
  int i = zoneIndex(tz);
  if (i < 0) {
    snprintf(buf, len, "custom");
    return;
  }
  const char* name = ZONES[i].name;
  snprintf(buf, len, "%.*s", (int)strcspn(name, ":("), name);
  size_t n = strlen(buf);
  while (n && buf[n - 1] == ' ') buf[--n] = '\0';
}

// ---------------------------------------------------------------- touch setup page

// Three targets for the mapping, then one more to check it
static const int CAL_TARGETS[4][2] = {{30, 30}, {290, 30}, {290, 210}, {30, 210}};
static constexpr int CAL_TOLERANCE_PX = 20;
static constexpr uint32_t CAL_DONE_MS = 1200;  // "Touch is set up" stays this long
static constexpr int CAL_SAMPLES = 12;

static int calStep = 0;  // 0-2 setup targets, 3 the check, 4 done
static uint16_t calRaw[4][2];
static touch::Calibration calNew;
static Page calNext = Page::Main;
static char calMessage[64];
static uint32_t calDoneAt = 0;
static uint16_t calSamples[2][CAL_SAMPLES];
static int calCount = 0;
static bool calWasDown = false;

static void startCalibration(Page next) {
  calNext = next;
  calStep = 0;
  calMessage[0] = '\0';
  calCount = 0;
  page = Page::Calibrate;
}

static void drawTarget(int x, int y, bool pressed) {
  fillRect(x - 22, y - 22, 45, 45, C_BG);
  fillRect(x - 22, y - 1, 9, 2, C_GREEN);
  fillRect(x + 14, y - 1, 9, 2, C_GREEN);
  fillRect(x - 1, y - 22, 2, 9, C_GREEN);
  fillRect(x - 1, y + 14, 2, 9, C_GREEN);
  circle(x, y, 12, C_GREEN);
  if (!pressed) {
    circle(x, y, 9, C_BG);
    circle(x, y, 3, C_GREEN);
  }
}

static void drawCalibrate() {
  if (calStep == 4) {
    circle(W / 2, 96, 26, C_OK_BG);
    text(icon::CHECK, W / 2, 96, textdatum_t::middle_center, fIcon26, C_GREEN, C_OK_BG);
    text("Touch is set up", W / 2, 146, textdatum_t::middle_center, fTitle, C_TEXT, C_BG);
    return;
  }
  text("Touch setup", W / 2, 92, textdatum_t::middle_center, fTitle, C_TEXT, C_BG);
  char buf[64];
  if (calStep < 3) snprintf(buf, sizeof buf, "Tap the centre of each target (%d of 3)", calStep + 1);
  else snprintf(buf, sizeof buf, "Now tap this one, to check");
  text(buf, W / 2, 122, textdatum_t::middle_center, fLabel13, C_SOFT, C_BG);
  if (calMessage[0]) {
    text(calMessage, W / 2, 146, textdatum_t::middle_center, fLabel13, C_AMBER, C_BG);
  } else if (calStep == 0) {
    text("So the settings screens know where you tap", W / 2, 146, textdatum_t::middle_center, fLabel13, C_DIM,
         C_BG);
  }
  drawTarget(CAL_TARGETS[calStep][0], CAL_TARGETS[calStep][1], false);
}

static void calTap(uint16_t rx, uint16_t ry) {
  if (calStep >= 4) return;
  calRaw[calStep][0] = rx;
  calRaw[calStep][1] = ry;
  logger::printf("touch: setup point %d raw %u,%u\n", calStep + 1, rx, ry);
  calMessage[0] = '\0';
  if (calStep < 2) {
    calStep++;
  } else if (calStep == 2) {
    int screen[3][2];
    memcpy(screen, CAL_TARGETS, sizeof screen);
    if (touch::solve(calRaw, screen, calNew)) {
      calStep = 3;
    } else {
      snprintf(calMessage, sizeof calMessage, "Those taps didn't line up, let's start again");
      calStep = 0;
    }
  } else {
    int x, y;
    touch::apply(calNew, rx, ry, x, y);
    int off = (int)lroundf(hypotf(x - CAL_TARGETS[3][0], y - CAL_TARGETS[3][1]));
    logger::printf("touch: check tap landed %d px from the target\n", off);
    if (off <= CAL_TOLERANCE_PX) {
      touch::save(calNew);
      logger::printf("touch: setup saved\n");
      calStep = 4;
      calDoneAt = millis();
    } else {
      snprintf(calMessage, sizeof calMessage, "That one landed %d px away, let's start again", off);
      calStep = 0;
    }
  }
  drawPage();
}

// Collects raw readings while the finger is down; the tap counts when it lifts
static void calibratePoll(bool down) {
  if (down) {
    uint16_t rx, ry;
    if (calStep < 4 && calCount < CAL_SAMPLES && touch::readRaw(rx, ry)) {
      calSamples[0][calCount] = rx;
      calSamples[1][calCount] = ry;
      if (++calCount == 1) drawTarget(CAL_TARGETS[calStep][0], CAL_TARGETS[calStep][1], true);
    }
  } else if (calWasDown && calCount) {
    // Median, leaving out the first reading (taken as the finger lands) when there are more
    const int from = calCount > 2 ? 1 : 0;
    std::sort(calSamples[0] + from, calSamples[0] + calCount);
    std::sort(calSamples[1] + from, calSamples[1] + calCount);
    const int mid = from + (calCount - from) / 2;
    calCount = 0;
    calTap(calSamples[0][mid], calSamples[1][mid]);
  }
  calWasDown = down;
}

// ---------------------------------------------------------------- main page

static const struct {
  const char* name;
  const char* icon;
  Page page;
} TILES[] = {
    {"WiFi", icon::WIFI, Page::Wifi},    {"Bike", icon::BIKE, Page::Bike}, {"Screen", icon::SCREEN, Page::Screen},
    {"Clock", icon::CLOCK, Page::Clock}, {"NAS", icon::NAS, Page::Nas},    {"Device", icon::DEVICE, Page::Device},
};
static constexpr int TILE_COUNT = sizeof TILES / sizeof TILES[0];

static int tileX(int i) { return 6 + (i % 2) * (TILE_W + 6); }
static int tileY(int i) { return 48 + (i / 2) * (TILE_H + 6); }

static uint16_t tileColor(int i) {
  const uint16_t colors[] = {C_GREEN, C_BLUE, C_AMBER, C_VIOLET, C_CYAN, C_TEAL};
  return colors[i];
}

static void tileText(int i, char* buf, size_t len, uint16_t& color) {
  color = C_DIM;
  const settings::Values v = settings::get();
  switch (TILES[i].page) {
    case Page::Wifi: {
      String sw;
      net::Status ns = net::status();
      if (net::switchStatus(sw) == net::Switch::Trying) {
        snprintf(buf, len, "joining %s", sw.c_str());
        color = C_AMBER;
      } else if (ns.connected) {
        snprintf(buf, len, "%s", ns.ssid.c_str());
      } else {
        snprintf(buf, len, ns.configured ? "not connected" : "not set up");
        color = C_AMBER;
      }
      break;
    }
    case Page::Bike: snprintf(buf, len, "%s, %s", v.bikeName, bike::linkText(bike::get().link)); break;
    case Page::Screen: {
      char t[16];
      formatValue(Opt::Full, v.fullMin, t, sizeof t);
      snprintf(buf, len, "dims after %s", t);
      break;
    }
    case Page::Clock: zoneShort(v.timeZone, buf, len); break;
    case Page::Nas: {
      nas::Status us = nas::status();
      snprintf(buf, len, "%s", nas::stateText(us.state));
      if (us.state == nas::State::Waiting || us.state == nas::State::NotConfigured || us.failed) color = C_AMBER;
      break;
    }
    default: snprintf(buf, len, "diagnostics, restart"); break;
  }
}

static void drawTile(int i, bool full) {
  const int x = tileX(i), y = tileY(i), cy = y + TILE_H / 2;
  char buf[64], shown[64];
  uint16_t color;
  tileText(i, buf, sizeof buf, color);
  if (!changed(i, buf) && !full) return;
  if (full) {
    roundRect(x, y, TILE_W, TILE_H, 10, C_CARD);
    text(TILES[i].icon, x + 14, cy, textdatum_t::middle_left, fIcon22, tileColor(i), C_CARD);
    text(TILES[i].name, x + 48, cy - 9, textdatum_t::middle_left, fLabel15, C_TEXT, C_CARD);
  }
  fit(buf, TILE_W - 56, fLabel12, shown, sizeof shown);
  fillRect(x + 48, cy + 3, TILE_W - 54, 15, C_CARD);
  text(shown, x + 48, cy + 10, textdatum_t::middle_left, fLabel12, color, C_CARD);
}

static void drawMain() {
  header("Settings");
  for (int i = 0; i < TILE_COUNT; i++) drawTile(i, true);
}

static void mainPress(int x, int y) {
  if (backHit(x, y)) {
    close();
    return;
  }
  for (int i = 0; i < TILE_COUNT; i++) {
    if (inside(x, y, tileX(i) - 3, tileY(i) - 3, TILE_W + 6, TILE_H + 6)) {
      go(TILES[i].page);
      return;
    }
  }
}

// ---------------------------------------------------------------- keyboard

enum class Field { BikeName, WifiName, WifiPass, NasShare, NasUser, NasPass };

static constexpr int KB_TOP = 70;
static constexpr int KEY_PITCH_X = 32, KEY_PITCH_Y = 34, KEY_W = 30, KEY_H = 31;
// Keys 0-39 are characters (row * 10 + column), then these
enum Key { K_SHIFT = 40, K_BACKSPACE, K_MODE, K_DOT, K_SPACE, K_DONE, K_COUNT };
static const char* const ABC[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char* const SYM[4] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",<>?/", "`~"};

static Field kbField;
static Page kbBack;
static char kbTitle[48];
static char kbHint[48];  // shown in the empty field
static char kbText[100];
static size_t kbMax;
static bool kbSymbols = false;
static int kbShift = 0;  // 0 off, 1 for the next letter, 2 caps lock
static char kbError[64];
static int kbPressed = -1;  // key drawn pressed
static uint32_t kbPressedAt = 0, kbRepeatAt = 0;

// WiFi network being joined
static char joinSsid[33];
static char joinPass[64];
static char joinFrom[33];  // the network it goes back to if this one fails
static bool joinNeedsPass = false;

static void openKeyboard(Field f, Page back, const char* title, const char* initial, size_t max,
                         const char* hint = "") {
  kbField = f;
  kbBack = back;
  strlcpy(kbTitle, title, sizeof kbTitle);
  strlcpy(kbHint, hint, sizeof kbHint);
  strlcpy(kbText, initial, sizeof kbText);
  kbMax = std::min(max, sizeof kbText - 1);
  kbSymbols = false;
  kbShift = 0;
  kbError[0] = '\0';
  go(Page::Keyboard);
}

static const char* keyRow(int r) { return (kbSymbols ? SYM : ABC)[r]; }

static bool keyRect(int id, int& x, int& y, int& w, int& h) {
  h = KEY_H;
  if (id < K_SHIFT) {
    const int r = id / 10, c = id % 10, n = strlen(keyRow(r));
    if (c >= n) return false;
    // The bottom letter row sits between shift and backspace
    const int start = r == 3 ? 48 + (224 - n * KEY_PITCH_X) / 2 : (W - n * KEY_PITCH_X) / 2;
    x = start + c * KEY_PITCH_X + 1;
    y = KB_TOP + r * KEY_PITCH_Y;
    w = KEY_W;
    return true;
  }
  y = KB_TOP + (id <= K_BACKSPACE ? 3 : 4) * KEY_PITCH_Y;
  switch (id) {
    case K_SHIFT:
      if (kbSymbols) return false;
      x = 1, w = 44;
      break;
    case K_BACKSPACE: x = 275, w = 44; break;
    case K_MODE: x = 1, w = 56; break;
    case K_DOT: x = 60, w = 30; break;
    case K_SPACE: x = 93, w = 130; break;
    default: x = 226, w = 93; break;  // K_DONE
  }
  return true;
}

static int keyAt(int x, int y) {
  for (int id = 0; id < K_COUNT; id++) {
    int kx, ky, kw, kh;
    if (keyRect(id, kx, ky, kw, kh) && inside(x, y, kx - 2, ky - 2, kw + 4, kh + 4)) return id;
  }
  return -1;
}

static char keyChar(int id) {
  char c = keyRow(id / 10)[id % 10];
  return kbShift && c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static void drawKey(int id, bool pressed) {
  int x, y, w, h;
  if (!keyRect(id, x, y, w, h)) return;
  uint16_t bg = id >= K_SHIFT ? C_LINE : C_CARD, fg = C_TEXT;
  if (id == K_DONE) bg = C_GREEN, fg = C_BG;
  if (id == K_SHIFT && kbShift) bg = C_TEXT, fg = C_BG;
  if (pressed) bg = C_BLUE, fg = C_BG;
  roundRect(x, y, w, h, 6, bg, false);
  const int cx = x + w / 2, cy = y + h / 2;
  switch (id) {
    case K_SHIFT: text(kbShift == 2 ? icon::CAPS : icon::SHIFT, cx, cy, textdatum_t::middle_center, fIcon18, fg, bg); break;
    case K_BACKSPACE: text(icon::BACKSPACE, cx, cy, textdatum_t::middle_center, fIcon18, fg, bg); break;
    case K_MODE: text(kbSymbols ? "abc" : "#+=", cx, cy, textdatum_t::middle_center, fLabel15, fg, bg); break;
    case K_DOT: text(".", cx, cy, textdatum_t::middle_center, fText18, fg, bg); break;
    case K_SPACE: text("space", cx, cy, textdatum_t::middle_center, fLabel13, pressed ? fg : C_DIM, bg); break;
    case K_DONE: text("Done", cx, cy, textdatum_t::middle_center, fLabel15, fg, bg); break;
    default: {
      const char label[2] = {keyChar(id), '\0'};
      text(label, cx, cy, textdatum_t::middle_center, fText18, fg, bg);
    }
  }
}

static void drawKeys() {
  fillRect(0, KB_TOP, W, H - KB_TOP, C_BG);
  for (int id = 0; id < K_COUNT; id++) drawKey(id, id == kbPressed);
}

static void drawKbHeader() {
  fillRect(0, 0, W, 33, C_BG);
  roundRect(4, 2, 38, 28, 8, C_CARD);
  text(icon::CLOSE, 23, 16, textdatum_t::middle_center, fIcon18, C_TEXT, C_CARD);
  char buf[64];
  if (kbError[0]) {
    fit(kbError, W - 58, fLabel13, buf, sizeof buf);
    text(buf, 50, 16, textdatum_t::middle_left, fLabel13, C_BAD, C_BG);
  } else {
    fit(kbTitle, W - 58, fLabel15, buf, sizeof buf);
    text(buf, 50, 16, textdatum_t::middle_left, fLabel15, C_TEXT, C_BG);
  }
}

static void drawField() {
  roundRect(4, 34, W - 8, 32, 8, C_CARD, false);
  clip(8, 36, W - 16, 28);
  if (!kbText[0]) {
    fillRect(14, 40, 2, 20, C_GREEN);
    if (kbHint[0]) text(kbHint, 22, 50, textdatum_t::middle_left, fLabel13, C_DIM, C_CARD);
  } else {
    // Long text shows its end, where the typing is
    const char* s = kbText;
    while (s[1] && textWidth(s, fText18) > W - 40) s++;
    text(s, 14, 50, textdatum_t::middle_left, fText18, C_TEXT, C_CARD);
    fillRect(15 + textWidth(s, fText18), 40, 2, 20, C_GREEN);
  }
  noClip();
}

static void drawKeyboard() {
  drawKbHeader();
  drawField();
  drawKeys();
}

static void kbFail(const char* message) {
  strlcpy(kbError, message, sizeof kbError);
  drawKbHeader();
}

static void type(char c) {
  size_t n = strlen(kbText);
  if (n >= kbMax) {
    kbFail("That's as long as it can be");
    return;
  }
  kbText[n] = c;
  kbText[n + 1] = '\0';
}

static void backspace() {
  size_t n = strlen(kbText);
  if (n) kbText[n - 1] = '\0';
}

static void startJoin(const char* pass);

static void keyboardDone() {
  String t = kbText;
  String url, user;
  bool havePass;
  nas::stored(url, user, havePass);
  switch (kbField) {
    case Field::BikeName: {
      t.trim();
      if (t.isEmpty()) {
        kbFail("Enter the name the bike shows in Bluetooth");
        return;
      }
      settings::Values v = settings::get();
      strlcpy(v.bikeName, t.c_str(), sizeof v.bikeName);
      if (!save(v)) {
        kbFail(saveError);
        return;
      }
      go(Page::Bike);
      return;
    }
    case Field::WifiName: {
      if (t.isEmpty()) {
        kbFail("Enter the network's name");
        return;
      }
      strlcpy(joinSsid, kbText, sizeof joinSsid);
      joinNeedsPass = false;  // could be an open network
      char title[48];
      snprintf(title, sizeof title, "Password for %s", joinSsid);
      openKeyboard(Field::WifiPass, Page::Wifi, title, "", 63, "Leave empty for an open network");
      return;
    }
    case Field::WifiPass: {
      const bool current = net::status().ssid == joinSsid;
      if (t.isEmpty() && current) {
        t = net::savedPassword();
      } else if (t.isEmpty() && joinNeedsPass) {
        kbFail("Enter the network's password");
        return;
      }
      if (!t.isEmpty() && (t.length() < 8 || t.length() > 63)) {
        kbFail("WiFi passwords are 8 to 63 characters");
        return;
      }
      startJoin(t.c_str());
      return;
    }
    case Field::NasShare:
      t.trim();
      if (!nas::validShare(t)) {
        kbFail("It should look like \\\\server\\share");
        return;
      }
      nas::store(t, user, nullptr);
      go(Page::Nas);
      return;
    case Field::NasUser:
      t.trim();
      nas::store(url, t, nullptr);
      go(Page::Nas);
      return;
    case Field::NasPass:
      if (!t.isEmpty()) nas::store(url, user, &t);
      go(Page::Nas);
      return;
  }
}

static void keyboardPress(int x, int y) {
  if (x < 48 && y < 34) {
    go(kbBack);
    return;
  }
  const int id = keyAt(x, y);
  if (id < 0) return;
  if (kbError[0]) {
    kbError[0] = '\0';
    drawKbHeader();
  }
  kbPressed = id;
  kbPressedAt = millis();
  kbRepeatAt = 0;
  bool relabel = false;
  switch (id) {
    case K_SHIFT:
      kbShift = (kbShift + 1) % 3;
      relabel = true;
      break;
    case K_MODE:
      kbSymbols = !kbSymbols;
      relabel = true;
      break;
    case K_BACKSPACE: backspace(); break;
    case K_DONE:
      kbPressed = -1;
      keyboardDone();
      return;
    case K_SPACE: type(' '); break;
    case K_DOT: type('.'); break;
    default: {
      const char c = keyChar(id);
      type(c);
      if (kbShift == 1 && isalpha((unsigned char)c)) {
        kbShift = 0;
        relabel = true;
      }
    }
  }
  if (relabel) drawKeys();
  else drawKey(id, true);
  drawField();
}

static void keyboardHold() {
  if (kbPressed != K_BACKSPACE) return;
  const uint32_t now = millis();
  if (now - kbPressedAt < REPEAT_DELAY_MS || (kbRepeatAt && now - kbRepeatAt < REPEAT_MS)) return;
  kbRepeatAt = now;
  backspace();
  drawField();
}

// ---------------------------------------------------------------- option pages (bike, screen)

static const Opt BIKE_ROWS[] = {Opt::BikeName, Opt::Release, Opt::RideEnd, Opt::MinSec, Opt::MinM};
static const Opt SCREEN_ROWS[] = {Opt::Full, Opt::Dim, Opt::Off, Opt::Night, Opt::Saved};
static constexpr int OPTION_ROWS = 5;

static const Opt* pageOptions() { return page == Page::Bike ? BIKE_ROWS : SCREEN_ROWS; }

static void drawOptionRow(int i) {
  const Opt o = pageOptions()[i];
  const settings::Values v = settings::get();
  const int y = ROW_Y + i * ROW_H;
  char buf[40];
  if (o == Opt::Night) {
    fillRect(0, y + 1, W, ROW_H - 1, C_BG);
    if (i) hline(14, y, W - 28, C_LINE);
    text(info(o).label, 14, y + ROW_H / 2, textdatum_t::middle_left, fLabel15, C_TEXT, C_BG);
    toggle(W - 62, y + ROW_H / 2, v.nightDim);
    return;
  }
  if (o == Opt::BikeName) snprintf(buf, sizeof buf, "%s", v.bikeName);
  else formatValue(o, valueOf(v, o), buf, sizeof buf);
  valueRow(y, ROW_H, i > 0, info(o).label, buf);
}

static void drawOptions() {
  header(page == Page::Bike ? "Bike" : "Screen");
  for (int i = 0; i < OPTION_ROWS; i++) drawOptionRow(i);
}

static Opt pickOpt;
static Page pickBack;

static void optionsPress(int x, int y) {
  if (backHit(x, y)) {
    go(Page::Main);
    return;
  }
  if (y < ROW_Y) return;
  const int i = (y - ROW_Y) / ROW_H;
  if (i >= OPTION_ROWS) return;
  const Opt o = pageOptions()[i];
  settings::Values v = settings::get();
  if (o == Opt::Night) {
    v.nightDim = !v.nightDim;
    if (save(v)) drawOptionRow(i);
  } else if (o == Opt::BikeName) {
    openKeyboard(Field::BikeName, Page::Bike, "Bike's Bluetooth name", v.bikeName, sizeof v.bikeName - 1);
  } else {
    pickOpt = o;
    pickBack = page;
    go(Page::Picker);
  }
}

// ---------------------------------------------------------------- picker (one option's choices)

static constexpr int CHOICE_W = 72, CHOICE_H = 52;

static int choiceX(int i) { return 8 + (i % 4) * (CHOICE_W + 6); }
static int choiceY(int i) { return 126 + (i / 4) * (CHOICE_H + 6); }

static bool choiceAllowed(uint16_t c) {
  settings::Values v = settings::get();
  setValue(v, pickOpt, c);
  char error[64];
  return settings::check(v, error, sizeof error);
}

static void drawPicker() {
  header(info(pickOpt).label);
  wrap(info(pickOpt).help, 14, 50, W - 28, 17, fLabel13, C_DIM, C_BG, 3);
  const settings::Values v = settings::get();
  const long current = valueOf(v, pickOpt);
  bool blocked = false;
  for (int i = 0; i < 8; i++) {
    const uint16_t c = info(pickOpt).choices[i];
    const bool allowed = choiceAllowed(c), selected = c == current;
    blocked |= !allowed;
    const uint16_t bg = selected ? C_GREEN : C_CARD;
    roundRect(choiceX(i), choiceY(i), CHOICE_W, CHOICE_H, 10, bg);
    char buf[16];
    formatValue(pickOpt, c, buf, sizeof buf);
    text(buf, choiceX(i) + CHOICE_W / 2, choiceY(i) + CHOICE_H / 2, textdatum_t::middle_center, fText18,
         selected ? C_BG : allowed ? C_TEXT : C_LINE, bg);
  }
  if (blocked) {
    char other[16], note[80];
    if (pickOpt == Opt::Full) {
      formatValue(Opt::Off, v.offMin, other, sizeof other);
      snprintf(note, sizeof note, "Greyed out: not shorter than Screen off after (%s)", other);
    } else if (pickOpt == Opt::Off) {
      formatValue(Opt::Full, v.fullMin, other, sizeof other);
      snprintf(note, sizeof note, "Greyed out: not longer than Full brightness for (%s)", other);
    } else {
      snprintf(note, sizeof note, "Greyed-out choices aren't allowed");
    }
    text(note, 14, 112, textdatum_t::middle_left, fLabel12, C_AMBER, C_BG);
  }
}

static void pickerPress(int x, int y) {
  if (backHit(x, y)) {
    go(pickBack);
    return;
  }
  for (int i = 0; i < 8; i++) {
    if (!inside(x, y, choiceX(i) - 3, choiceY(i) - 3, CHOICE_W + 6, CHOICE_H + 6)) continue;
    const uint16_t c = info(pickOpt).choices[i];
    if (!choiceAllowed(c)) return;
    settings::Values v = settings::get();
    setValue(v, pickOpt, c);
    if (save(v)) go(pickBack);
    return;
  }
}

// ---------------------------------------------------------------- WiFi page

struct Network {
  char ssid[33];
  int8_t rssi;
  bool secure;
};
static constexpr int MAX_NETWORKS = 16;
static Network nets[MAX_NETWORKS];
static int netCount = 0;
enum class Scan { None, Running, Done, Failed };
static Scan scanState = Scan::None;
static uint32_t scannedAt = 0;

static bool switching() {
  String s;
  return net::switchStatus(s) == net::Switch::Trying;
}

static void startScan() {
  WiFi.scanDelete();
  scanState = WiFi.scanNetworks(true) == WIFI_SCAN_FAILED ? Scan::Failed : Scan::Running;
}

// True when a running scan has just finished (nets has its results)
static bool scanFinished() {
  if (scanState != Scan::Running) return false;
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return false;
  if (n < 0) {
    scanState = Scan::Failed;
    return true;
  }
  netCount = 0;
  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (name.isEmpty() || name.length() > 32) continue;  // hidden network
    const int rssi = WiFi.RSSI(i);
    int k = 0;
    while (k < netCount && strcmp(nets[k].ssid, name.c_str()) != 0) k++;
    if (k < netCount) {
      if (rssi <= nets[k].rssi) continue;  // one entry per name, the strongest
    } else if (netCount < MAX_NETWORKS) {
      netCount++;
    } else {
      // Full: this one replaces the weakest, if it's stronger
      k = 0;
      for (int j = 1; j < netCount; j++) {
        if (nets[j].rssi < nets[k].rssi) k = j;
      }
      if (rssi <= nets[k].rssi) continue;
    }
    strlcpy(nets[k].ssid, name.c_str(), sizeof nets[k].ssid);
    nets[k].rssi = rssi;
    nets[k].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  std::sort(nets, nets + netCount, [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
  WiFi.scanDelete();  // ~100 bytes a network, and nets has what it needs
  scanState = Scan::Done;
  scannedAt = millis();
  return true;
}

static const char* signalIcon(int rssi) {
  return rssi >= -60 ? icon::WIFI : rssi >= -70 ? icon::WIFI_HIGH : rssi >= -80 ? icon::WIFI_LOW : icon::WIFI_ZERO;
}

static void wifiItem(int i, int y) {
  const int cy = y + LIST_ROW_H / 2;
  if (i == netCount) {
    text("Other network...", 14, cy, textdatum_t::middle_left, fLabel15, C_SOFT, C_BG);
    return;
  }
  const Network& n = nets[i];
  const net::Status ns = net::status();
  const bool current = ns.connected && ns.ssid == n.ssid;
  text(signalIcon(n.rssi), 14, cy, textdatum_t::middle_left, fIcon18, current ? C_GREEN : C_SOFT, C_BG);
  int right = listRight - 14;
  if (n.secure) {
    text(icon::LOCK, right, cy, textdatum_t::middle_right, fIcon14, C_DIM, C_BG);
    right -= 22;
  }
  if (current) {
    text("Connected", right, cy, textdatum_t::middle_right, fLabel12, C_GREEN, C_BG);
    right -= textWidth("Connected", fLabel12) + 10;
  }
  char name[40];
  fit(n.ssid, right - 44, fLabel15, name, sizeof name);
  text(name, 44, cy, textdatum_t::middle_left, fLabel15, C_TEXT, C_BG);
}

static void drawWifiList() {
  drawList(netCount + 1, wifiItem);
  if (netCount) return;
  const char* note = scanState == Scan::Running  ? "Looking for networks..."
                     : scanState == Scan::Failed ? "Couldn't look for networks. Tap the arrows to try again."
                                                 : "No networks found";
  text(note, 14, LIST_TOP + LIST_ROW_H + LIST_ROW_H / 2, textdatum_t::middle_left, fLabel13, C_DIM, C_BG);
}

static void wifiStatus() {
  char buf[80];
  uint16_t color = C_AMBER;
  String sw;
  const net::Status ns = net::status();
  if (net::switchStatus(sw) == net::Switch::Trying) {
    snprintf(buf, sizeof buf, "Joining %s...", sw.c_str());
  } else if (ns.connected) {
    snprintf(buf, sizeof buf, "Connected to %s, %s", ns.ssid.c_str(), ns.ip.c_str());
    color = C_GREEN;
  } else if (ns.configured) {
    snprintf(buf, sizeof buf, "Not connected, trying %s", ns.ssid.c_str());
  } else {
    snprintf(buf, sizeof buf, "No network set up yet");
  }
  statusLine(0, buf, color);
}

static void drawWifiHeader() { header("WiFi", icon::REFRESH, scanState == Scan::Running ? C_BLUE : C_TEXT); }

static void drawWifi() {
  drawWifiHeader();
  wifiStatus();
  drawWifiList();
}

static void wifiPress(int x, int y) {
  if (backHit(x, y)) {
    go(Page::Main);
    return;
  }
  if (actionHit(x, y)) {
    if (scanState != Scan::Running && !switching()) startScan();
    drawWifiHeader();
    drawWifiList();
    return;
  }
  bool scrolled;
  const int i = listPress(x, y, netCount + 1, scrolled);
  if (scrolled) drawWifiList();
  if (i < 0) return;
  if (i == netCount) {
    openKeyboard(Field::WifiName, Page::Wifi, "Network name", "", 32);
    return;
  }
  strlcpy(joinSsid, nets[i].ssid, sizeof joinSsid);
  joinNeedsPass = nets[i].secure;
  if (!nets[i].secure) {
    startJoin("");
    return;
  }
  char title[48];
  snprintf(title, sizeof title, "Password for %s", joinSsid);
  const bool current = net::status().ssid == joinSsid;
  openKeyboard(Field::WifiPass, Page::Wifi, title, "", 63, current ? "Leave empty to keep the saved one" : "");
}

// ---------------------------------------------------------------- joining a network

static int joinShown = -1;  // switch state on screen

static void startJoin(const char* pass) {
  strlcpy(joinPass, pass, sizeof joinPass);
  strlcpy(joinFrom, net::status().ssid.c_str(), sizeof joinFrom);
  logger::printf("menu: joining WiFi %s\n", joinSsid);
  net::switchTo(joinSsid, joinPass);
  go(Page::Join);
}

static int joinButtonY() { return 194; }

static void drawJoin() {
  header("WiFi");
  String sw;
  const net::Switch st = net::switchStatus(sw);
  joinShown = (int)st;
  const bool joined = st == net::Switch::Joined, failed = st == net::Switch::Failed;
  const uint16_t color = joined ? C_GREEN : failed ? C_BAD : C_AMBER;
  const uint16_t bg = joined ? C_OK_BG : C_CONFIRM_BG;
  circle(W / 2, 86, 24, bg);
  text(joined ? icon::CHECK : failed ? icon::WIFI_OFF : icon::WIFI, W / 2, 86, textdatum_t::middle_center, fIcon26,
       color, bg);

  char title[64], line1[80], line2[80] = "";
  const bool sameNetwork = strcmp(joinFrom, joinSsid) == 0;
  if (joined) {
    snprintf(title, sizeof title, "Connected to %s", joinSsid);
    snprintf(line1, sizeof line1, "Address %s", net::status().ip.c_str());
    snprintf(line2, sizeof line2, "Saved: it uses this network from now on");
  } else if (failed) {
    snprintf(title, sizeof title, "Couldn't join %s", joinSsid);
    snprintf(line1, sizeof line1, "Check the password, and that it's a 2.4 GHz network");
    if (joinFrom[0] && !sameNetwork) snprintf(line2, sizeof line2, "Back on %s", joinFrom);
    else if (sameNetwork) snprintf(line2, sizeof line2, "The saved password is kept");
  } else {
    snprintf(title, sizeof title, "Joining %s", joinSsid);
    snprintf(line1, sizeof line1, "This takes up to 20 seconds");
    if (joinFrom[0] && !sameNetwork) snprintf(line2, sizeof line2, "If it can't join, it goes back to %s", joinFrom);
  }
  char buf[80];
  fit(title, W - 24, fText18, buf, sizeof buf);
  text(buf, W / 2, 130, textdatum_t::middle_center, fText18, C_TEXT, C_BG);
  fit(line1, W - 24, fLabel13, buf, sizeof buf);
  text(buf, W / 2, 154, textdatum_t::middle_center, fLabel13, C_DIM, C_BG);
  fit(line2, W - 24, fLabel13, buf, sizeof buf);
  text(buf, W / 2, 173, textdatum_t::middle_center, fLabel13, C_DIM, C_BG);

  if (joined) {
    button(90, joinButtonY(), 140, 40, "Done", C_BG, C_GREEN);
  } else if (failed) {
    button(14, joinButtonY(), 140, 40, "Try again", C_TEXT, C_CARD);
    button(166, joinButtonY(), 140, 40, "Done", C_TEXT, C_CARD);
  }
}

static void joinPress(int x, int y) {
  String sw;
  const net::Switch st = net::switchStatus(sw);
  if (backHit(x, y)) {
    go(Page::Wifi);
    return;
  }
  if (y < joinButtonY() - 4) return;
  if (st == net::Switch::Joined) {
    go(Page::Wifi);
  } else if (st == net::Switch::Failed) {
    if (x < W / 2) {
      char title[48];
      snprintf(title, sizeof title, "Password for %s", joinSsid);
      openKeyboard(Field::WifiPass, Page::Wifi, title, joinPass, 63);
    } else {
      go(Page::Wifi);
    }
  }
}

// ---------------------------------------------------------------- clock page

static int zoneCount() { return ZONE_COUNT + (zoneIndex(settings::get().timeZone) < 0 ? 1 : 0); }

static void zoneItem(int i, int y) {
  const settings::Values v = settings::get();
  const int cy = y + LIST_ROW_H / 2;
  char custom[64];
  const char* name = ZONES[0].name;
  bool selected = true;
  if (i < ZONE_COUNT) {
    name = ZONES[i].name;
    selected = strcmp(ZONES[i].tz, v.timeZone) == 0;
  } else {
    snprintf(custom, sizeof custom, "Custom: %s", v.timeZone);
    name = custom;
  }
  char buf[64];
  fit(name, listRight - 60, fLabel15, buf, sizeof buf);
  text(buf, 14, cy, textdatum_t::middle_left, fLabel15, selected ? C_GREEN : C_TEXT, C_BG);
  if (selected) text(icon::CHECK, listRight - 34, cy, textdatum_t::middle_left, fIcon18, C_GREEN, C_BG);
}

static void clockStatus() {
  if (!net::timeSynced()) {
    statusLine(0, "Waiting for the time from the internet", C_AMBER);
    return;
  }
  char buf[64];
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  strftime(buf, sizeof buf, "Now %H:%M %Z, %a %d %b", &t);
  statusLine(0, buf, C_DIM);
}

static void drawClock() {
  header("Time zone");
  clockStatus();
  drawList(zoneCount(), zoneItem);
}

static void clockPress(int x, int y) {
  if (backHit(x, y)) {
    go(Page::Main);
    return;
  }
  bool scrolled;
  const int i = listPress(x, y, zoneCount(), scrolled);
  if (scrolled) drawList(zoneCount(), zoneItem);
  if (i < 0 || i >= ZONE_COUNT) return;
  settings::Values v = settings::get();
  strlcpy(v.timeZone, ZONES[i].tz, sizeof v.timeZone);
  if (!save(v)) return;
  clockStatus();
  drawList(zoneCount(), zoneItem);
}

// ---------------------------------------------------------------- NAS page

static char testName[48];  // test file sent from this page

static void nasStatus() {
  char buf[96], last[48];
  uint16_t color = C_DIM;
  const nas::Status us = nas::status();
  nas::lastUploaded(last, sizeof last);
  if (testName[0] && strcmp(last, testName) == 0) {
    snprintf(buf, sizeof buf, "The test file reached the NAS");
    color = C_GREEN;
  } else if (us.state == nas::State::Waiting && us.lastError[0]) {
    snprintf(buf, sizeof buf, "%s", us.lastError);
    color = C_AMBER;
  } else if (testName[0]) {
    snprintf(buf, sizeof buf, "Sending the test file...");
    color = C_BLUE;
  } else if (us.pending) {
    snprintf(buf, sizeof buf, "%s, %d to upload", nas::stateText(us.state), us.pending);
    color = C_BLUE;
  } else if (us.failed) {
    snprintf(buf, sizeof buf, "%s, %d failed", nas::stateText(us.state), us.failed);
    color = C_AMBER;
  } else {
    snprintf(buf, sizeof buf, "%s", nas::stateText(us.state));
    color = us.state == nas::State::Idle ? C_GREEN : C_AMBER;
  }
  statusLine(0, buf, color);
}

static void drawNasRow(int i) {
  const int y = LIST_TOP + i * LIST_ROW_H;
  if (i == 3) {
    fillRect(0, y, W, LIST_ROW_H, C_BG);
    roundRect(14, y + 4, W - 28, LIST_ROW_H - 6, 10, C_CARD);
    text(icon::CLOUD_UP, W / 2 - 64, y + 3 + LIST_ROW_H / 2, textdatum_t::middle_center, fIcon20, C_BLUE, C_CARD);
    text("Send a test file", W / 2 + 12, y + 3 + LIST_ROW_H / 2, textdatum_t::middle_center, fLabel15, C_TEXT,
         C_CARD);
    return;
  }
  String url, user;
  bool havePass;
  nas::stored(url, user, havePass);
  const char* labels[] = {"Share", "Username", "Password"};
  const char* values[] = {url.isEmpty() ? "not set" : url.c_str(), user.isEmpty() ? "none" : user.c_str(),
                          havePass ? "saved" : "not set"};
  valueRow(y, LIST_ROW_H, i > 0, labels[i], values[i]);
}

static void drawNas() {
  header("NAS");
  nasStatus();
  for (int i = 0; i < 4; i++) drawNasRow(i);
}

static void nasPress(int x, int y) {
  if (backHit(x, y)) {
    go(Page::Main);
    return;
  }
  if (y < LIST_TOP) return;
  const int i = (y - LIST_TOP) / LIST_ROW_H;
  String url, user;
  bool havePass;
  nas::stored(url, user, havePass);
  switch (i) {
    case 0: openKeyboard(Field::NasShare, Page::Nas, "NAS share, like \\\\server\\share", url.c_str(), 95); break;
    case 1: openKeyboard(Field::NasUser, Page::Nas, "NAS username", user.c_str(), 63); break;
    case 2:
      openKeyboard(Field::NasPass, Page::Nas, "NAS password", "", 63,
                   havePass ? "Leave empty to keep the saved one" : "");
      break;
    case 3: {
      String name;
      if (nas::queueTest(name)) {
        strlcpy(testName, name.c_str(), sizeof testName);
        logger::printf("menu: NAS test file %s queued\n", testName);
        nasStatus();
      } else {
        statusLine(0, "Couldn't write the test file to the SD card", C_BAD);
      }
      break;
    }
  }
}

// ---------------------------------------------------------------- device page

enum class Confirm { None, Restart, Reset };
static Confirm confirming = Confirm::None;
static uint32_t confirmUntil = 0;
static bool resetDone = false;

static constexpr int DIAG_Y = 50, DIAG_H = 48;  // the Diagnostics row
static constexpr int DEV_BTN_Y = 106, DEV_BTN_H = 40;

static void deviceButtons() {
  button(6, DEV_BTN_Y, 151, DEV_BTN_H, "Touch setup", C_TEXT, C_CARD);
  if (confirming == Confirm::Restart) button(163, DEV_BTN_Y, 151, DEV_BTN_H, "Tap again", C_PAUSED, C_CONFIRM_BG);
  else button(163, DEV_BTN_Y, 151, DEV_BTN_H, "Restart", C_TEXT, C_CARD);
  const int y = DEV_BTN_Y + DEV_BTN_H + 6;
  if (confirming == Confirm::Reset) {
    button(6, y, 308, DEV_BTN_H, "Tap again to reset every option", C_PAUSED, C_CONFIRM_BG);
  } else if (resetDone) {
    button(6, y, 308, DEV_BTN_H, "Options are back to their defaults", C_GREEN, C_CARD);
  } else {
    button(6, y, 308, DEV_BTN_H, "Reset options to defaults", C_TEXT, C_CARD);
  }
}

static void drawDevice() {
  header("Device");
  roundRect(6, DIAG_Y, W - 12, DIAG_H, 10, C_CARD);
  const int cy = DIAG_Y + DIAG_H / 2;
  text(icon::DIAGNOSTICS, 20, cy, textdatum_t::middle_left, fIcon18, C_TEAL, C_CARD);
  text("Diagnostics", 50, cy - 8, textdatum_t::middle_left, fLabel15, C_TEXT, C_CARD);
  text("Connections, uploads, memory, uptime", 50, cy + 10, textdatum_t::middle_left, fLabel12, C_DIM, C_CARD);
  text(icon::NEXT, W - 28, cy, textdatum_t::middle_left, fIcon14, C_DIM, C_CARD);
  deviceButtons();
  char buf[64];
  const net::Status ns = net::status();
  snprintf(buf, sizeof buf, "Firmware %s \xC2\xB7 %s", FW_VERSION, ns.connected ? ns.ip.c_str() : "offline");
  text(buf, W / 2, 222, textdatum_t::middle_center, fLabel12, C_DIM, C_BG);
}

static void devicePress(int x, int y) {
  if (backHit(x, y)) {
    go(Page::Main);
    return;
  }
  if (y >= DIAG_Y - 3 && y < DIAG_Y + DIAG_H + 3) {
    go(Page::Diagnostics);
    return;
  }
  if (y < DEV_BTN_Y - 3 || y >= DEV_BTN_Y + 2 * DEV_BTN_H + 9) return;
  const bool confirmed = (int32_t)(millis() - confirmUntil) < 0;
  if (y < DEV_BTN_Y + DEV_BTN_H + 3 && x < W / 2) {
    startCalibration(Page::Device);
    drawPage();
  } else if (y < DEV_BTN_Y + DEV_BTN_H + 3) {
    if (confirming == Confirm::Restart && confirmed) {
      button(163, DEV_BTN_Y, 151, DEV_BTN_H, "Restarting...", C_PAUSED, C_CONFIRM_BG);
      logger::printf("menu: restart requested\n");
      logger::flush(true);
      delay(200);
      ESP.restart();
    }
    confirming = Confirm::Restart;
    confirmUntil = millis() + CONFIRM_MS;
    deviceButtons();
  } else {
    if (confirming == Confirm::Reset && confirmed) {
      confirming = Confirm::None;
      resetDone = save(settings::defaults());
    } else {
      confirming = Confirm::Reset;
      confirmUntil = millis() + CONFIRM_MS;
    }
    deviceButtons();
  }
}

static void deviceRefresh() {
  if (confirming != Confirm::None && (int32_t)(millis() - confirmUntil) >= 0) {
    confirming = Confirm::None;
    deviceButtons();
  }
}

// ---------------------------------------------------------------- diagnostics page

// Everything the home screen leaves out, a line each, updated every second
static constexpr int DIAG_ROWS = 10;
static constexpr int DIAG_TOP = 49, DIAG_PITCH = 19, DIAG_VALUE_X = 100;

static void diagnosticsLine(int i) {
  static const char* const LABELS[DIAG_ROWS] = {"Firmware", "Started", "WiFi",    "Bike",   "Uploads",
                                                "",         "Clock",   "SD card", "Memory", "Sensors"};
  const char* label = LABELS[i];
  char buf[96];
  switch (i) {
    case 0: {
      char build[9];
      esp_app_get_elf_sha256(build, sizeof build);
      snprintf(buf, sizeof buf, "%s, build %s", FW_VERSION, build);
      break;
    }
    case 1: {
      const uint32_t up = millis() / 1000;
      snprintf(buf, sizeof buf, "%s, %lud %02lu:%02lu:%02lu ago", logger::resetReason(), (unsigned long)(up / 86400),
               (unsigned long)(up / 3600 % 24), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
      break;
    }
    case 2: {
      const net::Status ns = net::status();
      if (ns.connected) snprintf(buf, sizeof buf, "%s, %d dBm, %s", ns.ssid.c_str(), ns.rssi, ns.ip.c_str());
      else if (ns.configured) snprintf(buf, sizeof buf, "not connected, trying %s", ns.ssid.c_str());
      else snprintf(buf, sizeof buf, "not set up");
      break;
    }
    case 3: snprintf(buf, sizeof buf, "%s (%s)", bike::linkText(bike::get().link), settings::get().bikeName); break;
    case 4: {
      const nas::Status us = nas::status();
      snprintf(buf, sizeof buf, "%s, %d waiting, %d failed", nas::stateText(us.state), us.pending, us.failed);
      break;
    }
    case 5: {
      // The last error while there is one, else the last file uploaded
      const nas::Status us = nas::status();
      if (us.lastError[0]) {
        label = "Upload error";
        snprintf(buf, sizeof buf, "%s", us.lastError);
      } else {
        label = "Last upload";
        nas::lastUploaded(buf, sizeof buf);
        if (!buf[0]) snprintf(buf, sizeof buf, "none since it started");
      }
      break;
    }
    case 6:
      if (net::timeSynced()) {
        const time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %Z", &t);
      } else {
        snprintf(buf, sizeof buf, "waiting for the internet time");
      }
      break;
    case 7: {
      const storage::Status ss = storage::status();
      if (ss.mounted) {
        snprintf(buf, sizeof buf, "%s, %.1f of %.1f GB free", ss.type, ss.freeBytes / 1e9, ss.totalBytes / 1e9);
      } else {
        snprintf(buf, sizeof buf, "not found");
      }
      break;
    }
    case 8:
      snprintf(buf, sizeof buf, "%u KB free, largest %u, lowest %u",
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024),
               (unsigned)(esp_get_minimum_free_heap_size() / 1024));
      break;
    default:
      snprintf(buf, sizeof buf, "light %d, touch %s", analogRead(PIN_LDR),
               touch::calibrated() ? "set up" : "not set up");
      break;
  }
  char key[112];
  snprintf(key, sizeof key, "%s|%s", label, buf);
  if (!changed(i, key)) return;
  const int y = DIAG_TOP + i * DIAG_PITCH;
  char shown[96];
  fit(buf, W - DIAG_VALUE_X - 8, fLabel13, shown, sizeof shown);
  fillRect(0, y, W, DIAG_PITCH, C_BG);
  text(label, 14, y + 2, textdatum_t::top_left, fLabel13, C_DIM, C_BG);
  text(shown, DIAG_VALUE_X, y + 2, textdatum_t::top_left, fLabel13, C_TEXT, C_BG);
}

static void drawDiagnostics() {
  header("Diagnostics");
  for (int i = 0; i < DIAG_ROWS; i++) diagnosticsLine(i);
}

static void diagnosticsPress(int x, int y) {
  if (backHit(x, y)) go(Page::Device);
}

// ---------------------------------------------------------------- pages

static void drawPage() {
  for (auto& d : drawn) d[0] = '\0';
  fillRect(0, 0, W, H, C_BG);
  switch (page) {
    case Page::Calibrate: drawCalibrate(); break;
    case Page::Main: drawMain(); break;
    case Page::Wifi: drawWifi(); break;
    case Page::Join: drawJoin(); break;
    case Page::Bike:
    case Page::Screen: drawOptions(); break;
    case Page::Clock: drawClock(); break;
    case Page::Nas: drawNas(); break;
    case Page::Device: drawDevice(); break;
    case Page::Diagnostics: drawDiagnostics(); break;
    case Page::Picker: drawPicker(); break;
    case Page::Keyboard: drawKeyboard(); break;
  }
}

static void go(Page p) {
  const Page from = page;
  if (from == Page::Keyboard && p != Page::Keyboard) memset(kbText, 0, sizeof kbText);  // may be a password
  page = p;
  kbPressed = -1;
  if (from == Page::Main) listFirst = 0;
  if (p == Page::Wifi && from == Page::Main && !switching() && scanState != Scan::Running &&
      (scanState != Scan::Done || millis() - scannedAt > SCAN_STALE_MS)) {
    startScan();
  }
  if (p == Page::Nas && from == Page::Main) testName[0] = '\0';
  if (p == Page::Device && from == Page::Main) {
    confirming = Confirm::None;
    resetDone = false;
  }
  drawPage();
}

static void refresh() {
  // Collected on any page, so the scan's memory goes back as soon as it can
  const bool scanDone = scanFinished();
  String sw;
  switch (page) {
    case Page::Calibrate:
      if (calStep == 4 && millis() - calDoneAt >= CAL_DONE_MS) go(calNext);
      break;
    case Page::Main:
      for (int i = 0; i < TILE_COUNT; i++) drawTile(i, false);
      break;
    case Page::Wifi:
      if (scanDone) {
        drawWifiHeader();
        drawWifiList();
      }
      wifiStatus();
      break;
    case Page::Join:
      if ((int)net::switchStatus(sw) != joinShown) drawPage();
      break;
    case Page::Clock: clockStatus(); break;
    case Page::Nas: nasStatus(); break;
    case Page::Device: deviceRefresh(); break;
    case Page::Diagnostics:
      for (int i = 0; i < DIAG_ROWS; i++) diagnosticsLine(i);
      break;
    default: break;
  }
}

static void press(int x, int y) {
  switch (page) {
    case Page::Calibrate: break;
    case Page::Main: mainPress(x, y); break;
    case Page::Wifi: wifiPress(x, y); break;
    case Page::Join: joinPress(x, y); break;
    case Page::Bike:
    case Page::Screen: optionsPress(x, y); break;
    case Page::Clock: clockPress(x, y); break;
    case Page::Nas: nasPress(x, y); break;
    case Page::Device: devicePress(x, y); break;
    case Page::Diagnostics: diagnosticsPress(x, y); break;
    case Page::Picker: pickerPress(x, y); break;
    case Page::Keyboard: keyboardPress(x, y); break;
  }
}

static void hold() {
  if (page == Page::Keyboard) keyboardHold();
}

static void release() {
  if (page == Page::Keyboard && kbPressed >= 0) drawKey(kbPressed, false);
  kbPressed = -1;
}

// ---------------------------------------------------------------- public

void open() {
  opened = true;
  lastTouch = millis();
  waitRelease = true;
  calWasDown = false;
  if (touch::calibrated()) page = Page::Main;
  else startCalibration(Page::Main);
  logger::printf("menu: opened\n");
}

bool isOpen() { return opened; }

void close() {
  if (!opened) return;
  opened = false;
  memset(kbText, 0, sizeof kbText);
  memset(joinPass, 0, sizeof joinPass);
  logger::printf("menu: closed\n");
}

bool expired() { return opened && millis() - lastTouch > IDLE_MS; }

void draw(bool full) {
  if (!opened) {
    // A scan still running when the menu closed: free its results once it's done
    if (scanState == Scan::Running && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
      WiFi.scanDelete();
      scanState = Scan::None;
    }
    return;
  }
  if (full) drawPage();
  else refresh();
}

void poll(bool down) {
  static bool wasDown = false;
  static bool handled = false;  // this press has acted
  static bool haveLast = false;
  static int lastX = 0, lastY = 0;
  static uint32_t pressedAt = 0;

  if (down) lastTouch = millis();
  if (waitRelease) {
    if (!down) waitRelease = false;
    wasDown = down;
    return;
  }
  if (page == Page::Calibrate) {
    calibratePoll(down);
    if (calStep == 4 && millis() - calDoneAt >= CAL_DONE_MS) go(calNext);
    wasDown = down;
    return;
  }
  if (down && !wasDown) {
    pressedAt = millis();
    handled = haveLast = false;
  }
  if (down && !handled) {
    int x, y;
    if (touch::read(x, y)) {
      const bool steady = haveLast && abs(x - lastX) <= STEADY_PX && abs(y - lastY) <= STEADY_PX;
      if (steady || millis() - pressedAt >= STEADY_MS) {
        handled = true;
        press(x, y);
      }
      lastX = x;
      lastY = y;
      haveLast = true;
    }
  } else if (down) {
    hold();
  } else if (wasDown) {
    // A quick tap may lift before two readings agree; use the one it had
    if (!handled && haveLast) press(lastX, lastY);
    if (opened) release();
    handled = false;
  }
  wasDown = down;
}

void inject(int x, int y, bool raw) {
  if (!opened) return;
  lastTouch = millis();
  waitRelease = false;
  if (page == Page::Calibrate) {
    if (raw) calTap(x, y);
    return;
  }
  if (raw) return;
  press(x, y);
  if (opened) release();
}

}  // namespace menu
