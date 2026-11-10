#pragma once

#include <stdint.h>

// The last ride kept and this week's totals, for the home screen. Stored in
// NVS namespace "history", so they survive a restart.
namespace history {

struct Ride {
  uint32_t startEpoch;
  uint32_t distanceM;
  uint32_t movingS;
};

struct Week {
  uint16_t rides;
  uint32_t distanceM;
  uint32_t movingS;
};

void begin();
// A ride that was kept (exported for upload). Called from the uploader task.
void add(const Ride& r);
// False until a ride has been kept
bool last(Ride& out);
// Totals for the week (Monday to Sunday, local time) we're in now
Week thisWeek();

}  // namespace history
