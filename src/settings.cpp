#include "settings.h"
#include <Preferences.h>

Settings settings;

static const uint16_t IDLE_OPTIONS[] = {0, 30, 120};
static const uint8_t  IDLE_N         = 3;
static const uint16_t CW_OPTIONS[]   = {30, 40, 60, 80};
static const uint8_t  CW_N           = 4;
static const uint8_t  BRF_OPTIONS[]  = {25, 50, 75, 100};  // normal brightness %
static const uint8_t  BRF_N          = 4;
static const uint8_t  BRI_OPTIONS[]  = {0, 10, 20, 40};    // idle brightness %
static const uint8_t  BRI_N          = 4;
static const uint16_t DIM_OPTIONS[]  = {0, 10, 30, 60};    // auto-dim delay s (0 = off)
static const uint8_t  DIM_N          = 4;

void loadSettings() {
  Preferences p;
  p.begin("bbox", true);
  settings.flipped       = p.getBool("flip", false);
  settings.labelsGpio    = p.getBool("gpiolbl", false);
  settings.idleBlankSec  = p.getUShort("idle", 0);
  settings.chordWindowMs = p.getUShort("chordwin", 40);
  settings.bootSel = p.getUChar("boot", 1);
  { uint8_t def[5] = {0,1,2,3,4};
    if (p.getBytes("pcord", settings.pcStatOrder, 5) != 5) memcpy(settings.pcStatOrder, def, 5); }
  { uint8_t adef[APP_ORDER_MAX] = {0,1,2,3,4,5,6,7,8};
    if (p.getBytes("aord", settings.appOrder, APP_ORDER_MAX) != APP_ORDER_MAX)
      memcpy(settings.appOrder, adef, APP_ORDER_MAX); }
  settings.appHidden = p.getUShort("ahid", 0);
  settings.wifiMode = p.getUChar("wmode", 2);
  { uint8_t mdef[MCDU_MAP_N] = {3, 9, 4, 10, 5, 11, 6, 12, 7, 13, 1, 2, 17, 16};
    if (p.getBytes("mcdumap", settings.mcduMap, MCDU_MAP_N) != MCDU_MAP_N)
      memcpy(settings.mcduMap, mdef, MCDU_MAP_N); }
  { uint8_t kdef[KEYMAP_N] = {0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71};
    if (p.getBytes("keykey", settings.keyKey, KEYMAP_N) != KEYMAP_N)
      memcpy(settings.keyKey, kdef, KEYMAP_N); }
  { uint8_t mdef[KEYMAP_N] = {0};
    if (p.getBytes("keymod", settings.keyMod, KEYMAP_N) != KEYMAP_N)
      memcpy(settings.keyMod, mdef, KEYMAP_N); }
  settings.flightUnits = p.getUChar("funits", 0);
  settings.engStyle    = p.getUChar("engsty", 0);
  settings.timerSec    = p.getUShort("timersec", 300);
  settings.brightFull  = p.getUChar("brfull", 100);
  settings.brightIdle  = p.getUChar("bridle", 20);
  settings.dimIdleSec  = p.getUShort("dimidle", 15);
  p.end();
}

void saveSettings() {
  Preferences p;
  p.begin("bbox", false);
  p.putBool("flip", settings.flipped);
  p.putBool("gpiolbl", settings.labelsGpio);
  p.putUShort("idle", settings.idleBlankSec);
  p.putUShort("chordwin", settings.chordWindowMs);
  p.putUChar("boot", settings.bootSel);
  p.putBytes("pcord", settings.pcStatOrder, 5);
  p.putBytes("aord", settings.appOrder, APP_ORDER_MAX);
  p.putUShort("ahid", settings.appHidden);
  p.putUChar("wmode", settings.wifiMode);
  p.putBytes("mcdumap", settings.mcduMap, MCDU_MAP_N);
  p.putBytes("keykey", settings.keyKey, KEYMAP_N);
  p.putBytes("keymod", settings.keyMod, KEYMAP_N);
  p.putUChar("funits", settings.flightUnits);
  p.putUChar("engsty", settings.engStyle);
  p.putUShort("timersec", settings.timerSec);
  p.putUChar("brfull", settings.brightFull);
  p.putUChar("bridle", settings.brightIdle);
  p.putUShort("dimidle", settings.dimIdleSec);
  p.end();
}

void settingsToggleFlip()   { settings.flipped = !settings.flipped; saveSettings(); }
void settingsToggleLabels() { settings.labelsGpio = !settings.labelsGpio; saveSettings(); }

void settingsCycleIdle() {
  uint8_t k = 0;
  for (uint8_t n = 0; n < IDLE_N; n++) if (IDLE_OPTIONS[n] == settings.idleBlankSec) k = n;
  settings.idleBlankSec = IDLE_OPTIONS[(k + 1) % IDLE_N];
  saveSettings();
}

void settingsCycleChordWin() {
  uint8_t k = 0;
  for (uint8_t n = 0; n < CW_N; n++) if (CW_OPTIONS[n] == settings.chordWindowMs) k = n;
  settings.chordWindowMs = CW_OPTIONS[(k + 1) % CW_N];
  saveSettings();
}

void settingsCycleBrightFull() {
  uint8_t k = 0;
  for (uint8_t n = 0; n < BRF_N; n++) if (BRF_OPTIONS[n] == settings.brightFull) k = n;
  settings.brightFull = BRF_OPTIONS[(k + 1) % BRF_N];
  saveSettings();
}

void settingsCycleBrightIdle() {
  uint8_t k = 0;
  for (uint8_t n = 0; n < BRI_N; n++) if (BRI_OPTIONS[n] == settings.brightIdle) k = n;
  settings.brightIdle = BRI_OPTIONS[(k + 1) % BRI_N];
  saveSettings();
}

void settingsCycleDimIdle() {
  uint8_t k = 0;
  for (uint8_t n = 0; n < DIM_N; n++) if (DIM_OPTIONS[n] == settings.dimIdleSec) k = n;
  settings.dimIdleSec = DIM_OPTIONS[(k + 1) % DIM_N];
  saveSettings();
}
