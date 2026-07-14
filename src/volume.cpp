#include "volume.h"
#include "hostlink.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t  masterVol = 0;
static struct { char name[VOL_NAME_LEN]; uint8_t vol; } apps[VOL_MAX_APPS];
static uint8_t  appCount   = 0;
static uint32_t lastUpdate = 0;

uint8_t     volumeMaster()            { return masterVol; }
uint8_t     volumeCount()             { return appCount; }
const char *volumeName(uint8_t i)     { return (i < appCount) ? apps[i].name : ""; }
uint8_t     volumeVol(uint8_t i)      { return (i < appCount) ? apps[i].vol : 0; }
bool        volumeFresh(uint32_t now) { return lastUpdate && (now - lastUpdate) < 3000; }

// "<master>:<count>" — starts a fresh snapshot; the app rows follow on `vapp` lines.
void volumeApplyHeader(const char *args, uint32_t now) {
  char *end;
  long m = strtol(args, &end, 10);
  if (*end != ':') return;
  long c = strtol(end + 1, nullptr, 10);
  masterVol = (uint8_t)constrain(m, 0, 100);
  appCount  = (uint8_t)constrain(c, 0, VOL_MAX_APPS);
  lastUpdate = now;
}

// "<idx>:<vol>:<name>" — fills one app row (name may contain anything but ':').
void volumeApplyApp(const char *args, uint32_t now) {
  char *end;
  long idx = strtol(args, &end, 10);
  if (*end != ':') return;
  long vol = strtol(end + 1, &end, 10);
  if (*end != ':') return;
  const char *name = end + 1;
  if (idx < 0 || idx >= appCount) return;
  apps[idx].vol = (uint8_t)constrain(vol, 0, 100);
  strncpy(apps[idx].name, name, VOL_NAME_LEN - 1);
  apps[idx].name[VOL_NAME_LEN - 1] = 0;
  lastUpdate = now;
}

void volumeSetMaster(uint8_t v) {
  if (v > 100) v = 100;
  masterVol = v;                                  // optimistic echo
  char b[24]; snprintf(b, sizeof(b), "volset master:%u\n", v);
  hostlinkSend(b);
}

void volumeSetApp(uint8_t i, uint8_t v) {
  if (i >= appCount) return;
  if (v > 100) v = 100;
  apps[i].vol = v;                                // optimistic echo
  char b[24]; snprintf(b, sizeof(b), "volset %u:%u\n", i, v);
  hostlinkSend(b);
}

void volumeRequest() { hostlinkSend("volget\n"); }
