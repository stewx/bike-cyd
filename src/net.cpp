#include "net.h"

#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include "logger.h"
#include "settings.h"

namespace net {

static constexpr uint32_t RETRY_MS = 30000;
// A new network has this long to connect before the old one comes back
static constexpr uint32_t SWITCH_TIMEOUT_MS = 20000;

static String ssid;
static String pass;
static uint32_t lastAttempt = 0;

static Switch switchState = Switch::None;
static bool switchPending = false;
static String newSsid, newPass;
static uint32_t switchAt = 0;
static bool sawDown = false;  // the old link has dropped, so "connected" means the new one

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
  configTzTime(settings::get().timeZone, "pool.ntp.org", "time.google.com");
}

void setTimeZone() {
  setenv("TZ", settings::get().timeZone, 1);
  tzset();
}

void switchTo(const String& toSsid, const String& toPass) {
  newSsid = toSsid;
  newPass = toPass;
  switchPending = true;
  switchState = Switch::Trying;
}

Switch switchStatus(String& toSsid) {
  toSsid = newSsid;
  return switchState;
}

const String& savedPassword() { return pass; }

static void switchLoop() {
  if (switchPending) {
    switchPending = false;
    logger::printf("net: trying %s\n", newSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.begin(newSsid.c_str(), newPass.c_str());
    switchAt = millis();
    sawDown = false;
    return;
  }
  bool up = WiFi.status() == WL_CONNECTED;
  if (!up) sawDown = true;
  if (up && sawDown) {
    Preferences prefs;
    if (prefs.begin("wifi", false)) {
      prefs.putString("ssid", newSsid);
      prefs.putString("pass", newPass);
      prefs.end();
    }
    bool first = ssid.isEmpty();
    ssid = newSsid;
    pass = newPass;
    switchState = Switch::Joined;
    logger::printf("net: joined %s (%s), saved\n", ssid.c_str(), WiFi.localIP().toString().c_str());
    if (first) configTzTime(settings::get().timeZone, "pool.ntp.org", "time.google.com");
  } else if (millis() - switchAt > SWITCH_TIMEOUT_MS) {
    switchState = Switch::Failed;
    logger::printf("net: could not join %s, going back to %s\n", newSsid.c_str(), ssid.c_str());
    WiFi.disconnect();
    if (!ssid.isEmpty()) connect();
  }
}

void loop() {
  if (switchState == Switch::Trying) {
    switchLoop();
    return;
  }
  if (ssid.isEmpty() || WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastAttempt > RETRY_MS) {
    logger::printf("net: reconnecting\n");
    WiFi.disconnect();
    connect();
  }
}

Status status() {
  Status s;
  bool trying = switchState == Switch::Trying;
  s.configured = !ssid.isEmpty() || trying;
  s.connected = !trying && WiFi.status() == WL_CONNECTED;
  s.ssid = trying ? newSsid : ssid;
  s.ip = s.connected ? WiFi.localIP().toString() : String();
  s.rssi = s.connected ? WiFi.RSSI() : 0;
  return s;
}

bool timeSynced() {
  return time(nullptr) > 1700000000;  // after Nov 2023, so NTP has answered
}

}  // namespace net
