#pragma once

#include <Print.h>

namespace ui {

void begin();
// Redraws the status bar and status rows; call about once a second.
void draw();
// Reads the panel back and writes it to out as "SHOT w h\n" plus RGB888 rows.
void screenshot(Print& out);

}  // namespace ui
