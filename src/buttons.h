// Debounced physical buttons + the UI-suppression mask.
//
// Physical HID button index i (0..NUM_HID-1): the NUM_ALWAYS "always" buttons
// first, then the NUM_NAV nav buttons. The menu/toggle button is separate.
#pragma once
#include <Arduino.h>
#include "config.h"

struct Button {
  uint8_t  pin;
  bool     pressed;      // debounced state
  bool     prevPressed;  // debounced state last update (for edge detection)
  uint32_t tChange;      // millis of the last accepted state change (lockout)
};

extern Button toggleBtn;
extern Button alwaysBtns[NUM_ALWAYS];
extern Button navBtns[NUM_NAV];

// Physical buttons consumed by the UI (held across a page change); suppressed so
// they don't leak HID once back on HOME, until physically released.
extern uint32_t uiSuppressedMask;

void buttonsBegin();
void buttonsUpdate(uint32_t now);   // debounce all + clear suppression on release
bool buttonsAnyEdge();              // any button changed this update

static inline bool pressedEdge(const Button &b)  { return b.pressed && !b.prevPressed; }
static inline bool releasedEdge(const Button &b) { return !b.pressed && b.prevPressed; }
static inline bool changed(const Button &b)      { return b.pressed != b.prevPressed; }

Button &physBtn(uint8_t i);   // physical button for HID index i
uint8_t hidGpio(uint8_t i);   // GPIO of HID index i
bool    hidHeld(uint8_t i);   // pressed and not UI-suppressed
