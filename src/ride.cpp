#include "ride.h"

#include <SD.h>
#include <time.h>

#include "logger.h"
#include "net.h"
#include "settings.h"
#include "storage.h"

namespace ride {

static constexpr const char* RECORDING_DIR = "/rides/recording";
static constexpr const char* FINISHED_DIR = "/rides/finished";

// Data older than this means the bike has gone quiet
static constexpr uint32_t STALE_MS = 10000;
static constexpr uint32_t FLUSH_MS = 10000;

static Stats current = {};
static uint32_t startMs = 0;
static uint32_t lastRunningMs = 0;
static uint32_t lastSampleMs = 0;  // bike::Data::updatedMs of the last row written
static uint32_t lastFlushMs = 0;
static uint32_t baseDistanceM = 0;  // console distance when the ride started
static time_t startEpoch = 0;       // 0 until the clock is known

static File file;
static String fileName;
static Summary last = {};

static void moveFile(const char* toDir) {
  String from = String(RECORDING_DIR) + "/" + fileName;
  String to = String(toDir) + "/" + fileName;
  if (!SD.rename(from, to)) logger::printf("ride: could not move %s\n", from.c_str());
}

void begin() {
  if (!storage::status().mounted) return;
  File dir = SD.open(RECORDING_DIR);
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String name = f.name();
    f.close();
    logger::printf("ride: recovering interrupted ride %s\n", name.c_str());
    SD.rename(String(RECORDING_DIR) + "/" + name, String(FINISHED_DIR) + "/" + name);
  }
}

// Records the ride's start time once the clock is known. Rows hold seconds
// since the start, so a ride that began before NTP synced still gets the
// right times.
static void noteStartTime() {
  if (startEpoch || !net::timeSynced() || !file) return;
  startEpoch = time(nullptr) - (millis() - startMs) / 1000;
  file.printf("# start_epoch=%ld\n", (long)startEpoch);
}

static void startRide(const bike::Data& d) {
  current = {};
  current.active = true;
  startMs = lastRunningMs = lastFlushMs = millis();
  lastSampleMs = 0;
  baseDistanceM = d.distanceM;
  startEpoch = 0;
  fileName = "";

  if (!storage::status().mounted) {
    logger::printf("ride: no SD card, not recording\n");
    return;
  }
  char name[40];
  if (net::timeSynced()) {
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(name, sizeof name, "%Y%m%d-%H%M%S.csv", &t);
  } else {
    snprintf(name, sizeof name, "nosync-%08lx.csv", (unsigned long)esp_random());
  }
  // Simulated test rides are marked so they never pass for real ones
  fileName = bike::simulating() ? String("sim-") + name : String(name);
  file = SD.open(String(RECORDING_DIR) + "/" + fileName, FILE_WRITE);
  if (!file) {
    logger::printf("ride: could not create %s\n", name);
    return;
  }
  file.print("# bike-cyd ride v1\n# columns: t,speed_kmh,cadence_rpm,watts,distance_m,resistance\n");
  noteStartTime();
  current.recording = true;
  logger::printf("ride: started, recording %s\n", name);
}

static void endRide() {
  logger::printf("ride: ended after %lu s moving, %lu m\n", (unsigned long)current.movingSeconds,
                (unsigned long)current.distanceM);
  last = {};
  last.valid = true;
  if (file) {
    noteStartTime();
    file.close();
    moveFile(FINISHED_DIR);
    strlcpy(last.csvName, fileName.c_str(), sizeof last.csvName);
  }
  last.movingSeconds = current.movingSeconds;
  last.distanceM = current.distanceM;
  last.avgKmh = current.avgKmh;
  last.endedMs = millis();
  current = {};
}

void endNow() {
  if (!current.active) return;
  logger::printf("ride: ended from the screen\n");
  endRide();
}

Summary lastRide() { return last; }

void tick(const bike::Data& d) {
  const uint32_t now = millis();
  const bool fresh = d.link == bike::Link::Connected && d.updatedMs && now - d.updatedMs < STALE_MS;
  const bool running = fresh && d.workout == bike::Workout::Running;
  const bool consoleEnded = fresh && d.workout == bike::Workout::Idle;

  if (!current.active) {
    if (running) startRide(d);
    return;
  }

  if (running) {
    lastRunningMs = now;
    current.paused = false;
    current.movingSeconds++;
    if (d.distanceM >= baseDistanceM) current.distanceM = d.distanceM - baseDistanceM;
    if (d.speedKmh > current.maxKmh) current.maxKmh = d.speedKmh;
    current.avgKmh = current.distanceM * 3.6f / current.movingSeconds;

    if (file && d.updatedMs != lastSampleMs) {
      lastSampleMs = d.updatedMs;
      int32_t sinceStart = (int32_t)(d.updatedMs - startMs);  // the first message can predate the start
      file.printf("%.1f,%.2f,%u,%u,%lu,%u\n", max(sinceStart, (int32_t)0) / 1000.0f, d.speedKmh, d.cadenceRpm,
                  d.watts, (unsigned long)current.distanceM, d.resistance);
    }
  } else {
    current.paused = true;
    // A ride ends after settings rideEndMin without riding (paused or disconnected)
    if (consoleEnded || now - lastRunningMs > settings::get().rideEndMin * 60000UL) {
      endRide();
      return;
    }
  }

  noteStartTime();
  if (file && now - lastFlushMs > FLUSH_MS) {
    file.flush();
    lastFlushMs = now;
  }
}

Stats stats() { return current; }

}  // namespace ride
