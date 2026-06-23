#include "hostlink.h"
#include <stdarg.h>
#include "pcstats.h"
#include "settings.h"
#include "shelly.h"
#include "music.h"
#include "wled.h"
#include "beamng.h"
#include "flight.h"
#include "mcdu.h"
#include "chords.h"
#include "config.h"
#include "clock.h"
#include "ui.h"

static char     lineBuf[256];   // must hold the longest line; the flight telemetry line is ~210 chars
static uint16_t lineLen = 0;

// Send one line without ever blocking the input loop. USBCDC::write() spins forever
// when the host has the port open but isn't reading; we instead write only what the
// FIFO can take and give up after a short no-progress deadline (dropping the rest).
void hostlinkSend(const char *s) {
  size_t n = strlen(s), off = 0;
  uint32_t start = millis();
  while (off < n) {
    int sp = Serial.availableForWrite();
    if (sp > 0) {
      size_t chunk = ((size_t)sp < n - off) ? (size_t)sp : (n - off);
      Serial.write((const uint8_t *)s + off, chunk);   // chunk <= space, so write() can't spin
      off += chunk;
      start = millis();                                 // made progress -> extend the deadline
    } else if (millis() - start > 30) {
      break;                                            // host not draining: drop the rest
    } else {
      delay(1);                                         // brief, bounded yield while waiting for room
    }
  }
}

// Append printf-style to buf at offset n (cap = sizeof buf), never overflowing.
static int cfgAppend(char *buf, int n, int cap, const char *fmt, ...) {
  if (n >= cap - 1) return cap - 1;
  va_list ap; va_start(ap, fmt);
  int w = vsnprintf(buf + n, cap - n, fmt, ap);
  va_end(ap);
  if (w < 0) return n;
  n += w;
  return n > cap - 1 ? cap - 1 : n;
}

static void emitConfig() {
  char buf[384]; int n = 0;
  n = cfgAppend(buf, n, sizeof(buf), "cfg flip:%u labels:%u idle:%u chord:%u boot:%u pcorder:",
                settings.flipped, settings.labelsGpio, settings.idleBlankSec,
                settings.chordWindowMs, settings.bootSel);
  for (uint8_t i = 0; i < 5; i++)
    n = cfgAppend(buf, n, sizeof(buf), i == 0 ? "%u" : ",%u", settings.pcStatOrder[i]);
  n = cfgAppend(buf, n, sizeof(buf), " apporder:");
  uint8_t ord[APP_ORDER_MAX]; uint8_t an = uiGetAppOrder(ord);
  for (uint8_t i = 0; i < an; i++)
    n = cfgAppend(buf, n, sizeof(buf), i == 0 ? "%u" : ",%u", ord[i]);
  n = cfgAppend(buf, n, sizeof(buf), " apphidden:%u mcdumap:", uiGetAppHidden());
  for (uint8_t i = 0; i < MCDU_MAP_N; i++)
    n = cfgAppend(buf, n, sizeof(buf), i == 0 ? "%u" : ",%u", settings.mcduMap[i]);
  n = cfgAppend(buf, n, sizeof(buf), " funits:%u engsty:%u", settings.flightUnits, settings.engStyle);
  n = cfgAppend(buf, n, sizeof(buf), " wssid:%s ship:%s shuser:%s wmode:%u wledip:%s nchords:%u\n",
                shellyConfig.wifiSsid, shellyConfig.shellyIp,
                shellyConfig.shellyUser, settings.wifiMode, wledIp(), chordCount);
  hostlinkSend(buf);
  for (uint8_t i = 0; i < chordCount; i++) {
    char cb[40];
    snprintf(cb, sizeof(cb), "chd %u:%lu:%u\n", i, (unsigned long)chords[i].members, chords[i].output);
    hostlinkSend(cb);
  }
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
  else if (!strcmp(key, "funits"))  { settings.flightUnits = (uint8_t)constrain(v, 0, 3); }
  else if (!strcmp(key, "engsty"))  { settings.engStyle    = (uint8_t)(v != 0); }
  else if (!strcmp(key, "mcdumap")) {
    char *p2 = colon + 1;
    for (uint8_t i = 0; i < MCDU_MAP_N; i++) {
      char *end; long idx = strtol(p2, &end, 10);
      if (end == p2) break;
      if (idx >= 0 && idx < MCDU_OUTPUT_COUNT) settings.mcduMap[i] = (uint8_t)idx;
      p2 = end; if (*p2 == ',') p2++; else break;
    }
  }
  else if (!strcmp(key, "pcorder")) {
    char *p2 = colon + 1;
    for (uint8_t i = 0; i < 5; i++) {
      long idx = strtol(p2, &p2, 10);
      settings.pcStatOrder[i] = (idx >= 0 && idx < 8) ? (uint8_t)idx : 0xFF;
      if (*p2 == ',') p2++;
    }
  }
  else if (!strcmp(key, "apporder")) {
    char *p2 = colon + 1;
    uint8_t ord[APP_ORDER_MAX]; uint8_t n = 0;
    while (n < APP_ORDER_MAX) {
      char *end; long idx = strtol(p2, &end, 10);
      if (end == p2) break;                     // no more numbers
      ord[n++] = (uint8_t)idx; p2 = end;
      if (*p2 == ',') p2++; else break;
    }
    uiSetAppOrder(ord, n);                       // normalizes + persists
    uiNoteActivity(now); emitConfig(); return;
  }
  else if (!strcmp(key, "apphidden")) {
    uiSetAppHidden((uint16_t)v);                  // forces Menu visible + persists
    uiNoteActivity(now); emitConfig(); return;
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
  else if (!strcmp(key, "wled_ip")) {
    wledSetIp(colon+1); uiNoteActivity(now); emitConfig(); return;
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
  else if (!strncmp(line, "wled ", 5))    wledApplyFromCompanion(line + 5);
  else if (!strncmp(line, "music ", 6))   musicApply(line + 6, now);
  else if (!strncmp(line, "beamng ", 7))  beamngApply(line + 7, now);
  else if (!strncmp(line, "flight ", 7))  flightApply(line + 7, now);
  else if (!strncmp(line, "mcdu ", 5))    mcduApply(line + 5, now);
  else if (!strncmp(line, "time ", 5))    clockApplyHost(line + 5, now);
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
