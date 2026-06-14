#include "music.h"

MusicState music = {0, {0}, 0};

static const uint32_t STALE_MS = 4000;   // no line for this long -> "Waiting for PC..."

void musicApply(char *line, uint32_t now) {
  // line = "<state> <title...>"  (title may be empty)
  char *sp = strchr(line, ' ');
  music.playState = (uint8_t)constrain(atoi(line), 0, 2);
  if (sp) {
    strncpy(music.title, sp + 1, sizeof(music.title) - 1);
    music.title[sizeof(music.title) - 1] = 0;
  } else {
    music.title[0] = 0;
  }
  music.lastRx = now ? now : 1;            // never store 0 (== "never")
}

bool musicFresh(uint32_t now) {
  return music.lastRx != 0 && (now - music.lastRx) < STALE_MS;
}

void musicSendCmd(const char *cmd) {
  Serial.printf("mctl %s\n", cmd);
}
