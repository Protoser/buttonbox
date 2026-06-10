// Manual stopwatch with a lap ring buffer. Runs in the background off millis().
#pragma once
#include <Arduino.h>

void     swToggle(uint32_t now);          // start <-> stop
void     swReset();                       // zero everything, clear laps
bool     swRecordLap(uint32_t now);       // record a split; true if running (recorded)
void     swUndoLastLap();                 // remove the most recent split

uint32_t swElapsed(uint32_t now);
bool     swIsRunning();
uint16_t swLapCount();                    // total laps recorded
uint8_t  swLapsAvailable();               // how many are retained (ring-capped)
uint32_t swSplitByNumber(uint16_t lapNo); // split for 1-based lap number
uint32_t swLastSplit();
