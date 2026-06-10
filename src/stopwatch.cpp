#include "stopwatch.h"

static bool     running        = false;
static uint32_t startMs        = 0;   // millis at start of current run segment
static uint32_t accumMs        = 0;   // accumulated ms while paused
static uint32_t lastLapElapsed = 0;
static uint32_t lastSplit      = 0;
static uint16_t lapCount       = 0;

static const uint8_t MAX_LAPS  = 30;  // most recent laps kept (ring)
static uint32_t laps[MAX_LAPS] = {0};

// Saved baseline so the most recent lap can be undone (hold-to-cancel).
static uint32_t prevLapElapsed = 0;
static uint32_t prevSplit      = 0;

uint32_t swElapsed(uint32_t now) { return accumMs + (running ? (now - startMs) : 0); }
bool     swIsRunning()           { return running; }
uint16_t swLapCount()            { return lapCount; }
uint8_t  swLapsAvailable()       { return (lapCount < MAX_LAPS) ? (uint8_t)lapCount : MAX_LAPS; }
uint32_t swLastSplit()           { return lastSplit; }

void swToggle(uint32_t now) {
  if (running) { accumMs += now - startMs; running = false; }
  else         { startMs = now; running = true; }
}

void swReset() {
  running = false; accumMs = 0; startMs = 0;
  lastLapElapsed = 0; lastSplit = 0; lapCount = 0;
}

bool swRecordLap(uint32_t now) {
  if (!running) return false;
  uint32_t e = swElapsed(now);
  prevLapElapsed = lastLapElapsed;
  prevSplit      = lastSplit;
  lastSplit = e - lastLapElapsed;
  lastLapElapsed = e;
  laps[lapCount % MAX_LAPS] = lastSplit;
  lapCount++;
  return true;
}

void swUndoLastLap() {
  if (lapCount == 0) return;
  lapCount--;
  lastLapElapsed = prevLapElapsed;
  lastSplit      = prevSplit;
}

uint32_t swSplitByNumber(uint16_t lapNo) {
  if (lapNo < 1 || lapNo > lapCount) return 0;
  return laps[(lapNo - 1) % MAX_LAPS];
}
