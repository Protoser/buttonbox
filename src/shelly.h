// Shelly smart-plug integration — WiFi + HTTP polling on a background FreeRTOS task.
// Main loop never blocks; all network I/O runs on core 0.
#pragma once
#include <Arduino.h>

struct ShellyState {
  bool     output  = false;
  float    apower  = 0.0f;   // W
  float    voltage = 0.0f;   // V
  float    current = 0.0f;   // A
  float    tempC   = 0.0f;   // device temperature °C
  uint32_t lastRx  = 0;      // millis of last successful poll; 0 = never
};

struct ShellyConfig {
  char wifiSsid[33];
  char wifiPass[64];
  char shellyIp[20];
  char shellyUser[32];
  char shellyPass[64];
};

extern ShellyState  shellyState;
extern ShellyConfig shellyConfig;

void shellyLoadConfig();
void shellySaveConfig();
void shellyRestartWifi();       // reconnect with current credentials
void shellyBegin();             // load config, create mutex, start task
void shellyQueueToggle();       // request a Switch.Toggle on the next task tick
bool shellyFresh(uint32_t now); // true if last poll was < 6 s ago
bool shellyWifiOk();            // true if WiFi is associated
