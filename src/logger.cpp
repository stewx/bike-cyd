#include "logger.h"

#include <SD.h>
#include <time.h>

#include "net.h"
#include "storage.h"

namespace logger {

static constexpr const char* LOG_DIR = "/logs";
static constexpr const char* LOG_PATH = "/logs/bike-cyd.log";
static constexpr const char* OLD_LOG_PATH = "/logs/bike-cyd.1.log";
static constexpr size_t MAX_LOG_BYTES = 1024 * 1024;
static constexpr uint32_t FLUSH_MS = 5000;

// Lines wait here until the main loop writes them to the card
static char ring[2048];
static size_t ringLen = 0;
static size_t dropped = 0;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static void buffer(const char* line, size_t len) {
  char stamp[24];
  if (net::timeSynced()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S ", &t);
  } else {
    snprintf(stamp, sizeof stamp, "+%lus ", (unsigned long)(millis() / 1000));
  }
  size_t stampLen = strlen(stamp);

  portENTER_CRITICAL(&lock);
  if (ringLen + stampLen + len <= sizeof ring) {
    memcpy(ring + ringLen, stamp, stampLen);
    memcpy(ring + ringLen + stampLen, line, len);
    ringLen += stampLen + len;
  } else {
    dropped += len;
  }
  portEXIT_CRITICAL(&lock);
}

static void vlog(bool toSerial, const char* fmt, va_list args) {
  char line[256];
  int n = vsnprintf(line, sizeof line, fmt, args);
  if (n <= 0) return;
  size_t len = min((size_t)n, sizeof line - 1);
  if (toSerial) Serial.write((const uint8_t*)line, len);
  buffer(line, len);
}

void printf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlog(true, fmt, args);
  va_end(args);
}

void fileOnly(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlog(false, fmt, args);
  va_end(args);
}

void flush(bool force) {
  static uint32_t lastFlush = 0;
  if (!force && millis() - lastFlush < FLUSH_MS) return;
  lastFlush = millis();
  if (!ringLen || !storage::status().mounted) return;

  static char out[sizeof ring];
  portENTER_CRITICAL(&lock);
  size_t len = ringLen;
  size_t lost = dropped;
  memcpy(out, ring, len);
  ringLen = 0;
  dropped = 0;
  portEXIT_CRITICAL(&lock);

  if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  if (lost) f.printf("[log: %u bytes dropped]\n", (unsigned)lost);
  f.write((const uint8_t*)out, len);
  size_t size = f.size();
  f.close();
  if (size > MAX_LOG_BYTES) {
    SD.remove(OLD_LOG_PATH);
    SD.rename(LOG_PATH, OLD_LOG_PATH);
  }
}

}  // namespace logger
