// ============================================================================
//  ESP32-S3 buttonbox  —  15 buttons + CR-10 (ST7920) display
//
//  Native USB HID gamepad (32 buttons) + an on-screen menu UI. Modules:
//    config    pin map / counts            buttons   debounced inputs
//    display   the ST7920 panel            hid       USB gamepad
//    settings  NVS-persisted options       stopwatch lap timer
//    chords    2+ button combos -> button  ui        pages / nav / rendering
//  This file just wires them together: init in setup(), tick in loop().
// ============================================================================

#include <Arduino.h>
#include "soc/soc.h"            // REG_CLR_BIT
#include "soc/rtc_cntl_reg.h"   // RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT
#include "settings.h"
#include "buttons.h"
#include "hid.h"
#include "chords.h"
#include "hostlink.h"
#include "shelly.h"
#include "wled.h"
#include "clock.h"
#include "ui.h"

#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0
#error "Set -DARDUINO_USB_MODE=0 in platformio.ini so the box enumerates as a USB gamepad."
#endif

void setup() {
  // Clear the force-download flag so a normal reset never re-enters flash mode.
  REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

  buttonsBegin();
  loadSettings();
  chordsLoad();
  shellyBegin();
  wledBegin();
  clockBegin();
  uiBegin();
  hidBegin();
  Serial.begin(115200);
  uiNoteActivity(millis());
}

void loop() {
  uint32_t now = millis();

  buttonsUpdate(now);
  hostlinkUpdate(now);            // drain serial: PC telemetry + config commands
  clockUpdate(now);               // bring up NTP once WiFi is associated
  if (buttonsAnyEdge())         uiNoteActivity(now);
  uiHandleMenuButton(now);        // tap = launcher/resume, hold = quick-switch app

  // On non-Buttons pages the UI first claims the buttons it needs (the nav
  // buttons, or all buttons on the capture/test pages) by suppressing them.
  if (uiPage() != PAGE_BUTTONS) uiHandlePageInput();
  // The chord/HID engine then runs on EVERY page: any button not claimed by the
  // UI stays a live gamepad button (so the non-nav buttons work everywhere).
  updateChords(now);

  if (uiPage() == PAGE_TIMER)   uiHandleTimerLap(now);
  uiHandleWledBright(now);         // self-gating; handles hold-to-scroll + release flush

  uiTickDisplay(now);
}
