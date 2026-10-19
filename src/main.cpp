#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "bike.h"
#include "board.h"
#include "net.h"
#include "onedrive.h"
#include "ride.h"
#include "storage.h"
#include "ui.h"

static void logStatus() {
  net::Status ns = net::status();
  storage::Status ss = storage::status();
  bike::Data bd = bike::get();
  ride::Stats rs = ride::stats();
  Serial.printf("bike: link=%s workout=%d speed=%.2f cad=%u watts=%u res=%u dist=%lu | ride active=%d paused=%d t=%lu\n",
                bike::linkText(bd.link), (int)bd.workout, bd.speedKmh, bd.cadenceRpm, bd.watts, bd.resistance,
                (unsigned long)bd.distanceM, rs.active, rs.paused, (unsigned long)rs.movingSeconds);
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
  bike::start();
  onedrive::start();
}

void loop() {
  static uint32_t lastDraw = 0;
  static uint32_t lastLog = 0;

  net::loop();
  storage::loop();

  // Serial commands: 'S' dumps the screen (tools/screenshot.py);
  // ":ls <dir>" and ":get <file>" read the SD card (tools/sdfile.py);
  // ":od ..." controls the OneDrive uploader
  static String cmd;
  static bool inCmd = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (inCmd) {
      if (c == '\n') {
        cmd.trim();
        if (cmd.startsWith("ls ")) storage::listDir(Serial, cmd.substring(3).c_str());
        else if (cmd.startsWith("get ")) storage::sendFile(Serial, cmd.substring(4).c_str());
        else if (cmd == "od" || cmd.startsWith("od ")) onedrive::command(Serial, cmd.substring(3));
        inCmd = false;
      } else if (cmd.length() < 120) {
        cmd += c;
      }
    } else if (c == ':') {
      inCmd = true;
      cmd = "";
    } else if (c == 'S') {
      ui::screenshot(Serial);
    }
  }

  uint32_t now = millis();
  if (now - lastDraw >= 1000) {
    lastDraw = now;
    ride::tick(bike::get());
    ui::draw();
  }
  if (now - lastLog >= 10000) {
    lastLog = now;
    logStatus();
  }
  delay(10);
}
