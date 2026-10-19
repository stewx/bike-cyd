#include "nas.h"

#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <fcntl.h>

#include "logger.h"
#include "ride.h"
#include "storage.h"
#include "tcx.h"

// clang-format off
#include <smb2/smb2.h>  // must come before libsmb2.h
#include <smb2/libsmb2.h>
// clang-format on

namespace nas {

static constexpr const char* PENDING_DIR = "/rides/pending";
static constexpr const char* DONE_DIR = "/rides/done";
static constexpr const char* FAILED_DIR = "/rides/failed";

static constexpr uint32_t IDLE_CHECK_MS = 10000;
static constexpr uint32_t RETRY_MIN_MS = 60 * 1000;
static constexpr uint32_t RETRY_MAX_MS = 60 * 60 * 1000;
static constexpr int SMB_TIMEOUT_S = 30;
static constexpr size_t CHUNK = 4096;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Status current = {State::Offline, 0, 0, ""};
static uint32_t retryDelayMs = RETRY_MIN_MS;

struct Settings {
  String server, share, folder, user, pass;
  bool valid() const { return !server.isEmpty() && !share.isEmpty(); }
};

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
  logger::printf("nas: %s\n", buf);
  portENTER_CRITICAL(&lock);
  strlcpy(current.lastError, buf, sizeof current.lastError);
  portEXIT_CRITICAL(&lock);
}

