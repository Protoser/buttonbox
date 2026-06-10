#include "pcstats.h"

PcStats pcStats = {0, 0, 0, 0, 0, 0, 0, 0, 0};

static const uint32_t STALE_MS = 3000;   // no line for this long -> "Waiting for PC..."

static void applyToken(const char *key, int val) {
  if      (!strcmp(key, "cpu")) pcStats.cpuLoad  = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "ram")) pcStats.ramUsed  = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "gpu")) pcStats.gpuLoad  = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "ct"))  pcStats.cpuTemp  = (int8_t)constrain(val, -40, 125);
  else if (!strcmp(key, "gt"))  pcStats.gpuTemp  = (int8_t)constrain(val, -40, 125);
  else if (!strcmp(key, "vr"))  pcStats.vramUsed = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "cp"))  pcStats.cpuPower = (uint16_t)constrain(val, 0, 2000);
  else if (!strcmp(key, "gp"))  pcStats.gpuPower = (uint16_t)constrain(val, 0, 2000);
}

void pcStatsApply(char *line, uint32_t now) {
  bool any = false;
  for (char *tok = strtok(line, " \t"); tok; tok = strtok(nullptr, " \t")) {
    char *colon = strchr(tok, ':');
    if (colon) { *colon = 0; applyToken(tok, atoi(colon + 1)); any = true; }
  }
  if (any) pcStats.lastRx = now ? now : 1;   // never store 0 (== "never")
}

bool pcStatsFresh(uint32_t now) {
  return pcStats.lastRx != 0 && (now - pcStats.lastRx) < STALE_MS;
}
