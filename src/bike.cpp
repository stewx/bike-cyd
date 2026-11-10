#include "bike.h"

#include <NimBLEDevice.h>
#include <SD.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <vector>

#include "logger.h"
#include "settings.h"
#include "storage.h"

// iFit / ProForm protocol client for the HealthRider H25X ("I_EB").
// The init and poll frames are the generic ProForm bike sequence from
// qdomyos-zwift (src/devices/proformbike). Only the iFit service is touched;
// the Nordic DFU service on the same console is never written.

namespace bike {

static const NimBLEUUID IFIT_SERVICE("00001533-1412-efde-1523-785feabcd123");
static const NimBLEUUID IFIT_WRITE("00001534-1412-efde-1523-785feabcd123");
static const NimBLEUUID IFIT_NOTIFY("00001535-1412-efde-1523-785feabcd123");

static constexpr uint32_t SCAN_MS = 5000;
static constexpr uint32_t INIT_GAP_MS = 400;
static constexpr uint32_t POLL_MS = 200;
// Polling keeps the console awake, so it lets go after settings releaseMin
// without riding.
// After letting go: the console counts as asleep once it stops advertising
// for this long. It normally sleeps about 5 minutes after the disconnect, so
// if it is still awake after REST_MAX_MS someone is probably using it.
static constexpr uint32_t ASLEEP_MS = 60 * 1000;
static constexpr uint32_t REST_MAX_MS = 7 * 60 * 1000;
// Raw frame log on the SD card, for decoding work; one file per connection
static constexpr bool LOG_FRAMES = true;
static constexpr size_t MAX_FRAME_LOGS = 20;

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
static uint8_t unknownState = 0;  // last workout state byte we couldn't decode
static uint8_t loggedState = 0;
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
//   01 12 ...        workout state flags (10), speed km/h x100 (13-14)
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
    // Workout state flags: 0x02 = workout active, 0x01 = stopped. Seen: 0x01
    // idle, 0x03 paused, 0x02 and 0x0a running (0x08's meaning is unknown).
    const uint8_t s = d[10];
    current.speedKmh = (d[13] | (d[14] << 8)) / 100.0f;
    if (s == 0x01) {
      current.workout = Workout::Idle;
      current.distanceM = 0;
    } else if ((s & 0x03) == 0x03) {
      current.workout = Workout::Paused;
    } else if (s & 0x02) {
      current.workout = Workout::Running;
    } else {
      unknownState = s;
      // Unknown state: going by the pedals is the safest guess
      if (current.cadenceRpm || current.speedKmh > 0) current.workout = Workout::Running;
    }
    current.updatedMs = millis();
  }
  portEXIT_CRITICAL(&lock);
  if (unknownState != loggedState && unknownState) {
    loggedState = unknownState;
    logger::printf("bike: unknown workout state 0x%02x\n", unknownState);
  }
}

static bool write(NimBLERemoteCharacteristic* ch, const Frame& f) {
  logFrame(">>", f.data, f.len);
  return ch->writeValue(f.data, f.len, true);
}

static const NimBLEAdvertisedDevice* scan() {
  esp_task_wdt_reset();
  NimBLEScan* scanner = NimBLEDevice::getScan();
  scanner->setActiveScan(true);
  scanner->setInterval(100);
  scanner->setWindow(99);
  NimBLEScanResults results = scanner->getResults(SCAN_MS, false);
  const std::string bikeName = settings::get().bikeName;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    if (d->getName() == bikeName) return d;
  }
  return nullptr;
}

// Deletes the oldest frame logs so at most MAX_FRAME_LOGS - 1 remain
static void pruneLogs() {
  File dir = SD.open("/probe");
  if (!dir) return;
  std::vector<String> names;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) names.push_back(f.name());
  }
  dir.close();
  if (names.size() < MAX_FRAME_LOGS) return;
  // Names hold the start time, but "frames-7.log" (no clock yet) sorts by length first
  std::sort(names.begin(), names.end(), [](const String& a, const String& b) {
    return a.length() != b.length() ? a.length() < b.length() : a < b;
  });
  for (size_t i = 0; i + MAX_FRAME_LOGS - 1 < names.size(); i++) SD.remove("/probe/" + names[i]);
}

static void openLog() {
  if (!LOG_FRAMES || !storage::status().mounted) return;
  if (!SD.exists("/probe")) SD.mkdir("/probe");
  pruneLogs();
  char path[48];
  snprintf(path, sizeof path, "/probe/frames-%lu.log", (unsigned long)time(nullptr));
  logFile = SD.open(path, FILE_WRITE);
}

