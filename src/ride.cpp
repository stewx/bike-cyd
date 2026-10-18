#include "ride.h"

namespace ride {

// Data older than this means the bike has gone quiet.
static constexpr uint32_t STALE_MS = 10000;

static Stats current = {};

void tick(const bike::Data& d) {
  bool fresh = d.link == bike::Link::Connected && d.updatedMs && millis() - d.updatedMs < STALE_MS;
  bool running = fresh && d.workout == bike::Workout::Running;
  bool paused = fresh && d.workout == bike::Workout::Paused;

  if (!current.active) {
    if (!running) return;
    current = {};
    current.active = true;
  }

  if (!running && !paused) {
    // Workout ended on the console, or the bike disconnected
    current.active = false;
    current.paused = false;
    return;
  }

  current.paused = paused;
  if (running) {
    current.movingSeconds++;
    current.distanceM = d.distanceM;
    if (d.speedKmh > current.maxKmh) current.maxKmh = d.speedKmh;
    current.avgKmh = current.distanceM * 3.6f / current.movingSeconds;
  }
}

Stats stats() { return current; }

}  // namespace ride
