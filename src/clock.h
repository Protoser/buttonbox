// Header wall-clock timekeeping. The box has no RTC, so absolute time arrives from
// either NTP (over the Shelly-managed WiFi, re-synced hourly) or the PC companion
// ("time <epochUTC> <offsetSec>" pushed hourly). Once set, the ESP32 system clock
// keeps ticking on its own, so the UI just reads clockGet() on each redraw and the
// clock survives the companion disconnecting. The local UTC offset (learned from
// the companion) is persisted, so NTP can render local time on its own afterwards.
#pragma once
#include <Arduino.h>

void clockBegin();                                    // load the persisted TZ offset
void clockUpdate(uint32_t now);                       // start NTP once WiFi is up
void clockApplyHost(const char *args, uint32_t now);  // "<epochUTC> <offsetSec>" from companion
bool clockGet(uint8_t &h, uint8_t &m);                // local HH:MM; false until first sync
