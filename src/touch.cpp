#include "touch.h"

#include <Arduino.h>

#include "board.h"

namespace touch {

// XPT2046 control bytes: start bit, channel, 12-bit differential, PD1-0 = 00
// (power down between conversions, PENIRQ enabled)
static constexpr uint8_t CMD_READ_Z1 = 0xB0;
static constexpr uint8_t CMD_READ_Y = 0x90;
// Z1 rises with pressure; an untouched panel reads close to 0
static constexpr uint16_t PRESSURE_THRESHOLD = 200;

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
  return value >> 4;  // 12-bit result, MSB first after one busy bit
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
}

bool touched() {
  if (digitalRead(PIN_TOUCH_IRQ) == LOW) return true;
  return transfer(CMD_READ_Z1) > PRESSURE_THRESHOLD;
}

}  // namespace touch
