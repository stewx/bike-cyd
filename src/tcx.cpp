#include "tcx.h"

#include <SD.h>
#include <esp_task_wdt.h>
#include <time.h>

#include <vector>

#include "logger.h"

namespace tcx {

static constexpr const char* FINISHED_DIR = "/rides/finished";
static constexpr const char* PENDING_DIR = "/rides/pending";
static constexpr const char* DONE_DIR = "/rides/done";
static constexpr const char* CSV_DIR = "/rides/csv";
static constexpr const char* DISCARDED_DIR = "/rides/discarded";
static constexpr const char* FAILED_DIR = "/rides/failed";

// Rides shorter than this are not worth uploading
static constexpr float MIN_MOVING_S = 120;
static constexpr unsigned long MIN_DISTANCE_M = 500;
// Gaps between samples longer than this are pauses, not riding
static constexpr float MAX_GAP_S = 10;

struct Sample {
  float t;
  float kmh;
  unsigned cadence;
  unsigned watts;
  unsigned long distanceM;
  unsigned resistance;
};

struct Summary {
  long startEpoch = 0;
  int rows = 0;
  float movingS = 0;
  unsigned long distanceM = 0;
  float maxKmh = 0;
  double energyJ = 0;
  unsigned maxWatts = 0;
  double cadenceSum = 0;  // cadence x seconds while pedalling
  float cadenceS = 0;
  unsigned maxCadence = 0;
};

// The console derives speed from cadence (about 0.33 km/h per rpm) and can
// report a spike as a workout starts (38 km/h at 30 rpm), so implausible
// speeds are replaced with the cadence-based value.
static bool parseRow(const String& line, Sample& s) {
  if (sscanf(line.c_str(), "%f,%f,%u,%u,%lu,%u", &s.t, &s.kmh, &s.cadence, &s.watts, &s.distanceM,
             &s.resistance) != 6) {
    return false;
  }
  if (s.kmh > s.cadence * 0.6f + 5) s.kmh = s.cadence * 0.33f;
  return true;
}

static bool summarize(File& f, Summary& sum) {
  Sample prev = {}, s;
  bool havePrev = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.startsWith("# start_epoch=")) {
      sum.startEpoch = line.substring(14).toInt();
      continue;
    }
    if (line.startsWith("#") || !parseRow(line, s)) continue;
    sum.rows++;
    sum.distanceM = s.distanceM;
    if (s.kmh > sum.maxKmh) sum.maxKmh = s.kmh;
    if (s.watts > sum.maxWatts) sum.maxWatts = s.watts;
    if (s.cadence > sum.maxCadence) sum.maxCadence = s.cadence;
    if (havePrev) {
      float dt = s.t - prev.t;
      if (dt > 0 && dt <= MAX_GAP_S) {
        sum.movingS += dt;
        sum.energyJ += s.watts * dt;
        if (s.cadence) {
          sum.cadenceSum += s.cadence * dt;
          sum.cadenceS += dt;
        }
      }
    }
    prev = s;
    havePrev = true;
  }
  return sum.rows > 0;
}

