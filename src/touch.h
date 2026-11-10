#pragma once

#include <stdint.h>

// XPT2046 touch support: whether the screen is pressed (to wake the
// backlight) and, once set up, where (for the settings screens). The
// controller sits on its own pins, driven here by bit-banged SPI.
namespace touch {

// How raw readings map to the screen: screen = raw * scale + offset, with
// screen X taken from the raw Y reading if swapped
struct Calibration {
  bool swapped;
  float xScale, xOffset, yScale, yOffset;
};

// Puts the controller in its low-power mode with the touch interrupt
// enabled and loads the calibration from NVS (namespace "touch").
void begin();
// True while the screen is pressed (interrupt line low, or enough pressure).
bool touched();
// Median 12-bit raw readings while pressed; false if not pressed.
bool readRaw(uint16_t& rawX, uint16_t& rawY);
// Screen position in pixels; false if not pressed or not set up.
bool read(int& x, int& y);
bool calibrated();
// Works out the mapping from raw readings taken at three screen points
// (top left, top right, bottom right). False if the readings don't make
// sense (e.g. the same spot was tapped twice).
bool solve(const uint16_t raw[3][2], const int screen[3][2], Calibration& out);
void apply(const Calibration& c, uint16_t rawX, uint16_t rawY, int& x, int& y);
// Saves the mapping and starts using it
void save(const Calibration& c);

}  // namespace touch
