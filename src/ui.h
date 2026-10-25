#pragma once

#include <Print.h>

namespace ui {

void begin();
// Redraws the status bar and status rows; call about once a second.
void draw();
// Re-renders the screen into memory and writes it to out (tools/screenshot.py).
void screenshot(Print& out);
// Keeps the backlight on: full brightness for 2 minutes after the last
// activity (touch, riding, bike connecting), then dim, then off after 30.
void wake();
// Firmware update progress over WiFi: 0-100, or -1 when it failed.
void otaProgress(int percent);
// The start of a touch, with its screen position when known. Handles the
// paused screen's end-ride button and dismissing the ride-saved screen.
void press(bool havePosition, int x, int y);
// Blocking 3-target touch calibration on the screen; saves it to NVS.
bool calibrateTouch();

}  // namespace ui
