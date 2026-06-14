// Now-playing media received over the USB CDC serial link.
//
// The companion app reads Windows' current media session (the same source the
// keyboard media keys control: Spotify, browser/YouTube, etc.) and streams one
// line per update:
//   music <state> <title...>
// where state is 0 = nothing playing, 1 = playing, 2 = paused, and the rest of
// the line (after the single space) is the already-ASCII-sanitized title text.
//
// Control goes the other way: the Music page emits "mctl prev|playpause|next"
// lines, which the companion turns into media-session commands on the PC.
#pragma once
#include <Arduino.h>

struct MusicState {
  uint8_t  playState;        // 0 none, 1 playing, 2 paused
  char     title[64];
  uint32_t lastRx;           // millis() of last line (0 = never)
};
extern MusicState music;

void musicApply(char *line, uint32_t now);   // parse "<state> <title>" (modifies line)
bool musicFresh(uint32_t now);               // true if a line arrived recently
void musicSendCmd(const char *cmd);          // emit "mctl <cmd>" to the companion
