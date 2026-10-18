#pragma once

#include <Arduino.h>

namespace storage {

struct Status {
  bool mounted;
  const char* type;
  uint64_t totalBytes;
  uint64_t freeBytes;
};

// Mounts the microSD card and creates /rides.
bool begin();
// Retries the mount if the card was missing.
void loop();
Status status();

// Serial file access (tools/sdfile.py): lists a directory as "F <size> <path>"
// lines, or sends a file as "FILE <path> <size>", base64 lines, "EOF".
void listDir(Print& out, const char* path);
void sendFile(Print& out, const char* path);

}  // namespace storage
