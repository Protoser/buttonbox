// FlyByWire MCDU mirror received over the USB CDC serial link.
//
// The companion app connects to FlyByWire SimBridge's remote-MCDU WebSocket
// (ws://localhost:8380/interfaces/v1/mcdu), flattens the 24x14 colour MCDU screen
// to plain ASCII, and streams it here, one line per update, prefixed so hostlink
// can route it:
//   mcdu act:1                 -- 1 = SimBridge connected (heartbeat ~1 Hz), 0 = not
//   mcdu <row>:<text>          -- row 0..13 (0 = title, 13 = scratchpad), <= 24 chars
//   mcdu scroll:+1 / scroll:-1 -- nudge the on-box view offset (a box button mapped
//                                 to scroll round-trips through the companion)
//
// Input goes the other way: while the MCDU page is open the box repurposes every
// physical button via settings.mcduMap (see the on-device editor / companion). Scroll
// and "off" are handled on the box; a mapped MCDU key is emitted as "mcdukey <idx>"
// (an index into MCDU_OUTPUTS) which the companion turns into event:left:<NAME>.
// Unknown keys/rows are ignored so the protocol can grow without firmware changes.
#pragma once
#include <Arduino.h>

static const uint8_t MCDU_ROWS     = 14;   // 0 = title, 1..12 = body, 13 = scratchpad
static const uint8_t MCDU_COLS     = 24;   // MCDU is 24 chars wide
static const uint8_t MCDU_BODY     = 12;   // body rows (1..12)
static const uint8_t MCDU_BODY_VIS = 6;    // body rows visible at once (rest scroll)

struct McduState {
  uint8_t  active;                       // 1 = SimBridge connected
  char     rows[MCDU_ROWS][MCDU_COLS + 1];
  int8_t   scrollOff;                    // first body row shown in the scroll window
  uint32_t lastRx;                       // millis() of last line (0 = never)
};
extern McduState mcdu;

// MCDU outputs a physical button can be remapped to (settings.mcduMap holds an
// index into this list per button). 0..2 are handled on the box; 3+ are MCDU keys
// emitted to the companion as "mcdukey <idx>". Order MUST match host/mcdu.py.
static const uint8_t MCDU_OUTPUT_COUNT = 25;
static const uint8_t MCDU_OUT_NONE = 0;     // do nothing
static const uint8_t MCDU_OUT_SCRU = 1;     // scroll view up   (box-local)
static const uint8_t MCDU_OUT_SCRD = 2;     // scroll view down (box-local)

void mcduApply(char *line, uint32_t now);   // parse one line (modifies line)
bool mcduFresh(uint32_t now);               // true if a line arrived recently
const char *mcduOutputLabel(uint8_t outIdx); // short label for the on-device editor
void mcduHandleButton(uint8_t hidIndex);     // apply settings.mcduMap: scroll or emit key
