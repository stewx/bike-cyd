#include "onedrive.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "net.h"
#include "ride.h"
#include "storage.h"

namespace onedrive {

static constexpr const char* DEVICE_CODE_URL = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
static constexpr const char* TOKEN_URL = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
static constexpr const char* UPLOAD_URL = "https://graph.microsoft.com/v1.0/me/drive/special/approot:/rides/";
static constexpr const char* SCOPE = "Files.ReadWrite.AppFolder offline_access";

static constexpr const char* PENDING_DIR = "/rides/pending";
static constexpr const char* DONE_DIR = "/rides/done";
static constexpr const char* FAILED_DIR = "/rides/failed";

static constexpr uint32_t IDLE_CHECK_MS = 10000;
static constexpr uint32_t RETRY_MIN_MS = 60 * 1000;
static constexpr uint32_t RETRY_MAX_MS = 60 * 60 * 1000;
// TLS needs one large contiguous block for its record buffers
static constexpr size_t MIN_FREE_BLOCK = 40 * 1024;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Status current = {State::Offline, 0, 0, "", "", ""};

static String clientId;
static String refreshToken;
static String accessToken;
static uint32_t accessExpiresMs = 0;
static volatile bool signOutRequested = false;
static uint32_t retryDelayMs = RETRY_MIN_MS;

// ------------------------------------------------------------------ helpers

static void setState(State s) {
  portENTER_CRITICAL(&lock);
  current.state = s;
  portEXIT_CRITICAL(&lock);
}

static void setError(const char* fmt, ...) {
  char buf[64];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  Serial.printf("onedrive: %s\n", buf);
  portENTER_CRITICAL(&lock);
  strlcpy(current.lastError, buf, sizeof current.lastError);
  portEXIT_CRITICAL(&lock);
}

static void setSignIn(const char* code, const char* url) {
  portENTER_CRITICAL(&lock);
  strlcpy(current.userCode, code, sizeof current.userCode);
  strlcpy(current.verifyUrl, url, sizeof current.verifyUrl);
  portEXIT_CRITICAL(&lock);
}

static String urlEncode(const String& s) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3 / 2);
  for (char c : s) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += HEX_DIGITS[(uint8_t)c >> 4];
      out += HEX_DIGITS[(uint8_t)c & 15];
    }
  }
  return out;
}

static void loadSettings() {
  Preferences prefs;
  if (prefs.begin("onedrive", true)) {
    clientId = prefs.getString("client", "");
    refreshToken = prefs.getString("refresh", "");
    prefs.end();
  }
}

static void saveRefreshToken(const String& token) {
  refreshToken = token;
  Preferences prefs;
  if (prefs.begin("onedrive", false)) {
    if (token.isEmpty()) prefs.remove("refresh");
    else prefs.putString("refresh", token);
    prefs.end();
  }
}

static bool enoughMemory() {
  size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (block >= MIN_FREE_BLOCK) return true;
  setError("not enough memory for HTTPS (%u KB block)", (unsigned)(block / 1024));
  return false;
}

// POSTs a form and parses the JSON reply. Returns the HTTP status, or a
// negative HTTPClient error.
static int postForm(const char* url, const String& body, JsonDocument& reply) {
  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  HTTPClient http;
  if (!http.begin(tls, url)) return -1;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(body);
  if (code > 0) {
    String text = http.getString();
    deserializeJson(reply, text);
  }
  http.end();
  return code;
}

// ------------------------------------------------------------------ sign-in

