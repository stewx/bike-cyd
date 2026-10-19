#pragma once

#include "bike.h"

// Ride tracking and recording. A ride starts when the console reports a
// running workout and ends when the console ends the workout or nobody has
// ridden for 5 minutes. While it runs, every data message from the bike is
// appended to a CSV in /rides/recording on the SD card; finished rides move
// to /rides/finished, where tcx::exportFinished() turns them into TCX files.
namespace ride {

struct Stats {
  bool active;     // a ride is in progress (running or paused)
  bool paused;
  bool recording;  // samples are being written to the SD card
  uint32_t movingSeconds;
  uint32_t distanceM;
  float avgKmh;
  float maxKmh;
};

// Moves rides interrupted by a reset or power cut to /rides/finished.
void begin();
// Feed the latest bike data once a second.
void tick(const bike::Data& data);
Stats stats();

}  // namespace ride
