#pragma once

// Pin map for the ESP32-2432S028 (Cheap Yellow Display), 2USB revision.

// Display (ST7789) on SPI2 / HSPI
constexpr int PIN_TFT_SCLK = 14;
constexpr int PIN_TFT_MOSI = 13;
constexpr int PIN_TFT_MISO = 12;
constexpr int PIN_TFT_DC = 2;
constexpr int PIN_TFT_CS = 15;
constexpr int PIN_TFT_BL = 21;

// microSD on SPI3 / VSPI
constexpr int PIN_SD_SCK = 18;
constexpr int PIN_SD_MISO = 19;
constexpr int PIN_SD_MOSI = 23;
constexpr int PIN_SD_CS = 5;

// RGB LED, active low
constexpr int PIN_LED_R = 4;
constexpr int PIN_LED_G = 16;
constexpr int PIN_LED_B = 17;

// Light sensor (LDR)
constexpr int PIN_LDR = 34;

// Touch controller (XPT2046) interrupt line: low while the screen is touched
constexpr int PIN_TOUCH_IRQ = 36;