// Runs one device code sign-in round (up to ~15 minutes).
static void signIn() {
  setState(State::SigningIn);
  if (!enoughMemory()) return;

  JsonDocument reply;
  int code = postForm(DEVICE_CODE_URL, "client_id=" + urlEncode(clientId) + "&scope=" + urlEncode(SCOPE), reply);
  if (code != 200) {
    setError("device code request failed (%d %s)", code, reply["error"] | "");
    return;
  }
  String deviceCode = reply["device_code"] | "";
  String userCode = reply["user_code"] | "";
  String verifyUrl = reply["verification_uri"] | "https://microsoft.com/devicelogin";
  uint32_t interval = (reply["interval"] | 5) * 1000;
  uint32_t expires = (reply["expires_in"] | 900) * 1000;
  verifyUrl.replace("https://", "");
  setSignIn(userCode.c_str(), verifyUrl.c_str());
  Serial.printf("onedrive: sign in at https://%s with code %s\n", verifyUrl.c_str(), userCode.c_str());

  const String pollBody = "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" + urlEncode(clientId) +
                          "&device_code=" + urlEncode(deviceCode);
  uint32_t start = millis();
  while (millis() - start < expires) {
    delay(interval);
    if (!enoughMemory()) continue;
    JsonDocument poll;
    code = postForm(TOKEN_URL, pollBody, poll);
    if (code == 200) {
      String rt = poll["refresh_token"] | "";
      accessToken = poll["access_token"] | "";
      accessExpiresMs = millis() + ((poll["expires_in"] | 3600) - 120) * 1000;
      if (rt.isEmpty() || accessToken.isEmpty()) {
        setError("sign-in reply had no tokens");
        break;
      }
      saveRefreshToken(rt);
      setSignIn("", "");
      Serial.println("onedrive: signed in");
      return;
    }
    String err = poll["error"] | "";
    if (err == "authorization_pending") continue;
    if (err == "slow_down") {
      interval += 5000;
      continue;
    }
    setError("sign-in stopped (%d %s)", code, err.c_str());
    break;
  }
  setSignIn("", "");
}

static bool ensureAccessToken() {
  if (!accessToken.isEmpty() && (int32_t)(accessExpiresMs - millis()) > 0) return true;
  if (!enoughMemory()) return false;
  JsonDocument reply;
  String body = "client_id=" + urlEncode(clientId) + "&grant_type=refresh_token&refresh_token=" +
                urlEncode(refreshToken) + "&scope=" + urlEncode(SCOPE);
  int code = postForm(TOKEN_URL, body, reply);
  if (code == 200) {
    accessToken = reply["access_token"] | "";
    accessExpiresMs = millis() + ((reply["expires_in"] | 3600) - 120) * 1000;
    String rt = reply["refresh_token"] | "";
    if (!rt.isEmpty() && rt != refreshToken) saveRefreshToken(rt);
    return !accessToken.isEmpty();
  }
  String err = reply["error"] | "";
  if (err == "invalid_grant") {
    setError("sign-in expired, please sign in again");
    saveRefreshToken("");
  } else {
    setError("token refresh failed (%d %s)", code, err.c_str());
  }
  return false;
}

// ------------------------------------------------------------------ upload queue

static int countFiles(const char* dir, String* oldest) {
  int n = 0;
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    n++;
    // Ride files are named by start time, so the smallest name is the oldest
    if (oldest && (oldest->isEmpty() || String(f.name()) < *oldest)) *oldest = f.name();
  }
  return n;
}

static void updateCounts(String* oldest) {
  int pending = storage::status().mounted ? countFiles(PENDING_DIR, oldest) : 0;
  int failed = storage::status().mounted ? countFiles(FAILED_DIR, nullptr) : 0;
  portENTER_CRITICAL(&lock);
  current.pending = pending;
  current.failed = failed;
  portEXIT_CRITICAL(&lock);
}

static void moveTo(const char* dir, const String& name) {
  String from = String(PENDING_DIR) + "/" + name;
  String to = String(dir) + "/" + name;
  if (SD.exists(to)) SD.remove(to);
  if (!SD.rename(from, to)) setError("could not move %s", name.c_str());
}

enum class Result { Done, Retry, Failed, Reauth };

static Result upload(const String& name) {
  File f = SD.open(String(PENDING_DIR) + "/" + name);
  if (!f) return Result::Failed;
  size_t size = f.size();

  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(30000);
  if (!http.begin(tls, String(UPLOAD_URL) + urlEncode(name) + ":/content")) return Result::Retry;
  http.addHeader("Authorization", "Bearer " + accessToken);
  http.addHeader("Content-Type", "text/plain");
  uint32_t t0 = millis();
  int code = http.sendRequest("PUT", &f, size);
  String reply = code > 0 ? http.getString() : String();
  http.end();
  f.close();

  if (code == 200 || code == 201) {
    Serial.printf("onedrive: uploaded %s (%u bytes, %lu ms)\n", name.c_str(), (unsigned)size,
                  (unsigned long)(millis() - t0));
    return Result::Done;
  }
  if (code == 401) {
    accessToken = "";
    return Result::Reauth;
  }
  setError("upload of %s failed (%d)", name.c_str(), code);
  if (reply.length()) Serial.printf("onedrive: %s\n", reply.substring(0, 300).c_str());
  if (code < 0 || code == 408 || code == 429 || code >= 500) return Result::Retry;
  return Result::Failed;
}

