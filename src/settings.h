// Persistent user settings (NVS flash).
#pragma once
#include <Arduino.h>

static const uint8_t APP_ORDER_MAX = 12;  // launcher-order capacity (>= number of apps in ui.cpp APPS)
static const uint8_t MCDU_MAP_N    = 14;  // physical buttons remappable on the MCDU page (== NUM_HID)

// Flight-display unit flags (settings.flightUnits bitfield).
static const uint8_t FU_SPEED_MPH = 0x01;  // airspeed in mph (else kt)
static const uint8_t FU_ALT_M     = 0x02;  // altitude in metres (else ft)

struct Settings {
  bool     flipped       = false;  // when true: no screen rotation (panel in non-default position)
  bool     labelsGpio    = false;  // grid labels: false = HID number, true = GPIO
  uint16_t idleBlankSec  = 0;      // blank screen after N s idle (0 = never)
  uint16_t chordWindowMs = 40;     // hold window before a chord member fires solo
  uint8_t  bootSel        = 1;      // power-on screen: 0 = launcher, else app index+1 (see ui.cpp APPS)
  uint8_t  pcStatOrder[5] = {0, 1, 2, 3, 4};  // ordered stat indices 0-7 shown on dash; 0xFF = empty slot
  uint8_t  appOrder[APP_ORDER_MAX] = {0, 1, 2, 3, 4, 5, 6, 7, 8};  // launcher order; values index ui.cpp APPS
  uint16_t appHidden      = 0;               // bitmask: bit i set = app i hidden from launcher (Menu never hidden)
  uint8_t  wifiMode       = 2;               // 0=off, 1=always on, 2=auto (via companion when connected)
  // MCDU page: physical button (HID index) -> MCDU output index (see mcdu.cpp MCDU_OUTPUTS).
  // Default: 5x2 grid -> LSK L1/R1..L5/R5, nav -> scroll + OVFY/CLR.
  uint8_t  mcduMap[MCDU_MAP_N] = {3, 9, 4, 10, 5, 11, 6, 12, 7, 13, 1, 2, 17, 16};
  uint8_t  flightUnits    = 0;               // FU_* bitfield (speed/altitude units on the Flight display)
  uint8_t  engStyle       = 0;               // Engine view: 0 = dial gauges, 1 = bars
};
extern Settings settings;

void loadSettings();
void saveSettings();

// Mutating helpers (each cycles/toggles the value and persists it).
void settingsToggleFlip();
void settingsToggleLabels();
void settingsCycleIdle();
void settingsCycleChordWin();
