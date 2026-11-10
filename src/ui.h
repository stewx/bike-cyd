#pragma once

#include <Print.h>

namespace ui {

void begin();
// Redraws the status bar and status rows; call about once a second.
void draw();
// Re-renders the screen into memory and writes it to out (tools/screenshot.py).
void screenshot(Print& out);
// Keeps the backlight on: full brightness for a while after the last
// activity (touch, riding, bike connecting), then dim, then off (settings).
void wake();
// Firmware update progress over WiFi: 0-100, or -1 when it failed.
void otaProgress(int percent);
// Reads the touch screen; call every 30 ms. A tap on the home screen opens
// the settings screens, taps on the paused screen end the ride (tap, then
// tap again to confirm) and a tap dismisses the ride-saved screen.
void pollTouch();
// Testing: a tap at a screen position, or with raw touch readings (for the
// touch setup)
void tap(int x, int y, bool raw);

}  // namespace ui
