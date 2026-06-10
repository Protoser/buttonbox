// Button chords: pressing 2+ physical buttons together emits one virtual button
// (output 14..31) instead of the members. Engine runs on the HOME page.
#pragma once
#include <Arduino.h>
#include "config.h"

struct Chord {
  uint32_t members;   // bitmask over physical HID indices (bits 0..NUM_HID-1)
  uint8_t  output;    // HID index 14..31 emitted while the chord is held
};

static constexpr uint8_t MAX_CHORDS = 18;   // outputs 14..31

extern Chord    chords[MAX_CHORDS];
extern uint8_t  chordCount;
extern uint32_t chordMemberMask;             // OR of all members (for the hold-window)

void    chordsLoad();
void    chordsSave();
void    recomputeChordMask();
uint8_t firstFreeOutput();

void    updateChords(uint32_t now);          // HID engine, call on HOME
void    resetChordEngine();                  // release everything, call when leaving HOME
int8_t  activeChordOutput();                 // HID index of active chord output, or -1
