#include "ui.h"

#include <esp_heap_caps.h>
#include <time.h>

#include "display.h"
#include "net.h"
#include "storage.h"

namespace ui {

static LGFX lcd;

static constexpr int W = 320;
static constexpr int BAR_H = 24;
static constexpr int ROW_Y = BAR_H + 10;
static constexpr int ROW_H = 22;
static constexpr int VALUE_X = 92;
static constexpr int TEST_Y = 214;

static uint16_t C_BG, C_BAR, C_TEXT, C_DIM, C_OK, C_WARN, C_BAD;

static void barText(const char* text, int x, textdatum_t datum, int pad, uint16_t color) {
  lcd.setTextDatum(datum);
  lcd.setTextColor(color, C_BAR);
  lcd.setTextPadding(pad);
  lcd.drawString(text, x, BAR_H / 2);
}

static void row(int i, const char* label, const char* value, uint16_t color) {
  int y = ROW_Y + i * ROW_H;
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextPadding(0);
  lcd.setTextColor(C_DIM, C_BG);
  lcd.drawString(label, 8, y);
  lcd.setTextColor(color, C_BG);
  lcd.setTextPadding(W - VALUE_X - 4);
  lcd.drawString(value, VALUE_X, y);
}

// Three labelled bars so the panel's colour order and inversion can be checked by eye.
static void drawColourTest() {
  const int w = W / 3;
  const uint16_t colours[] = {lcd.color565(255, 0, 0), lcd.color565(0, 255, 0), lcd.color565(0, 0, 255)};
  const char* names[] = {"RED", "GREEN", "BLUE"};
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextPadding(0);
  for (int i = 0; i < 3; i++) {
    lcd.fillRect(i * w, TEST_Y, w + (i == 2 ? W % 3 : 0), 240 - TEST_Y, colours[i]);
    lcd.setTextColor(lcd.color565(0, 0, 0), colours[i]);
    lcd.drawString(names[i], i * w + w / 2, TEST_Y + (240 - TEST_Y) / 2);
  }
}

void begin() {
  lcd.init();
  lcd.setRotation(1);  // landscape, 320x240
  lcd.setBrightness(255);
  lcd.setFont(&fonts::Font2);

  C_BG = lcd.color565(0, 0, 0);
  C_BAR = lcd.color565(32, 32, 48);
  C_TEXT = lcd.color565(230, 230, 230);
  C_DIM = lcd.color565(140, 140, 150);
  C_OK = lcd.color565(80, 220, 120);
  C_WARN = lcd.color565(250, 200, 60);
  C_BAD = lcd.color565(250, 90, 80);

  lcd.fillScreen(C_BG);
  lcd.fillRect(0, 0, W, BAR_H, C_BAR);
  drawColourTest();
}

void draw() {
  char buf[64];
  net::Status ns = net::status();
  storage::Status ss = storage::status();
  bool synced = net::timeSynced();
  struct tm t = {};
  if (synced) {
    time_t now = time(nullptr);
    localtime_r(&now, &t);
  }

  // Status bar: WiFi | clock | SD
  if (ns.connected) {
    snprintf(buf, sizeof buf, "WiFi %d dBm", ns.rssi);
  } else {
    snprintf(buf, sizeof buf, ns.configured ? "WiFi ..." : "WiFi off");
  }
  barText(buf, 6, textdatum_t::middle_left, 100, ns.connected ? C_OK : C_WARN);

  if (synced) strftime(buf, sizeof buf, "%H:%M:%S %Z", &t);
  else snprintf(buf, sizeof buf, "--:--:--");
  barText(buf, W / 2, textdatum_t::middle_center, 100, C_TEXT);

  if (ss.mounted) snprintf(buf, sizeof buf, "SD %.1fG", ss.freeBytes / 1e9);
  else snprintf(buf, sizeof buf, "No SD");
  barText(buf, W - 6, textdatum_t::middle_right, 80, ss.mounted ? C_OK : C_BAD);

  // Status rows
  if (!ns.configured) row(0, "WiFi", "no settings stored", C_BAD);
  else if (ns.connected) row(0, "WiFi", ns.ssid.c_str(), C_OK);
  else {
    snprintf(buf, sizeof buf, "connecting to %s", ns.ssid.c_str());
    row(0, "WiFi", buf, C_WARN);
  }

  row(1, "IP", ns.connected ? ns.ip.c_str() : "-", C_TEXT);

  if (synced) {
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M %Z", &t);
    row(2, "Clock", buf, C_TEXT);
  } else {
    row(2, "Clock", "waiting for NTP", C_WARN);
  }

  if (ss.mounted) {
    snprintf(buf, sizeof buf, "%s, %.1f GB free of %.1f GB", ss.type, ss.freeBytes / 1e9, ss.totalBytes / 1e9);
    row(3, "SD card", buf, C_OK);
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

  row(6, "Bike", "not set up yet (step 2)", C_DIM);
  row(7, "Firmware", FW_VERSION, C_DIM);
}

}  // namespace ui
