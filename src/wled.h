// WLED LED-controller integration — JSON HTTP on a background FreeRTOS task.
// Mirrors the Shelly module: it does NOT manage WiFi itself, it piggybacks on the
// connection the Shelly task brings up/tears down based on settings.wifiMode, and
// polls/controls its WLED device whenever WiFi is associated. In AUTO mode with a
// companion connected, control is routed over serial instead (the PC does the HTTP).
#pragma once
#include <Arduino.h>

struct WledState {
  bool     on     = false;   // master on/off
  uint8_t  bri     = 0;      // brightness 0..255
  int16_t  preset  = -1;     // current preset id, -1 = none
  uint32_t lastRx  = 0;      // millis of last successful read (any source); 0 = never
};

extern WledState wledState;

void wledBegin();                      // load config, create mutex + background task
void wledLoadConfig();
void wledSaveConfig();
void wledSetIp(const char *ip);        // copy + persist the WLED device IP
const char *wledIp();

// Control actions — routed via serial when companionMode (AUTO + companion), else
// queued for the background task to perform directly over WiFi.
void wledPowerOn();
void wledPowerOff();
void wledSetBrightness(uint8_t bri);   // absolute 0..255 — sent once on release of a hold-adjust
void wledPresetNext();
void wledPresetPrev();

void wledApplyFromCompanion(char *line);   // parse "on:1 bri:128 ps:2" pushed by the PC
bool wledFresh(uint32_t now);              // true if last read was < 6 s ago
