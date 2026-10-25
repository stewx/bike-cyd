#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "bike.h"
#include "board.h"
#include "logger.h"
#include "nas.h"
#include "net.h"
#include "ride.h"
#include "storage.h"
#include "touch.h"
#include "ui.h"

// Any watched task (main loop, bike, NAS upload) that stops checking in for
// this long restarts the board.
static constexpr uint32_t WATCHDOG_MS = 60000;
// A new firmware is kept only after it has run this long with WiFi and
// updates over WiFi working; a crash before that rolls back to the old one.
static constexpr uint32_t HEALTHY_AFTER_MS = 60000;
static constexpr uint32_t SERIAL_STATUS_MS = 10000;

// The smooth fonts put each glyph's bitmap on the stack (a 112 px speed digit
// is ~5 KB), which leaves the default 8 KB loop stack only ~2.6 KB spare.
SET_LOOP_TASK_STACK_SIZE(10 * 1024);
static constexpr uint32_t FILE_STATUS_MS = 5 * 60 * 1000;

static bool otaStarted = false;

// Tells the Arduino core not to accept a new OTA image at boot; it is
// accepted in checkHealthy() once the firmware has proven itself.
extern "C" bool verifyRollbackLater() { return true; }

static const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
  }
}

static unsigned stackLeft(const char* task) {
  TaskHandle_t h = xTaskGetHandle(task);
  return h ? (unsigned)uxTaskGetStackHighWaterMark(h) : 0;
}

static void logStatus(bool toFile) {
  net::Status ns = net::status();
  storage::Status ss = storage::status();
  bike::Data bd = bike::get();
  ride::Stats rs = ride::stats();
  char clock[32] = "no";
  if (net::timeSynced()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(clock, sizeof clock, "%Y-%m-%d %H:%M:%S %Z", &t);
  }
  char bike[160], status[256];
  snprintf(bike, sizeof bike,
           "bike: link=%s workout=%d speed=%.2f cad=%u watts=%u res=%u dist=%lu | ride active=%d paused=%d t=%lu\n",
           bike::linkText(bd.link), (int)bd.workout, bd.speedKmh, bd.cadenceRpm, bd.watts, bd.resistance,
           (unsigned long)bd.distanceM, rs.active, rs.paused, (unsigned long)rs.movingSeconds);
  snprintf(status, sizeof status,
           "status: wifi=%s ip=%s rssi=%d time=%s sd=%s free=%.2fGB heap=%u largest=%u minheap=%u "
           "stack loop=%u bike=%u nas=%u ldr=%d up=%lus\n",
           ns.connected ? "up" : "down", ns.ip.c_str(), ns.rssi, clock, ss.mounted ? ss.type : "none",
           ss.freeBytes / 1e9, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)esp_get_minimum_free_heap_size(),
           stackLeft("loopTask"), stackLeft("bike"), stackLeft("nas"), analogRead(PIN_LDR),
           (unsigned long)(millis() / 1000));
  if (toFile) {
    logger::fileOnly("%s", bike);
    logger::fileOnly("%s", status);
  } else {
    Serial.print(bike);
    Serial.print(status);
  }
}

// ------------------------------------------------------------------ OTA

static void startOta() {
  Preferences prefs;
  String pass;
  if (prefs.begin("ota", true)) {
    pass = prefs.getString("pass", "");
    prefs.end();
  }
  if (pass.isEmpty()) {
    logger::printf("ota: no password set, updates over WiFi are off\n");
    otaStarted = true;  // don't retry
    return;
  }
  ArduinoOTA.setHostname("bike-cyd");
  // mDNS (bike-cyd.local) costs ~9 KB of heap; updates go to the IP instead
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.setPassword(pass.c_str());
  ArduinoOTA.onStart([] {
    logger::printf("ota: update starting\n");
    logger::flush(true);
    ui::otaProgress(0);
  });
  ArduinoOTA.onProgress([](unsigned done, unsigned total) {
    esp_task_wdt_reset();
    ui::otaProgress(total ? done * 100 / total : 0);
  });
  ArduinoOTA.onEnd([] {
    logger::printf("ota: update received, restarting\n");
    logger::flush(true);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    logger::printf("ota: update failed (error %u)\n", (unsigned)e);
    ui::otaProgress(-1);
  });
  ArduinoOTA.begin();
  otaStarted = true;
  logger::printf("ota: ready at %s\n", WiFi.localIP().toString().c_str());
}

static void checkHealthy() {
  static bool done = false;
  if (done || millis() < HEALTHY_AFTER_MS || WiFi.status() != WL_CONNECTED || !otaStarted) return;
  done = true;
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    logger::printf("ota: new firmware %s confirmed\n", FW_VERSION);
  }
}

// ------------------------------------------------------------------ serial commands

static void setOtaPassword(const String& pass) {
  Preferences prefs;
  if (prefs.begin("ota", false)) {
    prefs.putString("pass", pass);
    prefs.end();
  }
  Serial.println("ota: password set, restart to apply");
}