static void waitAfterError() {
  setState(State::Waiting);
  delay(retryDelayMs);
  retryDelayMs = min(retryDelayMs * 2, RETRY_MAX_MS);
}

// ------------------------------------------------------------------ task

static void run(void*) {
  loadSettings();
  for (;;) {
    if (signOutRequested) {
      signOutRequested = false;
      accessToken = "";
      saveRefreshToken("");
    }
    if (clientId.isEmpty()) {
      setState(State::NotConfigured);
      delay(IDLE_CHECK_MS);
      loadSettings();
      continue;
    }
    if (WiFi.status() != WL_CONNECTED || !net::timeSynced() || !storage::status().mounted) {
      setState(State::Offline);
      delay(IDLE_CHECK_MS);
      continue;
    }
    if (refreshToken.isEmpty()) {
      signIn();
      if (refreshToken.isEmpty()) delay(30000);
      continue;
    }

    String oldest;
    updateCounts(&oldest);
    // Leave the radio and memory to the bike while someone is riding
    if (oldest.isEmpty() || ride::stats().active) {
      setState(State::Idle);
      delay(IDLE_CHECK_MS);
      continue;
    }

    setState(State::Uploading);
    if (!ensureAccessToken()) {
      waitAfterError();
      continue;
    }
    if (!enoughMemory()) {
      waitAfterError();
      continue;
    }
    switch (upload(oldest)) {
      case Result::Done:
        moveTo(DONE_DIR, oldest);
        retryDelayMs = RETRY_MIN_MS;
        portENTER_CRITICAL(&lock);
        current.lastError[0] = '\0';
        portEXIT_CRITICAL(&lock);
        break;
      case Result::Failed:
        moveTo(FAILED_DIR, oldest);
        break;
      case Result::Reauth:
        break;  // next pass refreshes the access token
      case Result::Retry:
        waitAfterError();
        break;
    }
    updateCounts(nullptr);
  }
}

void start() { xTaskCreatePinnedToCore(run, "onedrive", 10240, nullptr, 1, nullptr, 1); }

Status status() {
  portENTER_CRITICAL(&lock);
  Status copy = current;
  portEXIT_CRITICAL(&lock);
  return copy;
}

const char* stateText(State state) {
  switch (state) {
    case State::NotConfigured: return "not set up";
    case State::Offline: return "waiting for network";
    case State::SigningIn: return "sign-in needed";
    case State::Idle: return "up to date";
    case State::Uploading: return "uploading";
    case State::Waiting: return "retrying soon";
  }
  return "?";
}

void command(Print& out, const String& args) {
  if (args.startsWith("client ")) {
    String id = args.substring(7);
    id.trim();
    Preferences prefs;
    if (prefs.begin("onedrive", false)) {
      prefs.putString("client", id);
      prefs.remove("refresh");
      prefs.end();
    }
    signOutRequested = true;
    out.printf("onedrive: client ID set to %s\n", id.c_str());
  } else if (args == "signout") {
    signOutRequested = true;
    out.println("onedrive: signing out");
  } else if (args == "test") {
    // A small file to check the upload path end to end
    char name[64];
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(name, sizeof name, "/rides/pending/test-%Y%m%d-%H%M%S.txt", &t);
    File f = SD.open(name, FILE_WRITE);
    if (f) {
      f.printf("bike-cyd upload test %s\n", name);
      f.close();
      out.printf("onedrive: queued %s\n", name);
    } else {
      out.println("onedrive: could not write test file");
    }
  } else {
    Status s = status();
    out.printf("onedrive: %s, %d pending, %d failed, code=%s url=%s error=%s\n", stateText(s.state), s.pending,
               s.failed, s.userCode, s.verifyUrl, s.lastError);
  }
}

}  // namespace onedrive
