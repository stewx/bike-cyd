#include "history.h"

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>

#include "logger.h"

namespace history {

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Ride lastRide = {};
static uint32_t weekStart = 0;  // local day number of the Monday the totals are for
static Week week = {};

// Local day number (days since 1970) of the Monday of t's week
static uint32_t mondayOf(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  const int sinceMonday = (lt.tm_wday + 6) % 7;
  // Noon on that day, so a daylight saving change can't move it across midnight
  lt.tm_hour = 12;
  lt.tm_min = lt.tm_sec = 0;
  lt.tm_isdst = -1;
  const time_t noon = mktime(&lt);
  return noon / 86400 - sinceMonday;
}

void begin() {
  Preferences prefs;
  if (!prefs.begin("history", true)) return;  // nothing stored yet
  Ride r = {prefs.getULong("lastT", 0), prefs.getULong("lastM", 0), prefs.getULong("lastS", 0)};
  uint32_t start = prefs.getULong("weekDay", 0);
  Week w = {prefs.getUShort("weekN", 0), prefs.getULong("weekM", 0), prefs.getULong("weekS", 0)};
  prefs.end();
  portENTER_CRITICAL(&lock);
  lastRide = r;
  weekStart = start;
  week = w;
  portEXIT_CRITICAL(&lock);
}

void add(const Ride& r) {
  if (r.startEpoch < 1700000000) return;  // no real start time
  const uint32_t monday = mondayOf(r.startEpoch);
  portENTER_CRITICAL(&lock);
  const bool newer = r.startEpoch > lastRide.startEpoch;
  if (newer) lastRide = r;
  // A ride from an older week (exported late) doesn't touch this week's totals
  if (monday > weekStart) {
    weekStart = monday;
    week = {};
  }
  const bool counts = monday == weekStart;
  if (counts) {
    week.rides++;
    week.distanceM += r.distanceM;
    week.movingS += r.movingS;
  }
  const Ride l = lastRide;
  const uint32_t start = weekStart;
  const Week w = week;
  portEXIT_CRITICAL(&lock);

  Preferences prefs;
  if (prefs.begin("history", false)) {
    prefs.putULong("lastT", l.startEpoch);
    prefs.putULong("lastM", l.distanceM);
    prefs.putULong("lastS", l.movingS);
    prefs.putULong("weekDay", start);
    prefs.putUShort("weekN", w.rides);
    prefs.putULong("weekM", w.distanceM);
    prefs.putULong("weekS", w.movingS);
    prefs.end();
  }
  logger::printf("history: ride of %lu m added%s, %u this week\n", (unsigned long)r.distanceM,
                 counts ? "" : " (older week)", w.rides);
}

bool last(Ride& out) {
  portENTER_CRITICAL(&lock);
  out = lastRide;
  portEXIT_CRITICAL(&lock);
  return out.startEpoch != 0;
}

Week thisWeek() {
  const time_t now = time(nullptr);
  portENTER_CRITICAL(&lock);
  const Week w = week;
  const uint32_t start = weekStart;
  portEXIT_CRITICAL(&lock);
  if (now < 1700000000 || mondayOf(now) != start) return {};
  return w;
}

}  // namespace history