static void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.startsWith("ls ")) storage::listDir(Serial, cmd.substring(3).c_str());
  else if (cmd.startsWith("get ")) storage::sendFile(Serial, cmd.substring(4).c_str());
  else if (cmd == "nas" || cmd.startsWith("nas ")) nas::command(Serial, cmd.substring(4));
  else if (cmd.startsWith("sim ")) bike::simulate(cmd.substring(4).toInt());
  else if (cmd.startsWith("rm ")) storage::remove(Serial, cmd.substring(3).c_str());
  else if (cmd.startsWith("mv ")) {
    int sp = cmd.indexOf(' ', 3);
    if (sp > 0) storage::move(Serial, cmd.substring(3, sp).c_str(), cmd.substring(sp + 1).c_str());
  } else if (cmd.startsWith("ota pass ")) {
    setOtaPassword(cmd.substring(9));
  } else if (cmd == "touchcal") {
    ui::calibrateTouch();
  } else if (cmd == "touch") {
    // Prints where each press lands, for 20 s, to check the calibration
    bool was = false;
    for (uint32_t start = millis(); millis() - start < 20000;) {
      esp_task_wdt_reset();
      int x, y;
      bool now = touch::touched();
      if (now && !was) {
        delay(20);
        if (touch::read(x, y)) Serial.printf("touch: %d,%d\n", x, y);
        else Serial.println("touch: pressed (not calibrated)");
      }
      was = now;
      delay(10);
    }
  } else if (cmd == "ldr") {
    // Calibration: 20 light sensor readings, half a second apart
    for (int i = 0; i < 20; i++) {
      Serial.printf("ldr: %d\n", analogRead(PIN_LDR));
      esp_task_wdt_reset();
      delay(500);
    }
  } else if (cmd == "hang") {
    // Test: freeze the main loop so the task watchdog restarts the board
    logger::printf("main: hanging on purpose to test the watchdog\n");
    logger::flush(true);
    for (;;) delay(1000);
  } else if (cmd == "reboot") {
    logger::printf("main: restart requested over serial\n");
    logger::flush(true);
    delay(100);
    ESP.restart();
  }
}

// 'S' dumps the screen (tools/screenshot.py); ":<command>\n" runs a command:
// ls/get/rm/mv (SD card, tools/sdfile.py), nas ..., sim <seconds>,
// ota pass <password>, touchcal, touch, ldr, hang, reboot
static void readSerial() {
  static String cmd;
  static bool inCmd = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (inCmd) {
      if (c == '\n') {
        handleCommand(cmd);
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
}

// ------------------------------------------------------------------ main

void setup() {
  Serial.begin(115200);
  // The RGB LED is active low; keep it off for now.
  for (int pin : {PIN_LED_R, PIN_LED_G, PIN_LED_B}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  touch::begin();
  // The light sensor only moves away from 0 on the most sensitive range
  analogSetPinAttenuation(PIN_LDR, ADC_0db);

  const esp_task_wdt_config_t wdt = {
      .timeout_ms = WATCHDOG_MS, .idle_core_mask = (1 << 0) | (1 << 1), .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(nullptr);  // this (main loop) task

  net::setTimeZone();
  Serial.println();
  // The ELF hash tells builds apart even when FW_VERSION hasn't changed
  char build[9];
  esp_app_get_elf_sha256(build, sizeof build);
  logger::printf("bike-cyd %s (build %s, %s) starting (reset: %s)\n", FW_VERSION, build,
                 esp_ota_get_running_partition()->label, resetReason());

  ui::begin();
  storage::begin();
  ride::begin();
  net::begin();
  bike::start();
  nas::start();
}

void loop() {
  static uint32_t lastDraw = 0;
  static uint32_t lastSerialStatus = 0;
  static uint32_t lastFileStatus = 0;

  esp_task_wdt_reset();
  net::loop();
  storage::loop();
  readSerial();

  if (!otaStarted && WiFi.status() == WL_CONNECTED) startOta();
  if (otaStarted) ArduinoOTA.handle();
  checkHealthy();

  static bool wasTouched = false;
  static uint32_t lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 30) {
    lastTouchCheck = millis();
    bool touched = touch::touched();
    if (touched && !wasTouched) {
      int x = 0, y = 0;
      delay(20);  // let the press settle before reading where it is
      bool havePosition = touch::read(x, y);
      ui::press(havePosition, x, y);
    } else if (touched) {
      ui::wake();
    }
    wasTouched = touched;
  }

  uint32_t now = millis();
  if (now - lastDraw >= 1000) {
    lastDraw = now;
    ride::tick(bike::get());
    ui::draw();
  }
  if (now - lastSerialStatus >= SERIAL_STATUS_MS) {
    lastSerialStatus = now;
    logStatus(false);
  }
  if (now - lastFileStatus >= FILE_STATUS_MS) {
    lastFileStatus = now;
    logStatus(true);
  }
  logger::flush();
  delay(10);
}
