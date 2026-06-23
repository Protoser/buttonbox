#include "flight.h"

FlightState flight = {};   // all fields zero until the first telemetry line

static const uint32_t STALE_MS = 3000;   // no line for this long -> "Waiting for MSFS..."

static void applyToken(const char *key, long val) {
  if      (!strcmp(key, "act"))    flight.active     = (uint8_t)(val != 0);
  else if (!strcmp(key, "ias"))    flight.ias        = (uint16_t)constrain(val, 0, 2000);
  else if (!strcmp(key, "alt"))    flight.alt        = (int32_t)constrain(val, -2000, 120000);
  else if (!strcmp(key, "hdg"))    flight.hdg        = (uint16_t)(((val % 360) + 360) % 360);
  else if (!strcmp(key, "vs"))     flight.vs         = (int16_t)constrain(val, -30000, 30000);
  else if (!strcmp(key, "pit"))    flight.pitch      = (int8_t)constrain(val, -90, 90);
  else if (!strcmp(key, "bnk"))    flight.bank       = (int8_t)constrain(val, -90, 90);
  else if (!strcmp(key, "apm"))    flight.apMaster   = (uint8_t)(val != 0);
  else if (!strcmp(key, "aphdg"))  flight.apHdgSel   = (uint16_t)(((val % 360) + 360) % 360);
  else if (!strcmp(key, "apalt"))  flight.apAltSel   = (int32_t)constrain(val, -2000, 120000);
  else if (!strcmp(key, "apmode")) flight.apModes    = (uint8_t)val;
  else if (!strcmp(key, "gear"))   flight.gearPct    = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "flaps"))  flight.flapsPct   = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "pbrk"))   flight.parkBrake  = (uint8_t)(val != 0);
  else if (!strcmp(key, "splr"))   flight.spoilers   = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "thr"))    flight.throttle   = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "eng"))    flight.engPrimary = (uint16_t)constrain(val, 0, 30000);
  else if (!strcmp(key, "eng2"))   flight.eng2       = (uint16_t)constrain(val, 0, 30000);
  else if (!strcmp(key, "eng3"))   flight.eng3       = (uint16_t)constrain(val, 0, 30000);
  else if (!strcmp(key, "eng4"))   flight.eng4       = (uint16_t)constrain(val, 0, 30000);
  else if (!strcmp(key, "neng"))   flight.nEng       = (uint8_t)constrain(val, 1, 4);
  else if (!strcmp(key, "fuel"))   flight.fuelPct    = (uint8_t)constrain(val, 0, 100);
  else if (!strcmp(key, "etyp"))   flight.engType    = (uint8_t)constrain(val, 0, 2);
}

void flightApply(char *line, uint32_t now) {
  bool any = false;
  for (char *tok = strtok(line, " \t"); tok; tok = strtok(nullptr, " \t")) {
    char *colon = strchr(tok, ':');
    if (colon) { *colon = 0; applyToken(tok, atol(colon + 1)); any = true; }
  }
  if (any) flight.lastRx = now ? now : 1;   // never store 0 (== "never")
}

bool flightFresh(uint32_t now) {
  return flight.lastRx != 0 && (now - flight.lastRx) < STALE_MS;
}
