#include "hid.h"
#include "keyboard.h"
#include "settings.h"

void hidBegin() {
  kbBegin();
}

// An output emits its bound key while held. keyKey == 0 means unbound: nothing is
// sent (the box stays silent for that output). Release is unconditional and cheap —
// kbRelease no-ops if the output was never pressed.
void hidEmitPress(uint8_t out) {
  if (out >= KEYMAP_N) return;
  uint8_t key = settings.keyKey[out];
  if (key) kbPress(out, settings.keyMod[out], key);
}

void hidEmitRelease(uint8_t out) {
  kbRelease(out);
}
