#include "touch.h"

#include <Arduino.h>
#include <Preferences.h>

#include <algorithm>

#include "board.h"

namespace touch {

// XPT2046 control bytes: start bit, channel, 12-bit differential, PD1-0 = 00
// (power down between conversions, PENIRQ enabled)
static constexpr uint8_t CMD_READ_X = 0xD0;
static constexpr uint8_t CMD_READ_Y = 0x90;
static constexpr uint8_t CMD_READ_Z1 = 0xB0;
// Z1 rises with pressure; an untouched panel reads close to 0
static constexpr uint16_t PRESSURE_THRESHOLD = 400;
static constexpr int SAMPLES = 7;

static Calibration cal = {};
static bool haveCal = false;

static uint16_t transfer(uint8_t cmd) {
  digitalWrite(PIN_TOUCH_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_TOUCH_MOSI, (cmd >> i) & 1);
    digitalWrite(PIN_TOUCH_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_TOUCH_CLK, LOW);
    delayMicroseconds(1);
  }
  uint16_t value = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(PIN_TOUCH_CLK, HIGH);
    delayMicroseconds(1);
    value = (value << 1) | digitalRead(PIN_TOUCH_MISO);
    digitalWrite(PIN_TOUCH_CLK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(PIN_TOUCH_CS, HIGH);
  return (value >> 3) & 0x0FFF;  // one busy bit, 12 data bits, 3 zeros
}

// Median of a few conversions, to drop the odd noisy sample
static uint16_t sample(uint8_t cmd) {
  uint16_t v[SAMPLES];
  for (int i = 0; i < SAMPLES; i++) v[i] = transfer(cmd);
  std::sort(v, v + SAMPLES);
  return v[SAMPLES / 2];
}

static void loadCalibration() {
  Preferences prefs;
  if (!prefs.begin("touch", true)) return;
  if (prefs.isKey("xs")) {
    cal.swapped = prefs.getBool("swap", false);
    cal.xScale = prefs.getFloat("xs", 0);
    cal.xOffset = prefs.getFloat("xo", 0);
    cal.yScale = prefs.getFloat("ys", 0);
    cal.yOffset = prefs.getFloat("yo", 0);
    haveCal = cal.xScale != 0 && cal.yScale != 0;
  }
  prefs.end();
}

void begin() {
  pinMode(PIN_TOUCH_CS, OUTPUT);
  pinMode(PIN_TOUCH_CLK, OUTPUT);
  pinMode(PIN_TOUCH_MOSI, OUTPUT);
  pinMode(PIN_TOUCH_MISO, INPUT);
  pinMode(PIN_TOUCH_IRQ, INPUT);
  digitalWrite(PIN_TOUCH_CS, HIGH);
  digitalWrite(PIN_TOUCH_CLK, LOW);
  transfer(CMD_READ_Y);  // leaves PD = 00, which enables the touch interrupt
  loadCalibration();
}

bool touched() {
  if (digitalRead(PIN_TOUCH_IRQ) == LOW) return true;
  return transfer(CMD_READ_Z1) > PRESSURE_THRESHOLD;
}

bool readRaw(uint16_t& rawX, uint16_t& rawY) {
  if (!touched()) return false;
  rawX = sample(CMD_READ_X);
  rawY = sample(CMD_READ_Y);
  return touched();  // a reading taken while the finger lifts is unreliable
}

void apply(const Calibration& c, uint16_t rawX, uint16_t rawY, int& x, int& y) {
  float a = c.swapped ? rawY : rawX;
  float b = c.swapped ? rawX : rawY;
  x = constrain((int)lroundf(a * c.xScale + c.xOffset), 0, 319);
  y = constrain((int)lroundf(b * c.yScale + c.yOffset), 0, 239);
}

bool read(int& x, int& y) {
  uint16_t rx, ry;
  if (!haveCal || !readRaw(rx, ry)) return false;
  apply(cal, rx, ry, x, y);
  return true;
}

bool calibrated() { return haveCal; }

bool solve(const uint16_t raw[3][2], const int screen[3][2], Calibration& c) {
  // Point 0 -> 1 moves right along the screen, 1 -> 2 moves down. The raw
  // axis that changes most on the first move is the screen's X axis.
  int dX = abs(raw[1][0] - raw[0][0]);
  int dY = abs(raw[1][1] - raw[0][1]);
  c.swapped = dY > dX;
  float a0 = raw[0][c.swapped ? 1 : 0], a1 = raw[1][c.swapped ? 1 : 0];
  float b1 = raw[1][c.swapped ? 0 : 1], b2 = raw[2][c.swapped ? 0 : 1];
  if (fabsf(a1 - a0) < 300 || fabsf(b2 - b1) < 300) return false;
  c.xScale = (screen[1][0] - screen[0][0]) / (a1 - a0);
  c.xOffset = screen[0][0] - a0 * c.xScale;
  c.yScale = (screen[2][1] - screen[1][1]) / (b2 - b1);
  c.yOffset = screen[1][1] - b1 * c.yScale;
  return true;
}

void save(const Calibration& c) {
  Preferences prefs;
  if (prefs.begin("touch", false)) {
    prefs.putBool("swap", c.swapped);
    prefs.putFloat("xs", c.xScale);
    prefs.putFloat("xo", c.xOffset);
    prefs.putFloat("ys", c.yScale);
    prefs.putFloat("yo", c.yOffset);
    prefs.end();
  }
  cal = c;
  haveCal = true;
}

}  // namespace touch
