#pragma once
#include <Arduino.h>

// ============================================================================
//  Pin map  —  ESP32-S3 N16R8 DevKitC-1
//  Avoided: 19/20 (native USB), 26-32 (flash), 33-37 (octal PSRAM),
//           0/3/45/46 (strapping).
// ============================================================================

// ---- CR-10 display (ST7920 128x64, software SPI) ----
static const uint8_t LCD_CLOCK_PIN = 17;  // ST7920 E   (clock)
static const uint8_t LCD_DATA_PIN  = 18;  // ST7920 R/W (data / MOSI)
static const uint8_t LCD_CS_PIN    = 21;  // ST7920 RS  (chip select)

// ---- Mode toggle ("menu" button) ----
// The cluster button wired to GPIO 2 (the one you use as the menu button).
// Switches the 4 nav buttons between HID buttons (NORMAL) and on-screen
// menu navigation (MENU). It is NOT reported as a HID button itself.
static const uint8_t MODE_TOGGLE_PIN = 2;

// ---- Always-on HID buttons ----
// Each wires GPIO -> button -> GND (internal pull-ups, no resistors needed).
// The 10 non-cluster buttons; no special function, numbering doesn't matter.
// HID button index is assigned in array order, starting at 0.
static const uint8_t ALWAYS_BUTTON_PINS[] = {7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// ---- The 4 menu/nav buttons ----
// The other four cluster buttons, listed physical TOP -> BOTTOM.
// NORMAL mode: HID buttons (indices continue after the always-on set: 10..13).
// MENU mode:   UP, DOWN, SELECT, BACK  (in this order — rearrange to taste).
static const uint8_t NAV_BUTTON_PINS[] = {1, 5, 6, 4};

static constexpr uint8_t NUM_ALWAYS = sizeof(ALWAYS_BUTTON_PINS) / sizeof(ALWAYS_BUTTON_PINS[0]);
static constexpr uint8_t NUM_NAV    = sizeof(NAV_BUTTON_PINS)    / sizeof(NAV_BUTTON_PINS[0]);
static constexpr uint8_t NUM_HID    = NUM_ALWAYS + NUM_NAV;   // 14 (USB gamepad supports 16)

// Menu navigation actions, in NAV_BUTTON_PINS order.
enum NavAction : uint8_t { NAV_UP = 0, NAV_DOWN = 1, NAV_SELECT = 2, NAV_BACK = 3 };

// Lockout debounce window (see buttons.cpp): a press registers the instant the
// contact closes, then any further edge is ignored for this long. So it both
// catches a short click and collapses contact bounce / rapid retriggers into one
// press. Set it a bit above the switch's worst-case bounce (~10-20 ms for cheap
// tactile/arcade buttons). It's also the minimum gap between two distinct presses.
static const uint16_t DEBOUNCE_MS = 50;