// Splits "\\server\share\folder" (or with forward slashes) into its parts.
static Settings loadSettings() {
  Settings s;
  Preferences prefs;
  if (!prefs.begin("nas", true)) return s;
  String url = prefs.getString("url", "");
  s.user = prefs.getString("user", "");
  s.pass = prefs.getString("pass", "");
  prefs.end();

  url.replace('\\', '/');
  while (url.startsWith("/")) url.remove(0, 1);
  int a = url.indexOf('/');
  if (a < 0) return s;
  int b = url.indexOf('/', a + 1);
  s.server = url.substring(0, a);
  s.share = b < 0 ? url.substring(a + 1) : url.substring(a + 1, b);
  s.folder = b < 0 ? "" : url.substring(b + 1);
  while (s.folder.endsWith("/")) s.folder.remove(s.folder.length() - 1);
  return s;
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
  bool mounted = storage::status().mounted;
  int pending = mounted ? countFiles(PENDING_DIR, oldest) : 0;
  int failed = mounted ? countFiles(FAILED_DIR, nullptr) : 0;
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

enum class Result { Done, Retry, Failed };

// Copies one file to the share as "<name>.part", then renames it, so nothing
// on the NAS ever sees a half-written ride.
static Result upload(const Settings& cfg, const String& name) {
  File f = SD.open(String(PENDING_DIR) + "/" + name);
  if (!f) return Result::Failed;

  struct smb2_context* smb2 = smb2_init_context();
  if (!smb2) {
    setError("out of memory");
    return Result::Retry;
  }
  smb2_set_timeout(smb2, SMB_TIMEOUT_S);
  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  if (!cfg.user.isEmpty()) smb2_set_user(smb2, cfg.user.c_str());
  if (!cfg.pass.isEmpty()) smb2_set_password(smb2, cfg.pass.c_str());

  Result result = Result::Retry;
  uint32_t t0 = millis();
  if (smb2_connect_share(smb2, cfg.server.c_str(), cfg.share.c_str(),
                         cfg.user.isEmpty() ? nullptr : cfg.user.c_str()) < 0) {
    setError("connect: %s", smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return Result::Retry;
  }

  String dir = cfg.folder.isEmpty() ? "" : cfg.folder + "/";
  if (!cfg.folder.isEmpty()) smb2_mkdir(smb2, cfg.folder.c_str());  // fails harmlessly if it exists
  String finalPath = dir + name;
  String partPath = finalPath + ".part";

  struct smb2fh* fh = smb2_open(smb2, partPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!fh) {
    setError("open: %s", smb2_get_error(smb2));
  } else {
    static uint8_t buf[CHUNK];
    bool ok = true;
    for (;;) {
      esp_task_wdt_reset();
      int n = f.read(buf, sizeof buf);
      if (n <= 0) break;
      for (int off = 0; off < n;) {
        int w = smb2_write(smb2, fh, buf + off, n - off);
        if (w <= 0) {
          setError("write: %s", smb2_get_error(smb2));
          ok = false;
          break;
        }
        off += w;
      }
      if (!ok) break;
    }
    smb2_close(smb2, fh);
    if (ok) {
      smb2_unlink(smb2, finalPath.c_str());  // replace an earlier copy
      if (smb2_rename(smb2, partPath.c_str(), finalPath.c_str()) < 0) {
        setError("rename: %s", smb2_get_error(smb2));
      } else {
        logger::printf("nas: uploaded %s (%u bytes, %lu ms)\n", name.c_str(), (unsigned)f.size(),
                      (unsigned long)(millis() - t0));
        result = Result::Done;
      }
    } else {
      smb2_unlink(smb2, partPath.c_str());
    }
  }
  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  f.close();
  return result;
}

// Sleeps in short steps so the task watchdog keeps getting fed
static void waitMs(uint32_t ms) {
  for (uint32_t start = millis(); millis() - start < ms;) {
    esp_task_wdt_reset();
    delay(min(ms - (millis() - start), (uint32_t)1000));
  }
}

static void waitAfterError() {
  setState(State::Waiting);
  waitMs(retryDelayMs);
  retryDelayMs = min(retryDelayMs * 2, RETRY_MAX_MS);
}

// ------------------------------------------------------------------ task

static void run(void*) {
  esp_task_wdt_add(nullptr);  // a hang (e.g. a stuck SMB call) restarts the board
  for (;;) {
    esp_task_wdt_reset();
    // Finished rides become TCX files in /rides/pending, even while the NAS is unreachable
    if (storage::status().mounted) tcx::exportFinished();

    Settings cfg = loadSettings();
    if (!cfg.valid()) {
      setState(State::NotConfigured);
      waitMs(IDLE_CHECK_MS);
      continue;
    }
    if (WiFi.status() != WL_CONNECTED || !storage::status().mounted) {
      setState(State::Offline);
      waitMs(IDLE_CHECK_MS);
      continue;
    }

    String oldest;
    updateCounts(&oldest);
    // Leave the radio and memory to the bike while someone is riding
    if (oldest.isEmpty() || ride::stats().active) {
      setState(State::Idle);
      waitMs(IDLE_CHECK_MS);
      continue;
    }

    setState(State::Uploading);
    switch (upload(cfg, oldest)) {
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
      case Result::Retry:
        waitAfterError();
        break;
    }
    updateCounts(nullptr);
  }
}

// Measured peak stack use is ~3.1 KB (TCX export, SMB upload)
void start() { xTaskCreatePinnedToCore(run, "nas", 6144, nullptr, 1, nullptr, 1); }

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
    case State::Idle: return "up to date";
    case State::Uploading: return "uploading";
    case State::Waiting: return "retrying soon";
  }
  return "?";
}

static void setSetting(Print& out, const char* key, String value) {
  value.trim();
  Preferences prefs;
  if (prefs.begin("nas", false)) {
    prefs.putString(key, value);
    prefs.end();
  }
  // Never echo the password back
  out.printf("nas: %s set%s%s\n", key, strcmp(key, "pass") ? " to " : "", strcmp(key, "pass") ? value.c_str() : "");
}

void command(Print& out, const String& args) {
  if (args.startsWith("url ")) {
    setSetting(out, "url", args.substring(4));
  } else if (args.startsWith("user ")) {
    setSetting(out, "user", args.substring(5));
  } else if (args.startsWith("pass ")) {
    setSetting(out, "pass", args.substring(5));
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
      out.printf("nas: queued %s\n", name);
    } else {
      out.println("nas: could not write test file");
    }
  } else {
    Settings cfg = loadSettings();
    Status s = status();
    out.printf("nas: %s, %d pending, %d failed, server=%s share=%s folder=%s user=%s error=%s\n", stateText(s.state),
               s.pending, s.failed, cfg.server.c_str(), cfg.share.c_str(), cfg.folder.c_str(), cfg.user.c_str(),
               s.lastError);
  }
}

}  // namespace nas
