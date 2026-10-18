#include "bike.h"

#include <NimBLEDevice.h>
#include <SD.h>

#include "storage.h"

// iFit / ProForm protocol client for the HealthRider H25X ("I_EB").
// The init and poll frames are the generic ProForm bike sequence from
// qdomyos-zwift (src/devices/proformbike). Only the iFit service is touched;
// the Nordic DFU service on the same console is never written.

namespace bike {

static constexpr const char* BIKE_NAME = "I_EB";
static const NimBLEUUID IFIT_SERVICE("00001533-1412-efde-1523-785feabcd123");
static const NimBLEUUID IFIT_WRITE("00001534-1412-efde-1523-785feabcd123");
static const NimBLEUUID IFIT_NOTIFY("00001535-1412-efde-1523-785feabcd123");

static constexpr uint32_t SCAN_MS = 5000;
static constexpr uint32_t INIT_GAP_MS = 400;
static constexpr uint32_t POLL_MS = 200;
// Raw frame log on the SD card, for decoding work
static constexpr bool LOG_FRAMES = true;

static const uint8_t INIT1[] = {0xfe, 0x02, 0x08, 0x02};
static const uint8_t INIT2[] = {0xff, 0x08, 0x02, 0x04, 0x02, 0x04, 0x02, 0x04, 0x81, 0x87,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t INIT3[] = {0xff, 0x08, 0x02, 0x04, 0x02, 0x04, 0x07, 0x04, 0x80, 0x8b,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t INIT4[] = {0xff, 0x08, 0x02, 0x04, 0x02, 0x04, 0x07, 0x04, 0x88, 0x93,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t INIT5[] = {0xfe, 0x02, 0x0a, 0x02};
static const uint8_t INIT6[] = {0xff, 0x0a, 0x02, 0x04, 0x02, 0x06, 0x02, 0x06, 0x82, 0x00,
                                0x00, 0x8a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t INIT7[] = {0xff, 0x0a, 0x02, 0x04, 0x02, 0x06, 0x02, 0x06, 0x84, 0x00,
                                0x00, 0x8c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t INIT8[] = {0xff, 0x08, 0x02, 0x04, 0x02, 0x04, 0x02, 0x04, 0x95, 0x9b,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t INIT9[] = {0xfe, 0x02, 0x2c, 0x04};
static const uint8_t INIT10[] = {0x00, 0x12, 0x02, 0x04, 0x02, 0x28, 0x07, 0x28, 0x90, 0x07,
                                 0x01, 0x10, 0xcc, 0x7a, 0x3e, 0xf4, 0xb8, 0x66, 0x3a, 0xf8};
static const uint8_t INIT11[] = {0x01, 0x12, 0xb4, 0x72, 0x46, 0x1c, 0xf0, 0xbe, 0x92, 0x40,
                                 0x3c, 0xea, 0xce, 0xa4, 0x88, 0x76, 0x4a, 0x28, 0x04, 0xe2};
static const uint8_t INIT12[] = {0xff, 0x08, 0xf6, 0xcc, 0xe0, 0x98, 0x02, 0x00, 0x00, 0xd1,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t POLL1[] = {0xfe, 0x02, 0x19, 0x03};
static const uint8_t POLL2[] = {0x00, 0x12, 0x02, 0x04, 0x02, 0x15, 0x07, 0x15, 0x02, 0x00,
                                0x0f, 0xbc, 0x90, 0x70, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00};
static const uint8_t POLL3[] = {0xff, 0x07, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x5d, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t POLL4[] = {0xfe, 0x02, 0x17, 0x03};
static const uint8_t POLL5[] = {0x00, 0x12, 0x02, 0x04, 0x02, 0x13, 0x07, 0x13, 0x02, 0x00,
                                0x0d, 0x3c, 0x9c, 0x31, 0x00, 0x00, 0x40, 0x40, 0x00, 0x80};
static const uint8_t POLL6[] = {0xff, 0x05, 0x00, 0x80, 0x01, 0x00, 0xa9, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t POLL7[] = {0xfe, 0x02, 0x0d, 0x02};

struct Frame {
  const uint8_t* data;
  size_t len;
};
#define FRAME(x) {x, sizeof(x)}

static const Frame INIT_SEQUENCE[] = {FRAME(INIT1), FRAME(INIT2), FRAME(INIT1), FRAME(INIT3),  FRAME(INIT1),
                                      FRAME(INIT4), FRAME(INIT5), FRAME(INIT6), FRAME(INIT5),  FRAME(INIT7),
                                      FRAME(INIT1), FRAME(INIT8), FRAME(INIT9), FRAME(INIT10), FRAME(INIT11),
                                      FRAME(INIT12)};
static const Frame POLL_SEQUENCE[] = {FRAME(POLL1), FRAME(POLL2), FRAME(POLL3), FRAME(POLL4),
                                      FRAME(POLL5), FRAME(POLL6), FRAME(POLL7)};

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Data current = {Link::Scanning, Workout::Unknown, 0, 0, 0, 0, 0, 0};
static File logFile;

static void setLink(Link link) {
  portENTER_CRITICAL(&lock);
  current.link = link;
  if (link != Link::Connected) {
    current.workout = Workout::Unknown;
    current.speedKmh = 0;
    current.cadenceRpm = 0;
    current.watts = 0;
    current.resistance = 0;
    current.distanceM = 0;
    current.updatedMs = 0;
  }
  portEXIT_CRITICAL(&lock);
}

static void logFrame(const char* dir, const uint8_t* data, size_t len) {
  if (!LOG_FRAMES || !logFile) return;
  logFile.printf("%lu %s", (unsigned long)millis(), dir);
  for (size_t i = 0; i < len; i++) logFile.printf(" %02x", data[i]);
  logFile.print('\n');
}

// A data message arrives as three 20-byte notifications:
//   00 12 01 04 ...  resistance level x 500 (10-11), watts (12-13), distance m (14-15), cadence (18)
//   01 12 ...        workout state (10), speed km/h x100 (13-14)
//   ff 12 ...        not used yet
// While the console is paused every value reads zero, so the last distance is kept.
static void onNotify(NimBLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
  logFrame("<<", d, len);
  // Replies to the init sequence look like data frames but hold other values
  if (current.link != Link::Connected || len != 20 || d[1] != 0x12) return;

  portENTER_CRITICAL(&lock);
  if (d[0] == 0x00 && d[2] == 0x01 && d[3] == 0x04) {
    uint16_t res = d[10] | (d[11] << 8);
    uint16_t watts = d[12] | (d[13] << 8);
    uint32_t dist = d[14] | (d[15] << 8);
    current.watts = watts <= 3000 ? watts : 0;
    current.cadenceRpm = d[18];
    // 500 per level: 500 is level 1, 10000 is level 20 (checked against the console)
    if (res >= 500 && res <= 20000) current.resistance = (res + 250) / 500;
    if (dist) current.distanceM = dist;
  } else if (d[0] == 0x01) {
    switch (d[10]) {
      case 0x0a: current.workout = Workout::Running; break;
      case 0x03: current.workout = Workout::Paused; break;
      case 0x01:
        current.workout = Workout::Idle;
        current.distanceM = 0;
        break;
      default: break;
    }
    current.speedKmh = (d[13] | (d[14] << 8)) / 100.0f;
    current.updatedMs = millis();
  }
  portEXIT_CRITICAL(&lock);
}

static bool write(NimBLERemoteCharacteristic* ch, const Frame& f) {
  logFrame(">>", f.data, f.len);
  return ch->writeValue(f.data, f.len, true);
}

static const NimBLEAdvertisedDevice* scan() {
  NimBLEScan* scanner = NimBLEDevice::getScan();
  scanner->setActiveScan(true);
  scanner->setInterval(100);
  scanner->setWindow(99);
  NimBLEScanResults results = scanner->getResults(SCAN_MS, false);
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    if (d->getName() == BIKE_NAME) return d;
  }
  return nullptr;
}

static void openLog() {
  if (!LOG_FRAMES || !storage::status().mounted) return;
  if (!SD.exists("/probe")) SD.mkdir("/probe");
  char path[48];
  snprintf(path, sizeof path, "/probe/frames-%lu.log", (unsigned long)time(nullptr));
  logFile = SD.open(path, FILE_WRITE);
}

static void session(NimBLEClient* client) {
  NimBLERemoteService* svc = client->getService(IFIT_SERVICE);
  NimBLERemoteCharacteristic* wr = svc ? svc->getCharacteristic(IFIT_WRITE) : nullptr;
  NimBLERemoteCharacteristic* nt = svc ? svc->getCharacteristic(IFIT_NOTIFY) : nullptr;
  if (!wr || !nt || !nt->subscribe(true, onNotify)) {
    Serial.println("bike: iFit service not usable");
    return;
  }

  openLog();
  setLink(Link::Initializing);
  for (const Frame& f : INIT_SEQUENCE) {
    if (!client->isConnected()) return;
    write(wr, f);
    delay(INIT_GAP_MS);
  }

  setLink(Link::Connected);
  Serial.println("bike: connected and polling");
  size_t step = 0;
  uint32_t lastFlush = millis();
  while (client->isConnected()) {
    write(wr, POLL_SEQUENCE[step]);
    step = (step + 1) % (sizeof POLL_SEQUENCE / sizeof POLL_SEQUENCE[0]);
    if (logFile && millis() - lastFlush > 5000) {
      logFile.flush();
      lastFlush = millis();
    }
    delay(POLL_MS);
  }
}

static void run(void*) {
  NimBLEDevice::init("bike-cyd");
  for (;;) {
    setLink(Link::Scanning);
    const NimBLEAdvertisedDevice* found = scan();
    if (!found) {
      delay(1000);
      continue;
    }

    setLink(Link::Connecting);
    NimBLEClient* client = NimBLEDevice::createClient();
    if (client->connect(found)) {
      session(client);
      client->disconnect();
    } else {
      Serial.println("bike: connect failed");
    }
    NimBLEDevice::deleteClient(client);
    if (logFile) logFile.close();
    Serial.println("bike: disconnected");
    delay(2000);
  }
}

void start() { xTaskCreatePinnedToCore(run, "bike", 8192, nullptr, 1, nullptr, 0); }

Data get() {
  portENTER_CRITICAL(&lock);
  Data copy = current;
  portEXIT_CRITICAL(&lock);
  return copy;
}

const char* linkText(Link link) {
  switch (link) {
    case Link::Scanning: return "scanning";
    case Link::Connecting: return "connecting";
    case Link::Initializing: return "starting up";
    case Link::Connected: return "connected";
  }
  return "?";
}

}  // namespace bike
