#include "music.h"
#include "hostlink.h"

MusicState music = {0, {0}, 0, 0};

static const uint32_t STALE_MS = 4000;   // no line for this long -> "Waiting for PC..."

void musicApply(char *line, uint32_t now) {
  // line = "<state> <title...>"  (title may be empty)
  char *sp = strchr(line, ' ');
  music.playState = (uint8_t)constrain(atoi(line), 0, 2);
  char newTitle[sizeof(music.title)];
  if (sp) {
    strncpy(newTitle, sp + 1, sizeof(newTitle) - 1);
    newTitle[sizeof(newTitle) - 1] = 0;
  } else {
    newTitle[0] = 0;
  }
  // Only re-anchor the scroll when the title actually changes; this runs ~twice a
  // second with the same title, so the strcmp guard keeps the anchor stable.
  if (strcmp(newTitle, music.title) != 0) {
    strcpy(music.title, newTitle);
    music.titleAt = now ? now : 1;         // never store 0 (== "never")
  }
  music.lastRx = now ? now : 1;            // never store 0 (== "never")
}

bool musicFresh(uint32_t now) {
  return music.lastRx != 0 && (now - music.lastRx) < STALE_MS;
}

void musicSendCmd(const char *cmd) {
  char buf[24]; snprintf(buf, sizeof(buf), "mctl %s\n", cmd); hostlinkSend(buf);
}
