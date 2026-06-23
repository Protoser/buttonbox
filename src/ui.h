// Screen pages, navigation, and rendering.
#pragma once
#include <Arduino.h>

enum Page : uint8_t {
  PAGE_LAUNCHER, PAGE_BUTTONS, PAGE_MENU, PAGE_SETTINGS, PAGE_BTNTEST, PAGE_TIMER,
  PAGE_LAPLIST, PAGE_CHORDS, PAGE_CHORD_CAPTURE, PAGE_CHORD_OUTPUT, PAGE_CHORD_EDIT,
  PAGE_DASH, PAGE_PCSTATS, PAGE_SHELLY, PAGE_MUSIC, PAGE_WLED, PAGE_APPORDER,
  PAGE_BEAMNG, PAGE_FLIGHT, PAGE_MCDU, PAGE_MCDUMAP, PAGE_MCDUMAP_SET
};

void uiBegin();                       // display init + apply saved orientation
Page uiPage();
void uiNoteActivity(uint32_t now);    // wake from idle blank, mark dirty
void uiApplyOrientation();            // re-apply saved rotation (after a remote flip change)
void uiEnterFlash();                  // show "flash mode" + enter the bootloader
void uiHandleMenuButton(uint32_t now); // menu/toggle button: tap=launcher, hold=quick-switch app
void uiHandlePageInput();             // physical-button presses on a non-HOME page
void uiHandleTimerLap(uint32_t now);  // Lap button: tap=record, hold=undo+open list
void uiHandleWledBright(uint32_t now); // WLED Bright focus: hold Up/Down to scroll, send on release
// The display is rendered by a dedicated task on core 0 (started in uiBegin), so the
// slow ST7920 flush never blocks the core-1 input loop. No per-loop display call.

// Launcher app order — shared with the host link (companion mirrors/edits it).
uint8_t uiAppCount();                  // number of launcher apps (APPS[])
uint8_t uiGetAppOrder(uint8_t *out);   // writes the current clean order (>= uiAppCount() bytes), returns count
void    uiSetAppOrder(const uint8_t *order, uint8_t n);  // apply order from companion, normalize + persist
uint16_t uiGetAppHidden();              // bitmask of apps hidden from the launcher
void     uiSetAppHidden(uint16_t mask); // apply hidden mask from companion (Menu forced visible) + persist
