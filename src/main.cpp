#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "board.h"
#include "net.h"
#include "storage.h"
#include "ui.h"

static void logStatus() {
  net::Status ns = net::status();
  storage::Status ss = storage::status();
  char clock[32] = "no";
  if (net::timeSynced()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(clock, sizeof clock, "%Y-%m-%d %H:%M:%S %Z", &t);
  }
  Serial.printf("status: wifi=%s ip=%s rssi=%d time=%s sd=%s free=%.2fGB heap=%u largest=%u\n",
                ns.connected ? "up" : "down", ns.ip.c_str(), ns.rssi, clock,
                ss.mounted ? ss.type : "none", ss.freeBytes / 1e9, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void setup() {
  Serial.begin(115200);
  // The RGB LED is active low; keep it off for now.
  for (int pin : {PIN_LED_R, PIN_LED_G, PIN_LED_B}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  Serial.printf("\nbike-cyd %s\n", FW_VERSION);

  ui::begin();
  storage::begin();
  net::begin();
}

void loop() {
  static uint32_t lastDraw = 0;
  static uint32_t lastLog = 0;

  net::loop();
  storage::loop();

  uint32_t now = millis();
  if (now - lastDraw >= 1000) {
    lastDraw = now;
    ui::draw();
  }
  if (now - lastLog >= 10000) {
    lastLog = now;
    logStatus();
  }
  delay(10);
}
