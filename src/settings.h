#pragma once

#include <stddef.h>
#include <stdint.h>

// Options changed from the settings screens, stored in NVS namespace
// "settings". Anything not stored there uses its default.
namespace settings {

struct Values {
  char bikeName[32];    // Bluetooth name the console advertises
  uint16_t releaseMin;  // disconnect from the bike after this long without riding
  uint16_t rideEndMin;  // a paused ride ends after this long
  uint16_t minRideSec;  // rides shorter than this, or than minRideM, aren't kept
  uint16_t minRideM;
  uint16_t fullMin;     // screen at full brightness after activity
  uint16_t offMin;      // then dimmed, and off after this long (0 = never off)
  uint8_t dimPct;       // dimmed brightness, % of full
  bool nightDim;        // lower full brightness in a dark room (light sensor)
  uint16_t savedMin;    // how long the ride-saved screen stays up
  char timeZone[48];    // POSIX TZ string
};

void begin();
Values get();
const Values& defaults();
// Whether every value is in range; if not, error says which one.
bool check(const Values& v, char* error, size_t len);
// Checks every value, then saves and uses them. On a bad value nothing
// changes and error says which one.
bool set(const Values& v, char* error, size_t len);

}  // namespace settings
