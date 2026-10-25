#pragma once

#include <Arduino.h>

// Uploads finished ride files to an SMB share on the NAS. Files waiting in
// /rides/pending on the SD card are copied oldest first and then moved to
// /rides/done (or /rides/failed). Settings live in NVS namespace "nas":
// "url" (\\server\share or \\server\share\folder), "user", "pass".
namespace nas {

enum class State {
  NotConfigured,  // no share stored
  Offline,        // waiting for WiFi or the SD card
  Idle,           // nothing to upload
  Uploading,
  Waiting,        // retrying after an error
};

struct Status {
  State state;
  int pending;  // files in /rides/pending
  int failed;   // files moved to /rides/failed
  char lastError[64];
};

void start();
Status status();
// Name of the last file uploaded ("" if none since boot)
void lastUploaded(char* out, size_t len);
// Wakes the uploader now (a ride just ended) instead of at its next check
void kick();
const char* stateText(State state);

// Serial commands ("nas ..."): "url <\\server\share[\folder]>", "user <name>",
// "pass <password>", "status", "test"
void command(Print& out, const String& args);

}  // namespace nas
