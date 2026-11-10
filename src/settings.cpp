#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include "logger.h"

namespace settings {

static const Values DEFAULTS = {
    "I_EB",  // bikeName
    3,       // releaseMin
    5,       // rideEndMin
    120,     // minRideSec
    500,     // minRideM
    2,       // fullMin
    30,      // offMin
    16,      // dimPct
    true,    // nightDim
    3,       // savedMin
    "AST4ADT,M3.2.0,M11.1.0",  // Halifax, NS
};

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Values current = DEFAULTS;

static bool inRange(const char* what, unsigned value, unsigned lo, unsigned hi, char* error, size_t len) {
  if (value >= lo && value <= hi) return true;
  snprintf(error, len, "%s must be %u to %u", what, lo, hi);
  return false;
}

static bool valid(const Values& v, char* error, size_t len) {
  if (!v.bikeName[0]) {
    snprintf(error, len, "bike name is empty");
    return false;
  }
  if (!v.timeZone[0]) {
    snprintf(error, len, "time zone is empty");
    return false;
  }
  if (!inRange("disconnect time", v.releaseMin, 1, 60, error, len) ||
      !inRange("ride end time", v.rideEndMin, 1, 60, error, len) ||
      !inRange("shortest ride time", v.minRideSec, 0, 3600, error, len) ||
      !inRange("shortest ride distance", v.minRideM, 0, 20000, error, len) ||
      !inRange("full brightness time", v.fullMin, 1, 120, error, len) ||
      !inRange("screen off time", v.offMin, 0, 1440, error, len) ||
      !inRange("dimmed brightness", v.dimPct, 1, 100, error, len) ||
      !inRange("ride summary time", v.savedMin, 1, 60, error, len)) {
    return false;
  }
  if (v.offMin && v.offMin <= v.fullMin) {
    snprintf(error, len, "screen off time must be after full brightness time");
    return false;
  }
  return true;
}

void begin() {
  Values v = DEFAULTS;
  Preferences prefs;
  if (prefs.begin("settings", true)) {
    // (reading a missing string logs an error, so check first)
    if (prefs.isKey("bike")) prefs.getString("bike", v.bikeName, sizeof v.bikeName);
    v.releaseMin = prefs.getUShort("release", v.releaseMin);
    v.rideEndMin = prefs.getUShort("rideEnd", v.rideEndMin);
    v.minRideSec = prefs.getUShort("minSec", v.minRideSec);
    v.minRideM = prefs.getUShort("minM", v.minRideM);
    v.fullMin = prefs.getUShort("full", v.fullMin);
    v.offMin = prefs.getUShort("off", v.offMin);
    v.dimPct = prefs.getUChar("dim", v.dimPct);
    v.nightDim = prefs.getBool("night", v.nightDim);
    v.savedMin = prefs.getUShort("saved", v.savedMin);
    if (prefs.isKey("tz")) prefs.getString("tz", v.timeZone, sizeof v.timeZone);
    prefs.end();
  }
  // A bad stored value (shouldn't happen) falls back to the defaults
  char error[64];
  if (!valid(v, error, sizeof error)) {
    logger::printf("settings: %s, using defaults\n", error);
    v = DEFAULTS;
  }
  portENTER_CRITICAL(&lock);
  current = v;
  portEXIT_CRITICAL(&lock);
}

Values get() {
  portENTER_CRITICAL(&lock);
  Values copy = current;
  portEXIT_CRITICAL(&lock);
  return copy;
}

const Values& defaults() { return DEFAULTS; }

bool check(const Values& v, char* error, size_t len) { return valid(v, error, len); }

bool set(const Values& v, char* error, size_t len) {
  if (!valid(v, error, len)) return false;
  Preferences prefs;
  if (prefs.begin("settings", false)) {
    prefs.putString("bike", v.bikeName);
    prefs.putUShort("release", v.releaseMin);
    prefs.putUShort("rideEnd", v.rideEndMin);
    prefs.putUShort("minSec", v.minRideSec);
    prefs.putUShort("minM", v.minRideM);
    prefs.putUShort("full", v.fullMin);
    prefs.putUShort("off", v.offMin);
    prefs.putUChar("dim", v.dimPct);
    prefs.putBool("night", v.nightDim);
    prefs.putUShort("saved", v.savedMin);
    prefs.putString("tz", v.timeZone);
    prefs.end();
  }
  portENTER_CRITICAL(&lock);
  current = v;
  portEXIT_CRITICAL(&lock);
  return true;
}

}  // namespace settings
