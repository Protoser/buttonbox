// Windows volume-mixer state + control. The companion (volume_audio.py) streams the
// master volume and per-application volumes; the box shows them on PAGE_VOLUME and
// sends back changes as `volset` lines. The box has no audio of its own — this is a
// remote control surface for the PC's audio, like the Music/PC pages.
#pragma once
#include <Arduino.h>

static const uint8_t VOL_MAX_APPS = 6;    // apps shown at once (screen fits master + a few)
static const uint8_t VOL_NAME_LEN = 13;   // per-app name buffer (incl. terminator)

// Inbound from the companion (parsed in hostlink):
//   vol <master>:<count>        -> volumeApplyHeader
//   vapp <idx>:<vol>:<name>     -> volumeApplyApp   (one per app)
void volumeApplyHeader(const char *args, uint32_t now);
void volumeApplyApp(const char *args, uint32_t now);

// Read side for the page.
uint8_t     volumeMaster();
uint8_t     volumeCount();                 // number of app rows
const char *volumeName(uint8_t i);         // app i name (0-based)
uint8_t     volumeVol(uint8_t i);          // app i volume 0..100
bool        volumeFresh(uint32_t now);     // companion data seen recently

// Control side (device -> companion), with an optimistic local echo so the bar moves
// immediately without waiting for the next stream.
void volumeSetMaster(uint8_t v);
void volumeSetApp(uint8_t i, uint8_t v);
void volumeRequest();                      // ask the companion to push state now
