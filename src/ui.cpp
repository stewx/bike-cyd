#include "ui.h"

#include <esp_heap_caps.h>
#include <time.h>

#include "bike.h"
#include "draw.h"
#include "history.h"
#include "logger.h"
#include "menu.h"
#include "nas.h"
#include "net.h"
#include "ride.h"
#include "settings.h"
#include "storage.h"
#include "tcx.h"
#include "touch.h"

// Ride screen layout is design C ("big and simple") from the Ride Screen
// Designs canvas: large speed, distance and time cards, one line of
// secondary metrics. Fonts and icons come from tools/makefonts.py.

namespace ui {

using namespace draw;

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

// Home screen (no status bar)
static constexpr int CLOCK_BASELINE = 111;
static constexpr int STATUS_Y = 131;  // the bike's state, centre line
static constexpr int HOME_CARD_Y = 152;
static constexpr int HOME_CARD_H = 80;
static constexpr uint32_t WIFI_GRACE_MS = 2 * 60 * 1000;  // WiFi down this long counts as a problem

// Paused screen (design P1): header, two cards, one big button
static constexpr int P_CARD_Y = 92;
static constexpr int P_CARD_H = 50;
static constexpr int P_CARD_W = 148;
static constexpr int BTN_X = 8;
static constexpr int BTN_Y = 150;
static constexpr int BTN_W = 304;
static constexpr int BTN_H = 72;
static constexpr uint32_t CONFIRM_MS = 5000;

enum class Screen { None, Idle, Ride, Paused, Saved, Settings };
static Screen shown = Screen::None;
static uint32_t lastActivity = 0;  // for the backlight timeout
static bool shownPaused = false;

static bool confirming = false;    // paused screen: first tap done, waiting for the second
static uint32_t confirmUntil = 0;
static int drawnButton = -1;       // which button face is on screen (-1 none, 0 end, 1 confirm)
static uint32_t dismissedFor = 0;  // endedMs of the ride whose saved screen was tapped away
static int drawnSavedState = -1;

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

// "Rides need 2 min and 0.5 km", from the settings
static String minimumText() {
  const settings::Values cfg = settings::get();
  char time[16] = "", dist[16] = "", buf[48];
  if (cfg.minRideSec % 60 == 0) snprintf(time, sizeof time, "%u min", cfg.minRideSec / 60);
  else snprintf(time, sizeof time, "%u s", cfg.minRideSec);
  if (cfg.minRideM % 100 == 0) snprintf(dist, sizeof dist, "%.1f km", cfg.minRideM / 1000.0f);
  else snprintf(dist, sizeof dist, "%.2f km", cfg.minRideM / 1000.0f);
  if (cfg.minRideSec && cfg.minRideM) snprintf(buf, sizeof buf, "Rides need %s and %s", time, dist);
  else snprintf(buf, sizeof buf, "Rides need %s", cfg.minRideSec ? time : dist);
  return buf;
}

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
      line2 = minimumText();
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

// ---------------------------------------------------------------- home screen

// Date, a big clock, the bike's state in words, and a card with the last
// ride and this week's distance, which becomes a warning when something
// needs attention. Status icons stay grey while all is well. The whole
// screen opens the settings (so it works before the touch setup); the gear
// is the hint.

enum HomePart { H_DATE, H_ICONS, H_CLOCK, H_STATUS, H_CARD, H_PARTS };
static char homeDrawn[H_PARTS][128];  // what each part shows, so only changes are redrawn
static uint32_t wifiDownSince = 0;

static bool homeChanged(HomePart part, const char* key) {
  if (strcmp(homeDrawn[part], key) == 0) return false;
  strlcpy(homeDrawn[part], key, sizeof homeDrawn[part]);
  return true;
}

// Small capitals with extra space between the letters; returns where it ends
static int spaced(const char* s, int x, int y, SmoothFont& f, uint16_t fg, uint16_t bg) {
  char c[2] = {0, 0};
  for (; *s; s++) {
    c[0] = *s;
    text(c, x, y, textdatum_t::middle_left, f, fg, bg);
    x += textWidth(c, f) + 1;
  }
  return x - 1;
}

// "Today", "Yesterday" or "Sat 25 Oct"
static void formatDay(time_t t, char* buf, size_t len) {
  const time_t now = time(nullptr), dayAfter = t + 86400;
  struct tm day, today, next;
  localtime_r(&t, &day);
  localtime_r(&now, &today);
  localtime_r(&dayAfter, &next);
  if (day.tm_year == today.tm_year && day.tm_yday == today.tm_yday) {
    snprintf(buf, len, "Today");
  } else if (next.tm_year == today.tm_year && next.tm_yday == today.tm_yday) {
    snprintf(buf, len, "Yesterday");
  } else {
    char wd[8], mon[8];
    strftime(wd, sizeof wd, "%a", &day);
    strftime(mon, sizeof mon, "%b", &day);
    snprintf(buf, len, "%s %d %s", wd, day.tm_mday, mon);
  }
}

struct Problem {
  bool bad;  // red rather than amber
  const char* icon;
  char title[48];
  char body[128];
};

// The most important thing that needs attention, if anything does
static bool findProblem(const net::Status& ns, const storage::Status& ss, const nas::Status& us, Problem& p) {
  p = {};
  if (!ss.mounted) {
    p.bad = true;
    p.icon = icon::SD;
    snprintf(p.title, sizeof p.title, "No SD card");
    snprintf(p.body, sizeof p.body, "Rides can't be recorded without one. Put a card in and it's picked up by itself.");
    return true;
  }
  p.icon = icon::WIFI_OFF;
  if (!ns.configured) {
    snprintf(p.title, sizeof p.title, "WiFi isn't set up");
    snprintf(p.body, sizeof p.body, "Rides are kept on the SD card. Choose a network in Settings, WiFi.");
    return true;
  }
  if (wifiDownSince && millis() - wifiDownSince >= WIFI_GRACE_MS) {
    snprintf(p.title, sizeof p.title, "WiFi not connected");
    snprintf(p.body, sizeof p.body, "Still trying %s. Rides are kept on the SD card until it's back.", ns.ssid.c_str());
    return true;
  }
  if (us.state == nas::State::NotConfigured) {
    p.icon = icon::CLOUD_OFF;
    snprintf(p.title, sizeof p.title, "NAS isn't set up");
    snprintf(p.body, sizeof p.body, "Rides are kept on the SD card. Set the share in Settings, NAS.");
    return true;
  }
  if (us.state == nas::State::Waiting && us.pending) {
    p.icon = icon::CLOUD_ALERT;
    snprintf(p.title, sizeof p.title, us.pending == 1 ? "1 ride waiting to upload" : "%d rides waiting to upload",
             us.pending);
    snprintf(p.body, sizeof p.body, "The NAS isn't answering. They're safe on the SD card and go up when it's back.");
    return true;
  }
  return false;
}

static void drawProblemCard(const Problem& p) {
  const uint16_t bg = p.bad ? C_BAD_BG : C_CONFIRM_BG, fg = p.bad ? C_BAD : C_AMBER;
  fillRect(8, HOME_CARD_Y, W - 16, HOME_CARD_H, C_BG);
  roundRect(8, HOME_CARD_Y, W - 16, HOME_CARD_H, 12, bg);
  text(p.icon, 22, HOME_CARD_Y + 20, textdatum_t::middle_left, fIcon20, fg, bg);
  text(p.title, 52, HOME_CARD_Y + 20, textdatum_t::middle_left, fLabel15, fg, bg);
  wrap(p.body, 52, HOME_CARD_Y + 33, W - 74, 15, fLabel12, p.bad ? C_SOFT : C_CONFIRM_SUB, bg, 3);
}

static void drawRideCard(bool have, const history::Ride& r, const history::Week& w) {
  const int headY = HOME_CARD_Y + 17, valueY = HOME_CARD_Y + 60, iconY = HOME_CARD_Y + 51;
  fillRect(8, HOME_CARD_Y, W - 16, HOME_CARD_H, C_BG);
  roundRect(8, HOME_CARD_Y, W - 16, HOME_CARD_H, 12, C_CARD);
  char buf[48];
  int x = spaced("LAST RIDE", 22, headY, fLabel12, C_DIM, C_CARD);
  if (have) {
    char day[24];
    formatDay(r.startEpoch, day, sizeof day);
    snprintf(buf, sizeof buf, " \xC2\xB7 %s", day);
    text(buf, x, headY, textdatum_t::middle_left, fLabel12, C_DIM, C_CARD);
  }
  if (w.rides) {
    snprintf(buf, sizeof buf, "%.1f km", w.distanceM / 1000.0f);
    text(buf, W - 22, headY, textdatum_t::middle_right, fLabel12, C_SOFT, C_CARD);
    text("This week  ", W - 22 - textWidth(buf, fLabel12), headY, textdatum_t::middle_right, fLabel12, C_DIM, C_CARD);
  }
  if (!have) {
    text("Your next ride will show up here", W / 2, iconY, textdatum_t::middle_center, fLabel13, C_DIM, C_CARD);
    return;
  }

  // Distance, moving time and average speed, spread across the card
  char values[3][16];
  snprintf(values[0], sizeof values[0], "%.2f", r.distanceM / 1000.0f);
  formatDuration(values[1], sizeof values[1], r.movingS);
  snprintf(values[2], sizeof values[2], "%.1f", r.movingS ? r.distanceM * 3.6f / r.movingS : 0.0f);
  const char* icons[3] = {icon::DISTANCE, icon::TIME, icon::AVERAGE};
  const uint16_t colors[3] = {C_GREEN, C_VIOLET, C_CYAN};
  const char* units[3] = {"km", "", "km/h"};
  int widths[3], total = 0;
  for (int i = 0; i < 3; i++) {
    widths[i] = 24 + textWidth(values[i], fCard) + (units[i][0] ? 4 + textWidth(units[i], fLabel13) : 0);
    total += widths[i];
  }
  const int gap = (W - 44 - total) / 2;
  x = 22;
  for (int i = 0; i < 3; i++) {
    text(icons[i], x, iconY, textdatum_t::middle_left, fIcon18, colors[i], C_CARD);
    text(values[i], x + 24, valueY, textdatum_t::baseline_left, fCard, C_TEXT, C_CARD);
    if (units[i][0]) {
      text(units[i], x + 28 + textWidth(values[i], fCard), valueY, textdatum_t::baseline_left, fLabel13, C_DIM,
           C_CARD);
    }
    x += widths[i] + gap;
  }
}

static void drawHomeStatic() {
  memset(homeDrawn, 0, sizeof homeDrawn);
  roundRect(W - 46, 6, 38, 32, 8, C_CARD);
  text(icon::SETTINGS, W - 27, 22, textdatum_t::middle_center, fIcon18, C_SOFT, C_CARD);
}

static void drawHome(const net::Status& ns, const storage::Status& ss, const bike::Data& bd, const nas::Status& us) {
  char key[128];
  const bool synced = net::timeSynced();
  const time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  // Date
  key[0] = '\0';
  if (synced) {
    char wd[12], mon[12];
    strftime(wd, sizeof wd, "%A", &t);
    strftime(mon, sizeof mon, "%B", &t);
    snprintf(key, sizeof key, "%s %d %s", wd, t.tm_mday, mon);
  }
  if (homeChanged(H_DATE, key)) {
    fillRect(0, 10, 200, 26, C_BG);
    text(key, 16, 23, textdatum_t::middle_left, fLabel15, C_SOFT, C_BG);
  }

  // WiFi and upload icons, grey while all is well
  const char* wifi = ns.connected ? icon::WIFI : icon::WIFI_OFF;
  const uint16_t wifiColor = ns.connected ? C_DIM : C_AMBER;
  const char* cloud = icon::CLOUD_OK;
  uint16_t cloudColor = C_DIM;
  if (us.state == nas::State::Uploading || (us.state == nas::State::Idle && us.pending)) {
    cloud = icon::CLOUD_UP;
    cloudColor = C_BLUE;
  } else if (us.state == nas::State::Waiting || us.state == nas::State::NotConfigured) {
    cloud = us.state == nas::State::Waiting ? icon::CLOUD_ALERT : icon::CLOUD_OFF;
    cloudColor = C_AMBER;
  } else if (us.state == nas::State::Offline) {
    cloud = icon::CLOUD_OFF;
  }
  snprintf(key, sizeof key, "%s%u%s%u", wifi, wifiColor, cloud, cloudColor);
  if (homeChanged(H_ICONS, key)) {
    fillRect(W - 112, 10, 62, 26, C_BG);
    text(cloud, W - 56, 22, textdatum_t::middle_right, fIcon18, cloudColor, C_BG);
    text(wifi, W - 84, 22, textdatum_t::middle_right, fIcon18, wifiColor, C_BG);
  }

  // Clock
  if (synced) strftime(key, sizeof key, "%H:%M", &t);
  else snprintf(key, sizeof key, "--:--");
  if (homeChanged(H_CLOCK, key)) {
    // The padding box spans the font's full height; keep it between the date and the status line
    clip(0, 42, 250, STATUS_Y - 14 - 42);
    text(key, 12, CLOCK_BASELINE, textdatum_t::baseline_left, fClock, C_TEXT, C_BG, 236);
    noClip();
  }

  // The bike, in words
  const char* ic = icon::MOON;
  uint16_t color = C_SOFT, iconColor = C_DIM;
  const char* state = "Bike asleep";
  const char* hint = "wakes when you start";
  if (bd.link == bike::Link::Connected) {
    ic = icon::BIKE;
    color = iconColor = C_GREEN;
    state = "Ready to ride";
    hint = "start pedalling";
  } else if (bd.link == bike::Link::Connecting || bd.link == bike::Link::Initializing) {
    ic = icon::BLUETOOTH;
    color = iconColor = C_BLUE;
    state = "Connecting to the bike...";
    hint = "";
  }
  snprintf(key, sizeof key, "%s|%s", state, hint);
  if (homeChanged(H_STATUS, key)) {
    fillRect(0, STATUS_Y - 13, W, 26, C_BG);
    text(ic, 16, STATUS_Y, textdatum_t::middle_left, fIcon20, iconColor, C_BG);
    text(state, 44, STATUS_Y, textdatum_t::middle_left, fLabel15, color, C_BG);
    if (hint[0]) {
      char buf[48];
      snprintf(buf, sizeof buf, " \xC2\xB7 %s", hint);
      text(buf, 44 + textWidth(state, fLabel15), STATUS_Y, textdatum_t::middle_left, fLabel15, C_DIM, C_BG);
    }
  }

  // Last ride, or what needs attention
  Problem p;
  if (findProblem(ns, ss, us, p)) {
    snprintf(key, sizeof key, "P%s|%s", p.title, p.body);
    if (homeChanged(H_CARD, key)) drawProblemCard(p);
  } else {
    history::Ride r;
    const bool have = history::last(r);
    const history::Week w = history::thisWeek();
    char day[24] = "";
    if (have) formatDay(r.startEpoch, day, sizeof day);
    snprintf(key, sizeof key, "R%lu|%lu|%s|%u|%lu", (unsigned long)r.distanceM, (unsigned long)r.movingS, day,
             w.rides, (unsigned long)w.distanceM);
    if (homeChanged(H_CARD, key)) drawRideCard(have, r, w);
  }
}

// ---------------------------------------------------------------- screens

// The ride screens have the status bar; home and the settings use the whole screen
static bool hasBar(Screen s) { return s == Screen::Ride || s == Screen::Paused || s == Screen::Saved; }

// Switches screens; false if s was already showing
static bool showScreen(Screen s) {
  if (s == shown) return false;
  const Screen was = shown;
  shown = s;
  if (s == Screen::Settings) {
    menu::draw(true);
    return true;
  }
  if (hasBar(s) && hasBar(was)) {
    fillRect(0, BAR_H, W, H - BAR_H, C_BG);
  } else {
    fillRect(0, 0, W, H, C_BG);
    if (hasBar(s)) drawBarFrame();
  }
  if (s == Screen::Idle) {
    drawHomeStatic();
  } else if (s == Screen::Ride) {
    shownPaused = true;  // forces drawSpeed to lay out the speed area
    drawRideStatic();
  } else if (s == Screen::Paused) {
    confirming = false;
    drawPausedStatic();
  } else if (s == Screen::Saved) {
    drawnSavedState = -1;
  }
  return true;
}

void begin() {
  draw::begin();
  lastActivity = millis();
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
  const ride::Summary last = ride::lastRide();
  // A ride takes over from the settings screens, and so does a while untouched
  if (menu::isOpen() && (s.rs.active || menu::expired())) menu::close();

  Screen want = Screen::Idle;
  if (s.rs.active) {
    want = s.rs.paused ? Screen::Paused : Screen::Ride;
  } else if (menu::isOpen()) {
    want = Screen::Settings;
  } else if (last.valid && last.endedMs != dismissedFor &&
             millis() - last.endedMs < settings::get().savedMin * 60000UL) {
    want = Screen::Saved;
  }
  // How long WiFi has been down, for the home screen's warning
  if (s.ns.connected || !s.ns.configured) wifiDownSince = 0;
  else if (!wifiDownSince) wifiDownSince = millis();

  const bool fresh = showScreen(want);
  if (want == Screen::Settings) {
    if (!fresh) menu::draw(false);
    return;
  }
  if (hasBar(want)) drawBar(s.ns, s.ss, s.bd, s.us);
  switch (want) {
    case Screen::Paused: drawPaused(s.rs); break;
    case Screen::Ride: drawRide(s.bd, s.rs); break;
    case Screen::Saved: drawSaved(last, s.us); break;
    default: drawHome(s.ns, s.ss, s.bd, s.us); break;
  }
}

// ---------------------------------------------------------------- backlight

// Full brightness for settings fullMin after activity, then dimmed to
// dimPct, then off after offMin (never if 0)
static constexpr uint8_t FULL = 255;
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
  const settings::Values cfg = settings::get();
  float darkness = cfg.nightDim ? constrain(ambient / DARK_READING, 0.0f, 1.0f) : 0.0f;
  int full = FULL - (int)(darkness * (FULL - NIGHT_FULL));
  int dimmed = max(1, FULL * cfg.dimPct / 100);