// Returns true if it let go of the bike because nobody was riding.
static bool session(NimBLEClient* client) {
  NimBLERemoteService* svc = client->getService(IFIT_SERVICE);
  NimBLERemoteCharacteristic* wr = svc ? svc->getCharacteristic(IFIT_WRITE) : nullptr;
  NimBLERemoteCharacteristic* nt = svc ? svc->getCharacteristic(IFIT_NOTIFY) : nullptr;
  if (!wr || !nt || !nt->subscribe(true, onNotify)) {
    logger::printf("bike: iFit service not usable\n");
    return false;
  }

  openLog();
  setLink(Link::Initializing);
  for (const Frame& f : INIT_SEQUENCE) {
    if (!client->isConnected()) return false;
    esp_task_wdt_reset();
    write(wr, f);
    delay(INIT_GAP_MS);
  }

  setLink(Link::Connected);
  logger::printf("bike: connected and polling\n");
  size_t step = 0;
  uint32_t lastFlush = millis();
  uint32_t lastRiding = millis();
  while (client->isConnected()) {
    esp_task_wdt_reset();
    write(wr, POLL_SEQUENCE[step]);
    step = (step + 1) % (sizeof POLL_SEQUENCE / sizeof POLL_SEQUENCE[0]);
    if (logFile && millis() - lastFlush > 5000) {
      logFile.flush();
      lastFlush = millis();
    }
    if (get().workout == Workout::Running) {
      lastRiding = millis();
    } else if (millis() - lastRiding > settings::get().releaseMin * 60000UL) {
      logger::printf("bike: idle for %u minutes, disconnecting so the console can sleep\n",
                     settings::get().releaseMin);
      return true;
    }
    delay(POLL_MS);
  }
  return false;
}

static bool seen(const NimBLEAddress& addr) {
  esp_task_wdt_reset();
  NimBLEScan* scanner = NimBLEDevice::getScan();
  scanner->setActiveScan(false);  // listen only; don't poke the console
  NimBLEScanResults results = scanner->getResults(SCAN_MS, false);
  for (int i = 0; i < results.getCount(); i++) {
    if (results.getDevice(i)->getAddress() == addr) return true;
  }
  return false;
}

// Stays away from the bike until it stops advertising (console asleep), so
// that the next advertisement means someone has woken it up to ride.
static void rest(const NimBLEAddress& addr) {
  setLink(Link::Resting);
  uint32_t start = millis();
  uint32_t lastSeen = millis();
  while (millis() - start < REST_MAX_MS) {
    if (seen(addr)) {
      lastSeen = millis();
    } else if (millis() - lastSeen > ASLEEP_MS) {
      logger::printf("bike: console asleep after %lu s\n", (unsigned long)((lastSeen - start) / 1000));
      return;
    }
  }
  logger::printf("bike: console still awake after 7 minutes, reconnecting\n");
}

static void run(void*) {
  esp_task_wdt_add(nullptr);  // a hang anywhere in here restarts the board
  NimBLEDevice::init("bike-cyd");
  for (;;) {
    setLink(Link::Scanning);
    const NimBLEAdvertisedDevice* found = scan();
    if (!found) {
      delay(1000);
      continue;
    }

    setLink(Link::Connecting);
    const NimBLEAddress addr = found->getAddress();
    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectTimeout(10000);  // well inside the 60 s watchdog
    bool released = false;
    if (client->connect(found)) {
      released = session(client);
      client->disconnect();
    } else {
      logger::printf("bike: connect failed\n");
    }
    NimBLEDevice::deleteClient(client);
    if (logFile) logFile.close();
    logger::printf("bike: disconnected\n");
    if (released) rest(addr);
    else delay(2000);
  }
}

void start() { xTaskCreatePinnedToCore(run, "bike", 8192, nullptr, 1, nullptr, 0); }

static uint32_t simStartMs = 0;
static uint32_t simLengthMs = 0;

bool simulating() { return simLengthMs != 0; }

void simulate(uint32_t seconds) {
  simStartMs = millis();
  simLengthMs = seconds * 1000;
  logger::printf("bike: simulating a %lu s ride\n", (unsigned long)seconds);
}

// Made-up data in the shape the console sends: a message every 2.3 s,
// running, then paused for 20 s halfway, then running, then ended.
static bool simulated(Data& d) {
  if (!simLengthMs) return false;
  const uint32_t elapsed = millis() - simStartMs;
  if (elapsed > simLengthMs + 15000) {
    simLengthMs = 0;
    logger::printf("bike: simulation over\n");
    return false;
  }
  const uint32_t t = elapsed / 2300 * 2300;  // time of the latest message
  const uint32_t pauseStart = simLengthMs / 2, pauseEnd = pauseStart + 20000;
  const uint32_t ridden = t < pauseStart ? t : t < pauseEnd ? pauseStart : t - 20000;

  d.link = Link::Connected;
  d.updatedMs = simStartMs + t;
  d.resistance = 8;
  d.distanceM = (uint32_t)(ridden / 1000.0f * 6.9f);  // about 25 km/h
  if (t >= simLengthMs) {
    d.workout = Workout::Idle;
  } else if (t >= pauseStart && t < pauseEnd) {
    d.workout = Workout::Paused;
  } else {
    d.workout = Workout::Running;
    float s = ridden / 1000.0f;
    d.cadenceRpm = (uint16_t)(75 + 10 * sinf(s * 6.2832f / 60));
    d.speedKmh = d.cadenceRpm * 0.33f;
    d.watts = (uint16_t)(140 + 50 * sinf(s * 6.2832f / 45));
    return true;
  }
  d.cadenceRpm = 0;
  d.speedKmh = 0;
  d.watts = 0;
  return true;
}

Data get() {
  portENTER_CRITICAL(&lock);
  Data copy = current;
  portEXIT_CRITICAL(&lock);
  simulated(copy);
  return copy;
}

const char* linkText(Link link) {
  switch (link) {
    case Link::Scanning: return "scanning";
    case Link::Connecting: return "connecting";
    case Link::Initializing: return "starting up";
    case Link::Connected: return "connected";
    case Link::Resting: return "resting";
  }
  return "?";
}

}  // namespace bike
