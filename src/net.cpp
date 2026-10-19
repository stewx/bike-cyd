#include "net.h"

#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include "logger.h"

namespace net {

static constexpr uint32_t RETRY_MS = 30000;
// Halifax, NS: Atlantic time with DST. time() stays UTC for ride files.
static constexpr const char* TIME_ZONE = "AST4ADT,M3.2.0,M11.1.0";

static String ssid;
static String pass;
static uint32_t lastAttempt = 0;

static void connect() {
  WiFi.begin(ssid.c_str(), pass.c_str());
  lastAttempt = millis();
}

void begin() {
  Preferences prefs;
  if (prefs.begin("wifi", true)) {
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
    prefs.end();
  }
  if (ssid.isEmpty()) {
    logger::printf("net: no WiFi settings in NVS\n");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  connect();
  configTzTime(TIME_ZONE, "pool.ntp.org", "time.google.com");
}

void setTimeZone() {
  setenv("TZ", TIME_ZONE, 1);
  tzset();
}

void loop() {
  if (ssid.isEmpty() || WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastAttempt > RETRY_MS) {
    logger::printf("net: reconnecting\n");
    WiFi.disconnect();
    connect();
  }
}

Status status() {
  Status s;
  s.configured = !ssid.isEmpty();
  s.connected = WiFi.status() == WL_CONNECTED;
  s.ssid = ssid;
  s.ip = s.connected ? WiFi.localIP().toString() : String();
  s.rssi = s.connected ? WiFi.RSSI() : 0;
  return s;
}

bool timeSynced() {
  return time(nullptr) > 1700000000;  // after Nov 2023, so NTP has answered
}

}  // namespace net