  uint32_t idle = millis() - lastActivity;
  int level = idle < cfg.fullMin * 60000UL                  ? full
              : cfg.offMin == 0 || idle < cfg.offMin * 60000UL ? min(dimmed, full)
                                                               : 0;
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
  if (!menu::isOpen()) menu::draw(false);  // tidies up after the settings screens
}

// ---------------------------------------------------------------- touch

// A tap on a screen that doesn't need its position
static void press() {
  if (shown == Screen::Saved) {
    dismissedFor = ride::lastRide().endedMs;
    draw();
  } else if (shown == Screen::Paused) {
    // A tap anywhere counts: the button is most of the screen, and the
    // second tap is what guards against a bump ending the ride
    if (!confirming) {
      confirming = true;
      confirmUntil = millis() + CONFIRM_MS;
      drawButton(true);
    } else {
      confirming = false;
      ride::endNow();
      draw();
    }
  } else if (shown == Screen::Idle) {
    menu::open();
    draw();
  }
}

void pollTouch() {
  static bool wasDown = false;
  static bool wakeOnly = false;  // this press landed on a dimmed screen
  const bool down = touch::touched();
  const bool pressed = down && !wasDown;
  wasDown = down;
  if (pressed) wakeOnly = millis() - lastActivity >= settings::get().fullMin * 60000UL;
  if (down) wake();
  if (wakeOnly) {
    // A touch on a dimmed screen only wakes it
    if (!down) wakeOnly = false;
    return;
  }
  if (shown == Screen::Settings) {
    menu::poll(down);
    if (!menu::isOpen()) draw();
  } else if (pressed) {
    press();
  }
}

