// Persistent user settings (NVS flash).
#pragma once
#include <Arduino.h>

static const uint8_t APP_ORDER_MAX = 8;   // launcher-order capacity (>= number of apps in ui.cpp APPS)

struct Settings {
  bool     flipped       = false;  // when true: no screen rotation (panel in non-default position)
  bool     labelsGpio    = false;  // grid labels: false = HID number, true = GPIO
  uint16_t idleBlankSec  = 0;      // blank screen after N s idle (0 = never)
  uint16_t chordWindowMs = 40;     // hold window before a chord member fires solo
  uint8_t  bootSel        = 1;      // power-on screen: 0 = launcher, else app index+1 (see ui.cpp APPS)
  uint8_t  pcStatOrder[5] = {0, 1, 2, 3, 4};  // ordered stat indices 0-7 shown on dash; 0xFF = empty slot
  uint8_t  appOrder[APP_ORDER_MAX] = {0, 1, 2, 3, 4, 5, 6, 7};  // launcher order; values index ui.cpp APPS
  uint8_t  wifiMode       = 2;               // 0=off, 1=always on, 2=auto (via companion when connected)
};
extern Settings settings;

void loadSettings();
void saveSettings();

// Mutating helpers (each cycles/toggles the value and persists it).
void settingsToggleFlip();
void settingsToggleLabels();
void settingsCycleIdle();
void settingsCycleChordWin();
