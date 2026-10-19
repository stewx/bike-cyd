#include "storage.h"

#include <SD.h>
#include <SPI.h>

#include "board.h"

namespace storage {

static constexpr uint32_t RETRY_MS = 10000;

static SPIClass sdSpi(VSPI);
static Status current = {false, "none", 0, 0};
static uint32_t lastAttempt = 0;

static const char* typeName(uint8_t type) {
  switch (type) {
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SD";
    case CARD_SDHC: return "SDHC";
    default: return "unknown";
  }
}

bool begin() {
  lastAttempt = millis();
  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, sdSpi, 20000000) || SD.cardType() == CARD_NONE) {
    SD.end();
    current = {false, "none", 0, 0};
    Serial.println("storage: no SD card");
    return false;
  }
  for (const char* dir : {"/rides", "/rides/pending", "/rides/done", "/rides/failed"}) {
    if (!SD.exists(dir)) SD.mkdir(dir);
  }
  // usedBytes() scans the FAT once, which can take a second on a big card
  current.mounted = true;
  current.type = typeName(SD.cardType());
  current.totalBytes = SD.totalBytes();
  current.freeBytes = current.totalBytes - SD.usedBytes();
  Serial.printf("storage: %s card, %.1f GB free of %.1f GB\n", current.type, current.freeBytes / 1e9,
                current.totalBytes / 1e9);
  return true;
}

void loop() {
  if (!current.mounted && millis() - lastAttempt > RETRY_MS) begin();
}

Status status() { return current; }

void listDir(Print& out, const char* path) {
  out.printf("\nLIST %s\n", path);
  File dir = current.mounted ? SD.open(path) : File();
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      out.printf("F %lu %s%s\n", (unsigned long)f.size(), f.path(), f.isDirectory() ? "/" : "");
    }
  }
  out.print("EOF\n");
}

void sendFile(Print& out, const char* path) {
  static constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  File f = current.mounted ? SD.open(path) : File();
  if (!f || f.isDirectory()) {
    out.printf("\nFILE %s -1\nEOF\n", path);
    return;
  }
  out.printf("\nFILE %s %lu\n", path, (unsigned long)f.size());
  // Base64 lines with a byte sum, so log output from other tasks mixed into
  // the stream can be told apart from file data
  uint8_t buf[480];
  char enc[641];
  for (uint32_t offset = 0;; ) {
    int n = f.read(buf, sizeof buf);
    if (n <= 0) break;
    uint16_t sum = 0;
    size_t e = 0;
    for (int i = 0; i < n; i += 3) {
      uint32_t v = buf[i] << 16;
      if (i + 1 < n) v |= buf[i + 1] << 8;
      if (i + 2 < n) v |= buf[i + 2];
      enc[e++] = B64[(v >> 18) & 63];
      enc[e++] = B64[(v >> 12) & 63];
      enc[e++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
      enc[e++] = i + 2 < n ? B64[v & 63] : '=';
    }
    for (int i = 0; i < n; i++) sum += buf[i];
    enc[e] = '\0';
    out.printf("D%lu %s %04x\n", (unsigned long)offset, enc, sum);
    offset += n;
  }
  out.print("EOF\n");
}

}  // namespace storage
