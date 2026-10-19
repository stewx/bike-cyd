#pragma once

#include <Arduino.h>

// Messages go to the USB serial port as before, and also, timestamped, to
// /logs/bike-cyd.log on the SD card (rotated to bike-cyd.1.log at 1 MB).
// printf() is safe from any task; the SD writes happen in flush().
namespace logger {

void printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Writes only to the SD log (for periodic status lines that would flood serial)
void fileOnly(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Writes buffered lines to the SD card; call from the main loop. Writes at
// most every 5 s unless forced (before a restart).
void flush(bool force = false);

}  // namespace logger
