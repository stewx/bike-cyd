#pragma once

#include <Arduino.h>

// Uploads finished ride files to the OneDrive app folder (Apps/<app name>/rides)
// using Microsoft Graph. Files waiting in /rides/pending on the SD card are
// uploaded oldest first and then moved to /rides/done (or /rides/failed).
//
// Sign-in uses the OAuth device code flow: the screen shows a short code to
// enter at microsoft.com/devicelogin. The app registration's client ID and the
// refresh token live in NVS namespace "onedrive".
namespace onedrive {

enum class State {
  NotConfigured,  // no client ID stored
  Offline,        // waiting for WiFi and the clock
  SigningIn,      // showing a device code
  Idle,           // signed in, nothing to upload
  Uploading,
  Waiting,        // retrying after an error
};

struct Status {
  State state;
  int pending;          // files in /rides/pending
  int failed;           // files moved to /rides/failed
  char userCode[16];    // while signing in
  char verifyUrl[48];   // while signing in
  char lastError[64];
};

void start();
Status status();
const char* stateText(State state);

// Serial commands ("od ..."): "client <id>", "signout", "status", "test"
void command(Print& out, const String& args);

}  // namespace onedrive
