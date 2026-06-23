#include "mcdu.h"
#include "settings.h"
#include "hostlink.h"
#include <ctype.h>

McduState mcdu = {0, {{0}}, 0, 0};

// Short labels for the on-device remap editor; index == MCDU output index. Order
// MUST match host/mcdu.py MCDU_OUTPUTS (the companion maps the same indices).
static const char *OUT_LABELS[MCDU_OUTPUT_COUNT] = {
  "Off", "Scrl Up", "Scrl Dn",
  "LSK L1", "LSK L2", "LSK L3", "LSK L4", "LSK L5", "LSK L6",
  "LSK R1", "LSK R2", "LSK R3", "LSK R4", "LSK R5", "LSK R6",
  "MENU", "CLR", "OVFY", "DIR", "PROG", "FPLN", "INIT", "PERF", "DATA", "AIRPORT",
};

const char *mcduOutputLabel(uint8_t outIdx) {
  return outIdx < MCDU_OUTPUT_COUNT ? OUT_LABELS[outIdx] : "?";
}

void mcduHandleButton(uint8_t hidIndex) {
  if (hidIndex >= MCDU_MAP_N) return;
  uint8_t out = settings.mcduMap[hidIndex];
  if (out == MCDU_OUT_NONE) return;
  if (out == MCDU_OUT_SCRU || out == MCDU_OUT_SCRD) {       // scroll the view on the box
    int off = mcdu.scrollOff + (out == MCDU_OUT_SCRD ? 1 : -1);
    int maxOff = MCDU_BODY - MCDU_BODY_VIS;
    mcdu.scrollOff = (int8_t)(off < 0 ? 0 : off > maxOff ? maxOff : off);
    return;
  }
  char buf[16]; snprintf(buf, sizeof(buf), "mcdukey %u\n", out); hostlinkSend(buf);  // -> companion -> SimBridge
}

static const uint32_t STALE_MS = 3000;   // no line for this long -> "Waiting for MCDU..."

void mcduApply(char *line, uint32_t now) {
  char *colon = strchr(line, ':');
  if (!colon) return;
  *colon = 0;
  const char *key = line;
  const char *val = colon + 1;

  if (!strcmp(key, "act")) {
    mcdu.active = (uint8_t)(atoi(val) != 0);
  } else if (!strcmp(key, "scroll")) {
    int off = (int)mcdu.scrollOff + atoi(val);          // val is "+1" / "-1"
    int maxOff = MCDU_BODY - MCDU_BODY_VIS;              // last window start
    if (off < 0) off = 0;
    if (off > maxOff) off = maxOff;
    mcdu.scrollOff = (int8_t)off;
  } else if (isdigit((unsigned char)key[0])) {
    int row = atoi(key);
    if (row >= 0 && row < MCDU_ROWS) {
      strncpy(mcdu.rows[row], val, MCDU_COLS);
      mcdu.rows[row][MCDU_COLS] = 0;
    }
  } else {
    return;                                              // unknown key: ignore, don't refresh
  }
  mcdu.lastRx = now ? now : 1;                            // never store 0 (== "never")
}

bool mcduFresh(uint32_t now) {
  return mcdu.lastRx != 0 && (now - mcdu.lastRx) < STALE_MS;
}
