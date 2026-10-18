#pragma once

#include <Arduino.h>

namespace storage {

struct Status {
  bool mounted;
  const char* type;
  uint64_t totalBytes;
  uint64_t freeBytes;
};

// Mounts the microSD card and creates /rides.
bool begin();
// Retries the mount if the card was missing.
void loop();
Status status();

}  // namespace storage
