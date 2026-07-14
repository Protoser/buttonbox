#include "chords.h"
#include "buttons.h"
#include "hid.h"
#include "settings.h"
#include "ui.h"
#include <Preferences.h>

// A special (non-HID) output drives a UI action instead of a gamepad button.
static inline bool outIsSpecial(uint8_t o) { return o >= 32; }

Chord    chords[MAX_CHORDS];
uint8_t  chordCount = 0;
uint32_t chordMemberMask = 0;

// Engine state (HOME only)
static uint32_t emittedMask    = 0;   // physical buttons with an individual HID press active
static uint32_t chordOwnedMask = 0;   // physical buttons consumed by a chord
static uint32_t pendingMask    = 0;   // chord members held inside the window, solo press deferred
static int8_t   activeChord    = -1;
static uint32_t downTime[NUM_HID] = {0};   // millis the button was pressed
static uint32_t emitTime[NUM_HID] = {0};   // millis its solo HID press was emitted

// A solo press is held at least this long even if released sooner, so a quick tap
// isn't shorter than the host's USB polling interval and gets missed.
static const uint16_t MIN_PRESS_MS = 20;

void recomputeChordMask() {
  chordMemberMask = 0;
  for (uint8_t i = 0; i < chordCount; i++) chordMemberMask |= chords[i].members;
}

void chordsLoad() {
  Preferences p;
  p.begin("bbox", true);
  chordCount = p.getUChar("nchords", 0);
  if (chordCount > MAX_CHORDS) chordCount = MAX_CHORDS;
  if (chordCount) p.getBytes("chords", chords, chordCount * sizeof(Chord));
  p.end();
  recomputeChordMask();
}

void chordsSave() {
  Preferences p;
  p.begin("bbox", false);
  p.putUChar("nchords", chordCount);
  p.putBytes("chords", chords, chordCount * sizeof(Chord));
  p.end();
}

uint8_t firstFreeOutput() {
  for (uint8_t o = NUM_HID; o < 32; o++) {
    bool used = false;
    for (uint8_t c = 0; c < chordCount; c++) if (chords[c].output == o) { used = true; break; }
    if (!used) return o;
  }
  return NUM_HID;
}

int8_t activeChordOutput() { return (activeChord >= 0) ? (int8_t)chords[activeChord].output : -1; }

void resetChordEngine() {
  for (uint8_t i = 0; i < NUM_HID; i++) if (emittedMask & (1u << i)) gamepad.releaseButton(i);
  if (activeChord >= 0 && !outIsSpecial(chords[activeChord].output))
    gamepad.releaseButton(chords[activeChord].output);
  emittedMask = 0; chordOwnedMask = 0; pendingMask = 0; activeChord = -1;
}

bool chordToggleHeld() { return (chordOwnedMask & (1u << CHORD_MEMBER_TOGGLE)) != 0; }

// Physical buttons that appear in a chord together with the menu button. While the
// menu button is held, the UI leaves these to the chord engine (see uiHandlePageInput)
// so a Menu+nav chord can form on pages where nav buttons are normally claimed.
uint32_t chordToggleBuddyMask() {
  uint32_t m = 0;
  for (uint8_t c = 0; c < chordCount; c++)
    if (chords[c].members & (1u << CHORD_MEMBER_TOGGLE)) m |= chords[c].members;
  return m & ~(1u << CHORD_MEMBER_TOGGLE);
}

void updateChords(uint32_t now) {
  uint32_t physMask = 0;
  for (uint8_t i = 0; i < NUM_HID; i++) {
    if (hidHeld(i)) { physMask |= (1u << i); if (pressedEdge(physBtn(i))) downTime[i] = now; }
  }
  // The menu/toggle button can be a chord member (bit CHORD_MEMBER_TOGGLE). It has no
  // HID identity, so it only ever joins a chord — never emits a button on its own.
  if (toggleBtn.pressed) physMask |= (1u << CHORD_MEMBER_TOGGLE);

  // Best matching chord: members fully held, most members wins (tie -> lowest).
  int8_t best = -1; uint8_t bestCount = 0;
  for (uint8_t c = 0; c < chordCount; c++) {
    if ((chords[c].members & physMask) == chords[c].members) {
      uint8_t cnt = __builtin_popcount(chords[c].members);
      if (cnt > bestCount) { bestCount = cnt; best = c; }
    }
  }

  if (activeChord >= 0 && (chords[activeChord].members & physMask) != chords[activeChord].members) {
    if (!outIsSpecial(chords[activeChord].output)) gamepad.releaseButton(chords[activeChord].output);
    activeChord = -1;
  }

  if (best >= 0 && best != activeChord) {
    for (uint8_t i = 0; i < NUM_HID; i++)
      if ((chords[best].members & (1u << i)) && (emittedMask & (1u << i))) { gamepad.releaseButton(i); emittedMask &= ~(1u << i); }
    if (activeChord >= 0 && !outIsSpecial(chords[activeChord].output)) gamepad.releaseButton(chords[activeChord].output);
    // Special outputs run a UI action once, on activation; normal ones press a button.
    if      (chords[best].output == CHORD_OUT_BRIGHT) uiBrightnessChord(now);
    else if (chords[best].output == CHORD_OUT_VOLUME) uiOpenVolume(now);
    else                                              gamepad.pressButton(chords[best].output);
    activeChord = best; chordOwnedMask |= chords[best].members;
    pendingMask &= ~chords[best].members;   // members joined a chord -> no deferred solo tap
  }

  // Emit solo presses. A chord member waits out the window before firing solo (so a
  // simultaneous combo registers as the chord, not its parts); meanwhile its bit sits
  // in pendingMask so a release inside the window can recover it as a quick tap below.
  for (uint8_t i = 0; i < NUM_HID; i++) {
    uint32_t bit = (1u << i);
    if (!(physMask & bit) || (chordOwnedMask & bit) || (emittedMask & bit)) continue;
    bool isMember = (chordMemberMask & bit);
    if (isMember && activeChord < 0 && (now - downTime[i]) < settings.chordWindowMs) { pendingMask |= bit; continue; }
    gamepad.pressButton(i); emittedMask |= bit; emitTime[i] = now; pendingMask &= ~bit;
  }

  for (uint8_t i = 0; i < NUM_HID; i++) {
    uint32_t bit = (1u << i);
    if (physMask & bit) continue;
    if (emittedMask & bit) {
      // Hold every solo press at least MIN_PRESS_MS so fast taps aren't missed.
      if ((now - emitTime[i]) >= MIN_PRESS_MS) { gamepad.releaseButton(i); emittedMask &= ~bit; }
    } else if (pendingMask & bit) {
      // Released inside the chord window without forming a chord: fire the tap now.
      gamepad.pressButton(i); emittedMask |= bit; emitTime[i] = now;
    }
    if (!(emittedMask & bit)) { pendingMask &= ~bit; chordOwnedMask &= ~bit; }
  }

  // The toggle member bit isn't covered by the 0..NUM_HID-1 loop above; release its
  // ownership once the menu button is up (this is what un-suppresses the menu action).
  if (!(physMask & (1u << CHORD_MEMBER_TOGGLE))) chordOwnedMask &= ~(1u << CHORD_MEMBER_TOGGLE);
}
