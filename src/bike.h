#pragma once

#include <Arduino.h>

namespace bike {

enum class Link { Scanning, Connecting, Initializing, Connected };

// Workout state as reported by the console.
enum class Workout { Unknown, Idle, Running, Paused };

struct Data {
  Link link;
  Workout workout;
  float speedKmh;
  uint16_t cadenceRpm;
  uint16_t watts;
  uint8_t resistance;
  uint32_t distanceM;  // console's distance for the current workout
  uint32_t updatedMs;  // millis() of the last data message, 0 if none yet
};

// Starts the BLE task: finds the bike, runs the iFit init sequence and polls it.
void start();
// Thread-safe copy of the latest values.
Data get();
const char* linkText(Link link);

}  // namespace bike
