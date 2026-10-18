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

// Reads WiFi settings from NVS (namespace "wifi", keys "ssid"/"pass") and connects.
void begin();
// Retries the connection if it has been down too long.
void loop();
Status status();
bool timeSynced();

}  // namespace net
