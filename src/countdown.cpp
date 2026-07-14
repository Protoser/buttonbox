#include "countdown.h"

static const uint16_t SET_MIN_S     = 15;       // shortest settable duration
static const uint16_t SET_MAX_S     = 99 * 60;  // longest (display is m:ss)
static const uint32_t RING_MS       = 30000;    // expired "ring" auto-dismisses after this

static uint16_t setSec   = 300;      // configured duration
static uint32_t remainMs = 300000;   // remaining when not running (baseline while running)
static bool     running  = false;
static bool     expired  = false;
static uint32_t startAt  = 0;        // millis() the current run segment started
static uint32_t expiredAt = 0;

uint16_t cdDurationSec() { return setSec; }
bool     cdIsRunning()   { return running; }
bool     cdIsExpired()   { return expired; }

uint32_t cdRemaining(uint32_t now) {
  if (!running) return remainMs;
  uint32_t e = now - startAt;
  return (e >= remainMs) ? 0 : remainMs - e;
}

void cdSetDuration(uint16_t sec) {
  setSec = constrain(sec, SET_MIN_S, SET_MAX_S);
  if (!running && !expired) remainMs = (uint32_t)setSec * 1000;
}

void cdAdjust(uint32_t now, int8_t dir) {
  if (expired) return;
  // Untouched (remaining == set duration): change the set duration itself.
  if (!running && remainMs == (uint32_t)setSec * 1000) {
    int32_t s = setSec;
    int32_t step = ((dir > 0) ? (s < 60) : (s <= 60)) ? 15 : 60;
    setSec   = (uint16_t)constrain(s + dir * step, (int32_t)SET_MIN_S, (int32_t)SET_MAX_S);
    remainMs = (uint32_t)setSec * 1000;
    return;
  }
  // Mid-run (or paused): nudge the remaining time by a minute.
  int32_t r = (int32_t)cdRemaining(now) + dir * 60000;
  remainMs = (r < 0) ? 0 : (uint32_t)r;
  if (running) startAt = now;
}

void cdToggle(uint32_t now) {
  if (expired)      { cdReset(); return; }
  if (running)      { remainMs = cdRemaining(now); running = false; }
  else if (remainMs > 0) { startAt = now; running = true; }
}

void cdReset() {
  running = false; expired = false;
  remainMs = (uint32_t)setSec * 1000;
}

bool cdService(uint32_t now) {
  if (running && cdRemaining(now) == 0) {
    running = false; expired = true; expiredAt = now;
    return true;
  }
  if (expired && (now - expiredAt) >= RING_MS) cdReset();
  return false;
}
