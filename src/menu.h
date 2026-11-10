#pragma once

// The settings screens, opened by tapping the home screen: WiFi network,
// bike and screen options, time zone, NAS share and device info. They need
// to know where taps land, so the first time they open they ask for the
// touch setup (tap a few targets).
namespace menu {

void open();
bool isOpen();
// Leaves the settings screens (back to home, or a ride started)
void close();
// Nobody has touched the settings screens for a couple of minutes
bool expired();
// Draws the current page in full, or (full = false, once a second) only
// the parts that change
void draw(bool full);
// Touch polling while open, every 30 ms: whether the screen is pressed
void poll(bool down);
// Testing: a tap at a screen position, or with raw touch readings (for the
// touch setup page)
void inject(int x, int y, bool raw);

}  // namespace menu
