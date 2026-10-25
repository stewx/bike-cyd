#pragma once

#include <stdint.h>

// Minimal XPT2046 touch support: whether the screen is pressed (to wake the
// backlight) and, once calibrated, where (for the paused screen's button).
// The controller sits on its own pins, driven here by bit-banged SPI.
namespace touch {

// Puts the controller in its low-power mode with the touch interrupt
// enabled and loads the calibration from NVS (namespace "touch").
void begin();
// True while the screen is pressed (interrupt line low, or enough pressure).
bool touched();
// Averaged 12-bit raw readings while pressed; false if not pressed.
bool readRaw(uint16_t& rawX, uint16_t& rawY);
// Screen position in pixels; false if not pressed or not calibrated.
bool read(int& x, int& y);
bool calibrated();
// Works out the mapping from raw readings taken at three screen points
// (top left, top right, bottom right) and saves it. False if the readings
// don't make sense (e.g. the same spot was tapped twice).
bool calibrate(const uint16_t raw[3][2], const int screen[3][2]);

}  // namespace touch
