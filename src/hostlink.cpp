#include "hostlink.h"
#include "pcstats.h"
#include "settings.h"
#include "shelly.h"
#include "music.h"
#include "chords.h"
#include "config.h"
#include "ui.h"

static char    lineBuf[96];
static uint8_t lineLen = 0;

static void emitConfig() {
  Serial.printf("cfg flip:%u labels:%u idle:%u chord:%u boot:%u pcorder:",
                settings.flipped, settings.labelsGpio, settings.idleBlankSec,
                settings.chordWindowMs, settings.bootSel);
  for (uint8_t i = 0; i < 5; i++)
    Serial.printf(i == 0 ? "%u" : ",%u", settings.pcStatOrder[i]);
  Serial.printf(" wssid:%s ship:%s shuser:%s wmode:%u nchords:%u\n",
                shellyConfig.wifiSsid, shellyConfig.shellyIp,
                shellyConfig.shellyUser, settings.wifiMode, chordCount);
  for (uint8_t i = 0; i < chordCount; i++)
    Serial.printf("chd %u:%lu:%u\n", i, (unsigned long)chords[i].members, chords[i].output);
}

// args e.g. "flip:0" — values are validated/clamped, then persisted.
static void handleSet(char *args, uint32_t now) {
  char *colon = strchr(args, ':');
  if (!colon) return;
  *colon = 0;
  const char *key = args;
  long v = atol(colon + 1);
  if      (!strcmp(key, "flip"))   { settings.flipped = (v != 0); uiApplyOrientation(); }
  else if (!strcmp(key, "labels")) { settings.labelsGpio = (v != 0); }
  else if (!strcmp(key, "idle"))   { settings.idleBlankSec  = (uint16_t)constrain(v, 0, 3600); }
  else if (!strcmp(key, "chord"))  { settings.chordWindowMs = (uint16_t)constrain(v, 0, 1000); }
  else if (!strcmp(key, "boot"))    { settings.bootSel = (uint8_t)constrain(v, 0, 32); }
  else if (!strcmp(key, "pcorder")) {
    char *p2 = colon + 1;
    for (uint8_t i = 0; i < 5; i++) {
      long idx = strtol(p2, &p2, 10);
      settings.pcStatOrder[i] = (idx >= 0 && idx < 8) ? (uint8_t)idx : 0xFF;
      if (*p2 == ',') p2++;
    }
  }
  else if (!strcmp(key, "wifi_mode")) {
    settings.wifiMode = (uint8_t)constrain(v, 0, 2);
    saveSettings(); shellyRestartWifi(); uiNoteActivity(now); emitConfig(); return;
  }
  // String-valued connectivity settings — save separately and return early
  else if (!strcmp(key, "wifi_ssid"))   {
    strncpy(shellyConfig.wifiSsid,  colon+1, sizeof(shellyConfig.wifiSsid)-1);
    shellySaveConfig(); shellyRestartWifi(); uiNoteActivity(now); emitConfig(); return;
  }
  else if (!strcmp(key, "wifi_pass"))   {
    strncpy(shellyConfig.wifiPass,  colon+1, sizeof(shellyConfig.wifiPass)-1);
    shellySaveConfig(); shellyRestartWifi(); uiNoteActivity(now); emitConfig(); return;
  }
  else if (!strcmp(key, "shelly_ip"))   {
    strncpy(shellyConfig.shellyIp,  colon+1, sizeof(shellyConfig.shellyIp)-1);
    shellySaveConfig(); uiNoteActivity(now); emitConfig(); return;
  }
  else if (!strcmp(key, "shelly_user")) {
    strncpy(shellyConfig.shellyUser, colon+1, sizeof(shellyConfig.shellyUser)-1);
    shellySaveConfig(); uiNoteActivity(now); emitConfig(); return;
  }
  else if (!strcmp(key, "shelly_pass")) {
    strncpy(shellyConfig.shellyPass, colon+1, sizeof(shellyConfig.shellyPass)-1);
    shellySaveConfig(); uiNoteActivity(now); emitConfig(); return;
  }
  else return;
  saveSettings();
  uiNoteActivity(now);
  emitConfig();
}

// args e.g. "add 3:14" (members mask + 0-based output) or "del 1".
static void handleChord(char *args) {
  if (!strncmp(args, "add ", 4)) {
    char *p = args + 4;
    char *colon = strchr(p, ':');
    if (!colon) return;
    *colon = 0;
    uint32_t mask = (uint32_t)strtoul(p, nullptr, 10);
    uint8_t  out  = (uint8_t)atoi(colon + 1);
    if (__builtin_popcount(mask) >= 2 && chordCount < MAX_CHORDS && out >= NUM_HID && out < 32) {
      chords[chordCount].members = mask;
      chords[chordCount].output  = out;
      chordCount++;
      chordsSave(); recomputeChordMask(); resetChordEngine();
    }
  } else if (!strncmp(args, "del ", 4)) {
    int idx = atoi(args + 4);
    if (idx >= 0 && idx < (int)chordCount) {
      for (uint8_t c = idx; c + 1 < chordCount; c++) chords[c] = chords[c + 1];
      chordCount--;
      chordsSave(); recomputeChordMask(); resetChordEngine();
    }
  }
  emitConfig();
}

static void dispatch(char *line, uint32_t now) {
  if      (!strcmp(line, "get"))          emitConfig();
  else if (!strncmp(line, "set ", 4))     handleSet(line + 4, now);
  else if (!strncmp(line, "chord ", 6))   handleChord(line + 6);
  else if (!strcmp(line, "flash"))        uiEnterFlash();
  else if (!strncmp(line, "shelly ", 7))  shellyApplyFromCompanion(line + 7);
  else if (!strncmp(line, "music ", 6))   musicApply(line + 6, now);
  else                                    pcStatsApply(line, now);   // PC telemetry
}

void hostlinkUpdate(uint32_t now) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen) { lineBuf[lineLen] = 0; dispatch(lineBuf, now); lineLen = 0; }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;   // overrun: drop the garbled line
    }
  }
}
