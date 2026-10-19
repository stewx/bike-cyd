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

}  // namespace ui
