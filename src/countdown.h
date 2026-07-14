// Countdown timer. Runs in the background off millis(), like the stopwatch.
// Idle -> adjust the set duration; running/paused -> +/- nudges the remaining
// time instead. On expiry it "rings" (cdIsExpired) until dismissed or ~30 s pass.
#pragma once
#include <Arduino.h>

void     cdSetDuration(uint16_t sec);        // set duration (clamped; no-op mid-run)
uint16_t cdDurationSec();
void     cdAdjust(uint32_t now, int8_t dir); // +/- one step (15 s below a minute, else 1 min)
void     cdToggle(uint32_t now);             // start <-> pause (dismisses an expired ring)
void     cdReset();                          // stop + restore the set duration
uint32_t cdRemaining(uint32_t now);          // ms left (0 once expired)
bool     cdIsRunning();
bool     cdIsExpired();                      // ringing, waiting to be dismissed
bool     cdService(uint32_t now);            // call every loop; true once on expiry
