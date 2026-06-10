// Persistent user settings (NVS flash).
#pragma once
#include <Arduino.h>

struct Settings {
  bool     flipped       = true;   // 180° rotation (panel mounted upside-down)
  bool     labelsGpio    = false;  // grid labels: false = HID number, true = GPIO
  uint16_t idleBlankSec  = 0;      // blank screen after N s idle (0 = never)
  uint16_t chordWindowMs = 40;     // hold window before a chord member fires solo
  uint8_t  bootSel        = 1;      // power-on screen: 0 = launcher, else app index+1 (see ui.cpp APPS)
  uint8_t  pcStatOrder[5] = {0, 1, 2, 3, 4};  // ordered stat indices 0-7 shown on dash; 0xFF = empty slot
};
extern Settings settings;

void loadSettings();
void saveSettings();

// Mutating helpers (each cycles/toggles the value and persists it).
void settingsToggleFlip();
void settingsToggleLabels();
void settingsCycleIdle();
void settingsCycleChordWin();
