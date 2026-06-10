// PC telemetry received over the USB CDC serial link.
//
// A host helper streams whitespace-separated "key:value" lines, e.g.
//   cpu:45 ram:62 gpu:30 ct:55 gt:61
// Keys: cpu/ram/gpu = load %, ct/gt = CPU/GPU temp °C. Unknown keys are ignored
// so the protocol can grow without firmware changes.
#pragma once
#include <Arduino.h>

struct PcStats {
  uint8_t  cpuLoad;   // %
  uint8_t  ramUsed;   // %
  uint8_t  gpuLoad;   // %
  int8_t   cpuTemp;   // °C
  int8_t   gpuTemp;   // °C
  uint8_t  vramUsed;  // %
  uint16_t cpuPower;  // W
  uint16_t gpuPower;  // W
  uint32_t lastRx;    // millis() of last complete line (0 = never)
};
extern PcStats pcStats;

void pcStatsApply(char *line, uint32_t now);   // parse one telemetry line (modifies line)
bool pcStatsFresh(uint32_t now);               // true if a line arrived recently
