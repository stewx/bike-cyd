#pragma once

#include <Arduino.h>

namespace bike {

// Resting: disconnected on purpose after 3 idle minutes, waiting for the
// console to go to sleep before connecting again.
enum class Link { Scanning, Connecting, Initializing, Connected, Resting };

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
// Testing: makes get() return a made-up ride of the given length (with a
// 20 s pause in the middle) instead of the real bike's data.
void simulate(uint32_t seconds);
const char* linkText(Link link);

}  // namespace bike