void tap(int x, int y, bool raw) {
  wake();
  if (shown == Screen::Settings) {
    menu::inject(x, y, raw);
    if (!menu::isOpen()) draw();
  } else {
    press();
  }
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
  // 10 rows = 6.4 KB, small enough to find even right after an upload or
  // during a WiFi scan
  static constexpr int BAND = 10;
  LGFX_Sprite strip(&lcd);
  strip.setColorDepth(16);
  if (!strip.createSprite(W, BAND)) {
    out.print("SHOT 0 0\n");
    return;
  }

  bool savedPaused = shownPaused;
  const Snapshot snap = snapshot();  // every band shows the same moment
  static uint8_t line[W * 3];        // R, G, B per pixel

  // Rows go out as text lines ("R<y> <base64> <sum>") so log output from
  // other tasks landing in the stream can't corrupt the image.
  // Each line is built in a static buffer and written in one go, as printf
  // would allocate a copy of it every row.
  static constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static char row[16 + (sizeof line + 2) / 3 * 4 + 8];

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
      size_t n = snprintf(row, sizeof row, "R%d ", top + y);
      for (size_t i = 0; i < sizeof line; i += 3) {
        uint32_t v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        sum += b[i] + b[i + 1] + b[i + 2];
        row[n++] = B64[(v >> 18) & 63];
        row[n++] = B64[(v >> 12) & 63];
        row[n++] = B64[(v >> 6) & 63];
        row[n++] = B64[v & 63];
      }
      n += snprintf(row + n, sizeof row - n, " %04x\n", sum);
      out.write((const uint8_t*)row, n);
    }
  }
  out.print("END\n");

  g = &lcd;
  oy = 0;
  // Home and the settings screens remember what's on the panel so they
  // redraw only changes; drawing into the strip spoiled that, so the
  // screen is drawn again
  shown = Screen::None;
  shownPaused = savedPaused;
  strip.deleteSprite();
}

}  // namespace ui
