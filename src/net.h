#pragma once

#include <Arduino.h>

namespace net {

struct Status {
  bool configured;  // WiFi settings found in NVS
  bool connected;
  String ssid;
  String ip;
  int rssi;
};

// Sets the local time zone (settings, Halifax by default). Call at boot
// before logging: the clock keeps running across a restart, so log
// timestamps need it before WiFi is up. Call again when it changes.
void setTimeZone();
// Reads WiFi settings from NVS (namespace "wifi", keys "ssid"/"pass") and connects.
void begin();
// Retries the connection if it has been down too long.
void loop();
Status status();
bool timeSynced();

enum class Switch { None, Trying, Joined, Failed };
// Moves to another network (on the next loop()). The new settings are
// saved only once it connects; if it can't within 20 s, the old network
// comes back.
void switchTo(const String& ssid, const String& pass);
// How the last switchTo() went, and which network it was for
Switch switchStatus(String& ssid);
const String& savedPassword();

}  // namespace net
