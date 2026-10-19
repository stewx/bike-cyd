#pragma once

// Minimal XPT2046 touch support: only "is the screen being touched", for
// waking the backlight. The controller sits on its own pins, driven here by
// a few bit-banged SPI transfers.
namespace touch {

// Puts the controller in its low-power mode with the touch interrupt enabled.
void begin();
// True while the screen is pressed (interrupt line low, or enough pressure).
bool touched();

}  // namespace touch
