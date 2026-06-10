// Shelly smart-plug integration — WiFi + HTTP polling on a background FreeRTOS task.
// Main loop never blocks; all network I/O runs on core 0.
#pragma once
#include <Arduino.h>

#define WIFI_MODE_OFF  0   // WiFi never started; Shelly data unavailable
#define WIFI_MODE_ON   1   // WiFi always on; ESP32 polls Shelly directly
#define WIFI_MODE_AUTO 2   // companion connected → companion polls via PC; else ESP32 uses WiFi

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
void shellyRestartWifi();              // notify task to re-evaluate WiFi (mode or creds changed)
void shellyBegin();                    // load config, create mutex, start task
void shellyQueueToggle();              // queue a direct WiFi Switch.Toggle
void shellyToggle();                   // toggle: via serial if companion mode, else direct WiFi
bool shellyCompanionMode();            // true when mode=AUTO and companion is actively connected
void shellyApplyFromCompanion(char *); // parse "out:1 apower:.. .." line pushed by companion
bool shellyFresh(uint32_t now);        // true if last poll (any source) was < 6 s ago
bool shellyWifiOk();                   // true if WiFi is associated
