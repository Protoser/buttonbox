#include "buttons.h"

Button   toggleBtn;
Button   alwaysBtns[NUM_ALWAYS];
Button   navBtns[NUM_NAV];
uint32_t uiSuppressedMask = 0;

static void initButton(Button &b, uint8_t pin) {
  pinMode(pin, INPUT_PULLUP);
  b = {pin, false, false, 0};
}

// Lockout debounce: accept a state change the instant the raw level differs,
// provided the debounce window has elapsed since the last accepted change. This
// registers a press on first contact, so quick taps survive even when the loop
// is briefly stalled by a slow display redraw.
static void updateButton(Button &b, uint32_t now) {
  b.prevPressed = b.pressed;
  bool raw = (digitalRead(b.pin) == LOW);   // pull-up: pressed pulls to GND
  if (raw != b.pressed && (now - b.tChange) >= DEBOUNCE_MS) {
    b.pressed = raw;
    b.tChange = now;
  }
}

void buttonsBegin() {
  initButton(toggleBtn, MODE_TOGGLE_PIN);
  for (uint8_t i = 0; i < NUM_ALWAYS; i++) initButton(alwaysBtns[i], ALWAYS_BUTTON_PINS[i]);
  for (uint8_t i = 0; i < NUM_NAV; i++)    initButton(navBtns[i],    NAV_BUTTON_PINS[i]);
}

void buttonsUpdate(uint32_t now) {
  updateButton(toggleBtn, now);
  for (auto &b : alwaysBtns) updateButton(b, now);
  for (auto &b : navBtns)    updateButton(b, now);
  for (uint8_t i = 0; i < NUM_HID; i++)
    if (releasedEdge(physBtn(i))) uiSuppressedMask &= ~(1u << i);
}

bool buttonsAnyEdge() {
  if (changed(toggleBtn)) return true;
  for (auto &b : alwaysBtns) if (changed(b)) return true;
  for (auto &b : navBtns)    if (changed(b)) return true;
  return false;
}

Button &physBtn(uint8_t i) { return (i < NUM_ALWAYS) ? alwaysBtns[i] : navBtns[i - NUM_ALWAYS]; }
uint8_t hidGpio(uint8_t i) { return (i < NUM_ALWAYS) ? ALWAYS_BUTTON_PINS[i] : NAV_BUTTON_PINS[i - NUM_ALWAYS]; }
bool    hidHeld(uint8_t i) { return physBtn(i).pressed && !(uiSuppressedMask & (1u << i)); }
