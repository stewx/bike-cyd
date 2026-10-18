#pragma once

#include "bike.h"

namespace ride {

// What the ride screen shows. Recording to the SD card comes in step 4.
struct Stats {
  bool active;  // a ride is in progress (running or paused)
  bool paused;
  uint32_t movingSeconds;
  uint32_t distanceM;
  float avgKmh;
  float maxKmh;
};

// Feed the latest bike data once a second.
void tick(const bike::Data& data);
Stats stats();

}  // namespace ride
