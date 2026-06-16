#include "beamng.h"

BeamNGState beamng = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static const uint32_t STALE_MS = 3000;   // no line for this long -> "Waiting for PC..."

static void applyToken(const char *key, long val) {
  if      (!strcmp(key, "act"))    beamng.active    = (uint8_t)(val != 0);
  else if (!strcmp(key, "gear"))   beamng.gear      = (int8_t)constrain(val, -1, 30);
  else if (!strcmp(key, "spd"))    beamng.speed     = (uint16_t)constrain(val, 0, 1000);
  else if (!strcmp(key, "unit"))   beamng.unit      = (uint8_t)(val != 0);
  else if (!strcmp(key, "rpm"))    beamng.rpm       = (uint16_t)constrain(val, 0, 30000);
  else if (!strcmp(key, "fuel"))   beamng.fuel      = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "et"))     beamng.engTemp   = (int8_t)constrain(val, -40, 125);
  else if (!strcmp(key, "ot"))     beamng.oilTemp   = (int8_t)constrain(val, -40, 125);
  else if (!strcmp(key, "tb"))     beamng.turbo     = (uint8_t)constrain(val, 0, 255);
  else if (!strcmp(key, "tf"))     beamng.turboFlag = (uint8_t)(val != 0);
  else if (!strcmp(key, "lights")) beamng.lights    = (uint32_t)val;
}

void beamngApply(char *line, uint32_t now) {
  bool any = false;
  for (char *tok = strtok(line, " \t"); tok; tok = strtok(nullptr, " \t")) {
    char *colon = strchr(tok, ':');
    if (colon) { *colon = 0; applyToken(tok, atol(colon + 1)); any = true; }
  }
  if (any) beamng.lastRx = now ? now : 1;   // never store 0 (== "never")
}

bool beamngFresh(uint32_t now) {
  return beamng.lastRx != 0 && (now - beamng.lastRx) < STALE_MS;
}
