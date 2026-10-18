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
  if (!SD.exists("/rides")) SD.mkdir("/rides");
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

}  // namespace storage