static void isoTime(char* out, size_t len, long epoch) {
  time_t t = epoch;
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void writeTcx(File& in, File& out, const Summary& sum) {
  char ts[24];
  isoTime(ts, sizeof ts, sum.startEpoch);
  const unsigned avgWatts = sum.movingS > 0 ? (unsigned)(sum.energyJ / sum.movingS + 0.5) : 0;
  const unsigned avgCadence = sum.cadenceS > 0 ? (unsigned)(sum.cadenceSum / sum.cadenceS + 0.5) : 0;
  const float avgSpeed = sum.movingS > 0 ? sum.distanceM / sum.movingS : 0;
  // Mechanical work in kJ is a common stand-in for kcal burned on a bike
  const unsigned calories = (unsigned)(sum.energyJ / 1000 + 0.5);

  out.print(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<TrainingCenterDatabase xmlns=\"http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2\" "
      "xmlns:ns3=\"http://www.garmin.com/xmlschemas/ActivityExtension/v2\">\n"
      " <Activities>\n"
      "  <Activity Sport=\"Biking\">\n");
  out.printf("   <Id>%s</Id>\n", ts);
  out.printf("   <Lap StartTime=\"%s\">\n", ts);
  out.printf("    <TotalTimeSeconds>%.0f</TotalTimeSeconds>\n", sum.movingS);
  out.printf("    <DistanceMeters>%lu</DistanceMeters>\n", sum.distanceM);
  out.printf("    <MaximumSpeed>%.2f</MaximumSpeed>\n", sum.maxKmh / 3.6f);
  out.printf("    <Calories>%u</Calories>\n", calories);
  out.print("    <Intensity>Active</Intensity>\n");
  out.printf("    <Cadence>%u</Cadence>\n", avgCadence);
  out.print("    <TriggerMethod>Manual</TriggerMethod>\n    <Track>\n");

  in.seek(0);
  Sample s;
  while (in.available()) {
    esp_task_wdt_reset();  // long rides take a while to write
    String line = in.readStringUntil('\n');
    if (line.startsWith("#") || !parseRow(line, s)) continue;
    isoTime(ts, sizeof ts, sum.startEpoch + lroundf(s.t));
    out.printf(
        "     <Trackpoint><Time>%s</Time><DistanceMeters>%lu</DistanceMeters><Cadence>%u</Cadence>"
        "<Extensions><ns3:TPX><ns3:Speed>%.2f</ns3:Speed><ns3:Watts>%u</ns3:Watts></ns3:TPX></Extensions>"
        "</Trackpoint>\n",
        ts, s.distanceM, s.cadence, s.kmh / 3.6f, s.watts);
  }

  out.print("    </Track>\n    <Extensions><ns3:LX>");
  out.printf("<ns3:AvgSpeed>%.2f</ns3:AvgSpeed><ns3:MaxBikeCadence>%u</ns3:MaxBikeCadence>", avgSpeed,
             sum.maxCadence);
  out.printf("<ns3:AvgWatts>%u</ns3:AvgWatts><ns3:MaxWatts>%u</ns3:MaxWatts>", avgWatts, sum.maxWatts);
  out.print(
      "</ns3:LX></Extensions>\n"
      "   </Lap>\n"
      "   <Notes>Indoor ride on a HealthRider H25X, recorded by bike-cyd</Notes>\n"
      "  </Activity>\n"
      " </Activities>\n"
      "</TrainingCenterDatabase>\n");
}

// "2026-10-18_2145.tcx" in local time (with "sim-" in front for simulated
// rides), made unique against earlier uploads
static String tcxName(long startEpoch, const String& csvName) {
  time_t t = startEpoch;
  struct tm tm;
  localtime_r(&t, &tm);
  char stamp[24];
  strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M", &tm);
  String base = csvName.startsWith("sim-") ? String("sim-") + stamp : String(stamp);
  String name = base + ".tcx";
  for (int n = 2; SD.exists(String(PENDING_DIR) + "/" + name) || SD.exists(String(DONE_DIR) + "/" + name); n++) {
    name = base + "_" + n + ".tcx";
  }
  return name;
}

static void moveCsv(const String& name, const char* toDir) {
  String to = String(toDir) + "/" + name;
  if (SD.exists(to)) SD.remove(to);
  SD.rename(String(FINISHED_DIR) + "/" + name, to);
}

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Result last = {};

static void setResult(const String& csvName, const String& tcxName, Outcome outcome) {
  portENTER_CRITICAL(&lock);
  strlcpy(last.csv, csvName.c_str(), sizeof last.csv);
  strlcpy(last.tcx, tcxName.c_str(), sizeof last.tcx);
  last.outcome = outcome;
  portEXIT_CRITICAL(&lock);
}

Result lastResult() {
  portENTER_CRITICAL(&lock);
  Result copy = last;
  portEXIT_CRITICAL(&lock);
  return copy;
}

// Returns true if a TCX was written
static bool exportOne(const String& csvName) {
  const String csvPath = String(FINISHED_DIR) + "/" + csvName;
  File in = SD.open(csvPath);
  if (!in) return false;

  Summary sum;
  if (!summarize(in, sum)) {
    in.close();
    logger::printf("tcx: %s has no samples\n", csvName.c_str());
    moveCsv(csvName, DISCARDED_DIR);
    setResult(csvName, "", Outcome::TooShort);
    return false;
  }
  if (sum.movingS < MIN_MOVING_S || sum.distanceM < MIN_DISTANCE_M) {
    in.close();
    logger::printf("tcx: %s too short (%.0f s, %lu m), discarded\n", csvName.c_str(), sum.movingS, sum.distanceM);
    moveCsv(csvName, DISCARDED_DIR);
    setResult(csvName, "", Outcome::TooShort);
    return false;
  }
  if (!sum.startEpoch) {
    in.close();
    logger::printf("tcx: %s has no start time\n", csvName.c_str());
    moveCsv(csvName, FAILED_DIR);
    setResult(csvName, "", Outcome::Failed);
    return false;
  }

  // Written next to the CSV first, so the uploader never sees a partial file
  const String name = tcxName(sum.startEpoch, csvName);
  const String tmpPath = String(FINISHED_DIR) + "/" + name + ".tmp";
  File out = SD.open(tmpPath, FILE_WRITE);
  if (!out) {
    in.close();
    logger::printf("tcx: could not create %s\n", tmpPath.c_str());
    return false;
  }
  writeTcx(in, out, sum);
  in.close();
  out.close();

  if (!SD.rename(tmpPath, String(PENDING_DIR) + "/" + name)) {
    logger::printf("tcx: could not queue %s\n", name.c_str());
    SD.remove(tmpPath);
    return false;
  }
  moveCsv(csvName, CSV_DIR);
  setResult(csvName, name, Outcome::Exported);
  logger::printf("tcx: %s -> %s (%.0f s, %lu m, %d points)\n", csvName.c_str(), name.c_str(), sum.movingS,
                sum.distanceM, sum.rows);
  return true;
}

int exportFinished() {
  File dir = SD.open(FINISHED_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  std::vector<String> csvs;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    f.close();
    if (name.endsWith(".csv")) csvs.push_back(name);
    else if (name.endsWith(".tmp")) SD.remove(String(FINISHED_DIR) + "/" + name);  // left by a reset
  }
  dir.close();

  int written = 0;
  for (const String& name : csvs) {
    if (exportOne(name)) written++;
  }
  return written;
}

}  // namespace tcx
