#include "settings.h"
#include <Preferences.h>

Settings settings;

static const uint16_t IDLE_OPTIONS[] = {0, 30, 120};
static const uint8_t  IDLE_N         = 3;
static const uint16_t CW_OPTIONS[]   = {30, 40, 60, 80};
static const uint8_t  CW_N           = 4;

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
  { uint8_t adef[APP_ORDER_MAX] = {0,1,2,3,4,5,6,7};
    if (p.getBytes("aord", settings.appOrder, APP_ORDER_MAX) != APP_ORDER_MAX)
      memcpy(settings.appOrder, adef, APP_ORDER_MAX); }
  settings.appHidden = p.getUChar("ahid", 0);
  settings.wifiMode = p.getUChar("wmode", 2);
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
  p.putUChar("ahid", settings.appHidden);
  p.putUChar("wmode", settings.wifiMode);
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
