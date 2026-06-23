#include "ui.h"
#include "config.h"
#include "display.h"
#include "settings.h"
#include "buttons.h"
#include "chords.h"
#include "stopwatch.h"
#include "pcstats.h"
#include "shelly.h"
#include "music.h"
#include "wled.h"
#include "beamng.h"
#include "flight.h"
#include "mcdu.h"
#include "clock.h"
#include "esp32-hal-tinyusb.h"   // usb_persist_restart()

// ----------------------------------------------------------------------------
//  Page state
// ----------------------------------------------------------------------------
static Page   page = PAGE_LAUNCHER;
static int8_t sel  = 0;

// Remembered app page+selection so the menu button resumes where you left off.
static Page   lastApp    = PAGE_BUTTONS;
static int8_t lastAppSel = 0;

// BeamNG page: which sub-view is shown (cycled with Up/Down). See drawBeamng().
static uint8_t beamngView = 0;
static const uint8_t BEAMNG_VIEWS = 4;   // OVERVIEW / DASH / STATUS / LIGHTS

// Flight page: which sub-view is shown (cycled with Up/Down). See drawFlight().
static uint8_t flightView = 0;
static const uint8_t FLIGHT_VIEWS = 4;   // FLIGHT / A/P / CONFIG / ENGINE

// WLED page: which control SELECT has focused (0=power, 1=brightness, 2=preset).
static uint8_t wledFocus = 0;
// Brightness hold-adjust: while a button is held the value scrolls locally and is
// only sent to the device on release.
static bool     wledBriActive = false;
static int      wledBriValue  = 0;     // pending brightness shown while adjusting
static uint32_t wledBriStepAt = 0;
static const uint8_t  WLED_BRI_STEP      = 8;
static const uint16_t WLED_BRI_REPEAT_MS = 70;

static const char *MENU_ITEMS[]     = {"Button Test", "Chords", "Settings", "App Order", "MCDU Keys", "Flash Mode", "Back"};
static const uint8_t MENU_COUNT     = 7;
static const char *SETTINGS_ITEMS[] = {"Rotate", "Labels", "Idle", "Chord", "Boot", "WiFi", "Back"};
static const uint8_t SETTINGS_COUNT = 7;
static const char *PCSTAT_ITEMS[]   = {"CPU", "RAM", "GPU", "CPU Temp", "GPU Temp",
                                       "VRAM", "CPU Pwr", "GPU Pwr"};
static const uint8_t PCSTAT_NUM      = 8;   // number of selectable stats
static const uint8_t PCSTAT_MAX_ON   = 5;   // up to 5 shown at once (screen fits 5 rows)
static const uint8_t PCSTAT_TOTAL    = PCSTAT_MAX_ON + 1;  // 5 slots + Back row

static int8_t  testLastHid  = -1;
static uint8_t testLastGpio = 0;

// MCDU key map editor: which physical button (HID index) the output picker is editing.
static uint8_t mcduEditBtn  = 0;

// Chord editor scratch
static uint32_t captureMask    = 0;
static uint32_t pendingMembers = 0;
static uint8_t  editChord      = 0;
static uint8_t  outputSel      = NUM_HID;
static bool     editingOutput  = false;

// Lap button long-press
static const uint16_t LAP_HOLD_MS = 500;
static uint32_t lapHoldStart     = 0;
static bool     lapHoldHandled   = false;
static bool     lapRecordedPress = false;

// Menu button: tap = launcher/resume, hold = quick-switch to the previous app.
static const uint16_t MENU_HOLD_MS = 400;
static uint32_t menuHoldStart   = 0;
static bool     menuHoldHandled = false;

// Display state. The panel is rendered from a task on core 0 (see displayTask), so
// the fields it shares with the core-1 input loop are volatile: core 1 only raises
// these flags, the render task consumes them. lastDraw is touched by the task only.
static volatile bool     displayDirty = true;
static uint32_t          lastDraw     = 0;
static volatile uint32_t lastActivity = 0;
static volatile bool     blanked      = false;
static volatile bool     orientDirty  = false;   // request: re-apply saved rotation on the render task
static TaskHandle_t      displayTaskHandle = nullptr;

// Nav-button legend glyphs (right... drawn at the left edge to match the buttons).
enum HintKind : uint8_t { H_NONE, H_UP, H_DOWN, H_LEFT, H_RIGHT, H_PLAY, H_PAUSE, H_TEXT };
struct NavHint { HintKind kind; char label[3]; };
static const NavHint LIST_HINTS[4] = {{H_UP, ""}, {H_DOWN, ""}, {H_RIGHT, ""}, {H_LEFT, ""}};

// ----------------------------------------------------------------------------
//  Forward declarations
// ----------------------------------------------------------------------------
static void applyOrientation();
static void enterBootloader();
static void gotoPage(Page p);
static void confirmCapture();
static void handleNav(uint8_t a);
static void render();
static void displayTask(void *);
static void drawLauncher();

// ----------------------------------------------------------------------------
//  App launcher registry
//  Each app = a name, a tiny icon drawer (u8g2 primitives, see below), and the
//  page it opens. Add an app by adding one row here (+ its icon + page).
// ----------------------------------------------------------------------------
static void iconButtons(int cx, int cy);
static void iconTimer(int cx, int cy);
static void iconMenu(int cx, int cy);
static void iconPc(int cx, int cy);
static void iconShelly(int cx, int cy);
static void iconMusic(int cx, int cy);
static void iconWled(int cx, int cy);
static void iconBeamng(int cx, int cy);
static void iconFlight(int cx, int cy);
static void iconMcdu(int cx, int cy);

struct App { const char *name; void (*drawIcon)(int cx, int cy); Page page; };
// New apps append here so existing persisted app indices (appOrder/appHidden) stay valid.
static const App APPS[] = {
  {"Buttons", iconButtons, PAGE_BUTTONS},
  {"Timer",   iconTimer,   PAGE_TIMER},
  {"PC",      iconPc,      PAGE_DASH},
  {"Shelly",  iconShelly,  PAGE_SHELLY},
  {"Music",   iconMusic,   PAGE_MUSIC},
  {"Menu",    iconMenu,    PAGE_MENU},
  {"WLED",    iconWled,    PAGE_WLED},
  {"BeamNG",  iconBeamng,  PAGE_BEAMNG},
  {"Flight",  iconFlight,  PAGE_FLIGHT},
  {"MCDU",    iconMcdu,    PAGE_MCDU},
};
static const uint8_t APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

// Launcher display order. settings.appOrder stores app indices; build a clean,
// de-duplicated permutation of 0..APP_COUNT-1, appending any apps the stored
// order omits (so a firmware-added app still shows up). Returns the count.
static uint8_t buildAppOrder(uint8_t out[APP_COUNT]) {
  bool used[APP_COUNT] = {};
  uint8_t n = 0;
  for (uint8_t i = 0; i < APP_ORDER_MAX && n < APP_COUNT; i++) {
    uint8_t a = settings.appOrder[i];
    if (a < APP_COUNT && !used[a]) { out[n++] = a; used[a] = true; }
  }
  for (uint8_t a = 0; a < APP_COUNT && n < APP_COUNT; a++)
    if (!used[a]) out[n++] = a;
  return n;
}

// Write a clean order back into settings.appOrder so the reorder page can swap
// adjacent slots directly; unused capacity is marked empty (0xFF).
static void normalizeAppOrder() {
  uint8_t ord[APP_COUNT];
  uint8_t n = buildAppOrder(ord);
  for (uint8_t i = 0; i < n; i++)             settings.appOrder[i] = ord[i];
  for (uint8_t i = n; i < APP_ORDER_MAX; i++) settings.appOrder[i] = 0xFF;
}

// True if app index `a` is the Menu launcher — it can never be hidden.
static bool appIsMenu(uint8_t a) { return a < APP_COUNT && APPS[a].page == PAGE_MENU; }

// Launcher order with hidden apps removed. Returns the visible count (>= 1, since
// Menu is never hidden).
static uint8_t buildVisibleOrder(uint8_t out[APP_COUNT]) {
  uint8_t ord[APP_COUNT];
  uint8_t n = buildAppOrder(ord);
  uint8_t v = 0;
  for (uint8_t i = 0; i < n; i++)
    if (!(settings.appHidden & (1u << ord[i]))) out[v++] = ord[i];
  return v;
}

static bool appGrab = false;   // PAGE_APPORDER: true = the selected app is picked up (move / hide)

// ----------------------------------------------------------------------------
//  Actions
// ----------------------------------------------------------------------------
static void applyOrientation() { u8g2.setDisplayRotation(settings.flipped ? U8G2_R0 : U8G2_R2); }

static void enterBootloader() {
  // The render task owns the panel on core 0; stop it before we draw from core 1.
  if (displayTaskHandle) vTaskSuspend(displayTaskHandle);
  delay(50);                       // let an in-flight render finish so it can't overdraw us
  u8g2.setMaxClipWindow();         // a clipped page (PFD/Music) may have left a clip active
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 26, "Flash mode:");
  u8g2.drawStr(0, 42, "run upload now");
  u8g2.sendBuffer();
  delay(600);
  usb_persist_restart(RESTART_BOOTLOADER);  // does not return
}

static void gotoPage(Page p) {
  if (page != PAGE_LAUNCHER && p == PAGE_LAUNCHER) { lastApp = page; lastAppSel = sel; }  // remember on minimize
  // Capture/Button-Test grab every button, so clear any HID currently held by
  // the engine on entry (other pages keep the non-nav buttons live).
  if (p == PAGE_CHORD_CAPTURE || p == PAGE_BTNTEST || p == PAGE_MCDUMAP) resetChordEngine();
  if (p == PAGE_CHORD_CAPTURE) captureMask = 0;
  if (p == PAGE_BTNTEST)       testLastHid = -1;
  page = p; sel = 0; displayDirty = true;
}

static void confirmCapture() {
  if (__builtin_popcount(captureMask) >= 2 && chordCount < MAX_CHORDS) {
    pendingMembers = captureMask; editingOutput = false; outputSel = firstFreeOutput();
    gotoPage(PAGE_CHORD_OUTPUT);
  } else {
    gotoPage(PAGE_CHORDS);
  }
}

// Nav-button dispatch (a: 0=UP 1=DOWN 2=SELECT 3=BACK).
static void handleNav(uint8_t a) {
  switch (page) {
    case PAGE_LAUNCHER: {
      uint8_t vis[APP_COUNT]; uint8_t vn = buildVisibleOrder(vis);
      if (sel >= vn) sel = vn - 1;
      if      (a == NAV_UP)     { if (sel > 0) sel--; }
      else if (a == NAV_DOWN)   { if (sel < vn - 1) sel++; }
      else if (a == NAV_SELECT) { gotoPage(APPS[vis[sel]].page); return; }
      else if (a == NAV_BACK)   { gotoPage(lastApp); sel = lastAppSel; return; }
      break;
    }

    case PAGE_MENU:
      if      (a == NAV_UP)   { if (sel > 0) sel--; }
      else if (a == NAV_DOWN) { if (sel < MENU_COUNT - 1) sel++; }
      else if (a == NAV_BACK) { gotoPage(PAGE_LAUNCHER); return; }
      else switch (sel) {
        case 0: gotoPage(PAGE_BTNTEST);  return;
        case 1: gotoPage(PAGE_CHORDS);   return;
        case 2: gotoPage(PAGE_SETTINGS); return;
        case 3: normalizeAppOrder(); appGrab = false; gotoPage(PAGE_APPORDER); return;
        case 4: gotoPage(PAGE_MCDUMAP);  return;
        case 5: enterBootloader();       return;
        case 6: gotoPage(PAGE_LAUNCHER); return;
      }
      break;

    case PAGE_SETTINGS:
      if      (a == NAV_UP)   { if (sel > 0) sel--; }
      else if (a == NAV_DOWN) { if (sel < SETTINGS_COUNT - 1) sel++; }
      else if (a == NAV_BACK) { gotoPage(PAGE_MENU); return; }
      else switch (sel) {
        case 0: settingsToggleFlip(); orientDirty = true; displayDirty = true;  break;
        case 1: settingsToggleLabels();                                         break;
        case 2: settingsCycleIdle();                                            break;
        case 3: settingsCycleChordWin();                                        break;
        case 4: settings.bootSel = (settings.bootSel + 1) % (APP_COUNT + 1); saveSettings(); break;
        case 5: settings.wifiMode = (settings.wifiMode + 1) % 3; saveSettings(); shellyRestartWifi(); break;
        case 6: gotoPage(PAGE_MENU); return;
      }
      break;

    case PAGE_TIMER:
      if      (a == NAV_UP)     swToggle(millis());
      else if (a == NAV_SELECT) swReset();
      else if (a == NAV_BACK)   { gotoPage(PAGE_LAUNCHER); return; }
      // NAV_DOWN (Lap): press = record split, hold = undo + open list — uiHandleTimerLap()
      break;

    case PAGE_LAPLIST: {
      uint8_t n = swLapsAvailable();
      if      (a == NAV_UP)   { if (sel > 0) sel--; }
      else if (a == NAV_DOWN) { if (n && sel < n - 1) sel++; }
      else if (a == NAV_BACK) { gotoPage(PAGE_TIMER); return; }
      break;
    }

    case PAGE_CHORDS: {
      uint8_t count = chordCount + 2;
      if      (a == NAV_UP)   { if (sel > 0) sel--; }
      else if (a == NAV_DOWN) { if (sel < count - 1) sel++; }
      else if (a == NAV_BACK) { gotoPage(PAGE_MENU); return; }
      else {
        if (sel < chordCount)       { editChord = sel; gotoPage(PAGE_CHORD_EDIT); return; }
        else if (sel == chordCount) { if (chordCount < MAX_CHORDS) gotoPage(PAGE_CHORD_CAPTURE); return; }
        else                        { gotoPage(PAGE_MENU); return; }
      }
      break;
    }

    case PAGE_CHORD_OUTPUT:
      if      (a == NAV_UP)   outputSel = (outputSel >= 31)      ? NUM_HID : outputSel + 1;
      else if (a == NAV_DOWN) outputSel = (outputSel <= NUM_HID) ? 31      : outputSel - 1;
      else if (a == NAV_BACK) { gotoPage(editingOutput ? PAGE_CHORD_EDIT : PAGE_CHORDS); return; }
      else {
        if (editingOutput) chords[editChord].output = outputSel;
        else { chords[chordCount].members = pendingMembers; chords[chordCount].output = outputSel; chordCount++; }
        chordsSave(); recomputeChordMask(); gotoPage(PAGE_CHORDS); return;
      }
      break;

    case PAGE_CHORD_EDIT:
      if      (a == NAV_UP)   { if (sel > 0) sel--; }
      else if (a == NAV_DOWN) { if (sel < 2) sel++; }
      else if (a == NAV_BACK) { gotoPage(PAGE_CHORDS); return; }
      else {
        if (sel == 0) { editingOutput = true; outputSel = chords[editChord].output; gotoPage(PAGE_CHORD_OUTPUT); return; }
        if (sel == 1) {
          for (uint8_t c = editChord; c + 1 < chordCount; c++) chords[c] = chords[c + 1];
          chordCount--; chordsSave(); recomputeChordMask(); gotoPage(PAGE_CHORDS); return;
        }
        gotoPage(PAGE_CHORDS); return;
      }
      break;

    case PAGE_DASH:
      if      (a == NAV_SELECT) { gotoPage(PAGE_PCSTATS); return; }
      else if (a == NAV_BACK)   { gotoPage(PAGE_LAUNCHER); return; }
      break;

    case PAGE_SHELLY:
      if      (a == NAV_SELECT) { shellyToggle(); }
      else if (a == NAV_BACK)   { gotoPage(PAGE_LAUNCHER); return; }
      break;

    case PAGE_WLED:
      // SELECT cycles the focused control; Up/Down act on it (3 nav buttons, 5 actions).
      // Brightness (focus 1) is press-and-hold: see uiHandleWledBright().
      if      (a == NAV_SELECT) wledFocus = (wledFocus + 1) % 3;
      else if (a == NAV_BACK)   { gotoPage(PAGE_LAUNCHER); return; }
      else if (a == NAV_UP) {
        if      (wledFocus == 0) wledPowerOn();
        else if (wledFocus == 2) wledPresetNext();
      }
      else if (a == NAV_DOWN) {
        if      (wledFocus == 0) wledPowerOff();
        else if (wledFocus == 2) wledPresetPrev();
      }
      break;

    case PAGE_MUSIC:
      if      (a == NAV_UP)     musicSendCmd("prev");
      else if (a == NAV_DOWN)   musicSendCmd("next");
      else if (a == NAV_SELECT) musicSendCmd("playpause");
      else if (a == NAV_BACK)   { gotoPage(PAGE_LAUNCHER); return; }
      break;

    case PAGE_BEAMNG:
      // Up/Down (and Select) cycle the read-only telemetry sub-views.
      if      (a == NAV_UP)     beamngView = (beamngView + BEAMNG_VIEWS - 1) % BEAMNG_VIEWS;
      else if (a == NAV_DOWN)   beamngView = (beamngView + 1) % BEAMNG_VIEWS;
      else if (a == NAV_SELECT) beamngView = (beamngView + 1) % BEAMNG_VIEWS;
      else if (a == NAV_BACK)   { gotoPage(PAGE_LAUNCHER); return; }
      break;

    case PAGE_FLIGHT:
      // Up/Down (and Select) cycle the read-only instrument sub-views.
      if      (a == NAV_UP)     flightView = (flightView + FLIGHT_VIEWS - 1) % FLIGHT_VIEWS;
      else if (a == NAV_DOWN)   flightView = (flightView + 1) % FLIGHT_VIEWS;
      else if (a == NAV_SELECT) flightView = (flightView + 1) % FLIGHT_VIEWS;
      else if (a == NAV_BACK)   { gotoPage(PAGE_LAUNCHER); return; }
      break;

    case PAGE_PCSTATS: {
      if      (a == NAV_UP)   { if (sel > 0) sel--; }
      else if (a == NAV_DOWN) { if (sel < PCSTAT_TOTAL - 1) sel++; }
      else if (a == NAV_BACK) { gotoPage(PAGE_DASH); return; }
      else if (sel >= PCSTAT_MAX_ON) { gotoPage(PAGE_DASH); return; }   // Back row
      else {
        uint8_t cur = settings.pcStatOrder[sel];
        // Build sequence: stats not used by other slots, then 0xFF (Off)
        bool used[PCSTAT_NUM] = {};
        for (uint8_t s = 0; s < PCSTAT_MAX_ON; s++)
          if (s != sel && settings.pcStatOrder[s] < PCSTAT_NUM)
            used[settings.pcStatOrder[s]] = true;
        uint8_t seq[PCSTAT_NUM + 1]; uint8_t seqN = 0;
        for (uint8_t n = 0; n < PCSTAT_NUM; n++) if (!used[n]) seq[seqN++] = n;
        seq[seqN++] = 0xFF;
        uint8_t pos = seqN - 1;   // default to Off
        for (uint8_t n = 0; n < seqN; n++) if (seq[n] == cur) { pos = n; break; }
        settings.pcStatOrder[sel] = seq[(pos + 1) % seqN];
        saveSettings();
      }
      break;
    }

    case PAGE_APPORDER:
      // Browse: Up/Down move the cursor, Select picks the app up, Back exits.
      // Picked up: Up/Down move it, Select hides/shows it (Menu locked), Back drops.
      // settings.appOrder is normalized on entry, so the first APP_COUNT slots
      // are a clean permutation we can swap in place.
      if (a == NAV_BACK) {
        if (appGrab) appGrab = false;                      // drop, back to browse
        else { gotoPage(PAGE_MENU); return; }
      }
      else if (a == NAV_SELECT) {
        if (!appGrab) appGrab = true;                      // pick up for move / hide
        else {                                             // picked up: toggle hidden
          uint8_t app = settings.appOrder[sel];
          if (!appIsMenu(app)) { settings.appHidden ^= (1u << app); saveSettings(); }
        }
      }
      else if (a == NAV_UP) {
        if (appGrab && sel > 0) {
          uint8_t t = settings.appOrder[sel]; settings.appOrder[sel] = settings.appOrder[sel - 1]; settings.appOrder[sel - 1] = t; sel--; saveSettings();
        } else if (sel > 0) sel--;
      }
      else if (a == NAV_DOWN) {
        if (appGrab && sel < APP_COUNT - 1) {
          uint8_t t = settings.appOrder[sel]; settings.appOrder[sel] = settings.appOrder[sel + 1]; settings.appOrder[sel + 1] = t; sel++; saveSettings();
        } else if (sel < APP_COUNT - 1) sel++;
      }
      break;

    // PAGE_MCDUMAP is a press-capture page (grabs every button in uiHandlePageInput),
    // so it has no nav handling here.
    case PAGE_MCDUMAP_SET:
      // Pick this button's MCDU output from the full list; Select sets it, Back cancels.
      if      (a == NAV_UP)     { if (sel > 0) sel--; }
      else if (a == NAV_DOWN)   { if (sel < MCDU_OUTPUT_COUNT - 1) sel++; }
      else if (a == NAV_BACK)   { gotoPage(PAGE_MCDUMAP); sel = mcduEditBtn; return; }
      else if (a == NAV_SELECT) {
        settings.mcduMap[mcduEditBtn] = (uint8_t)sel; saveSettings();
        gotoPage(PAGE_MCDUMAP); sel = mcduEditBtn; return;
      }
      break;

    default: break;
  }
  displayDirty = true;
}

// ----------------------------------------------------------------------------
//  Rendering
// ----------------------------------------------------------------------------
// Content area helpers — legend left when flipped=false (U8G2_R2/mounted), right when flipped=true (U8G2_R0).
static uint8_t cL() { return settings.flipped ? 0 : 18; }   // left edge of content
static uint8_t cR() { return settings.flipped ? 110 : 128; } // right edge of content (exclusive)

// Small HH:MM clock centered in the header strip, between the left-aligned title
// and any right-aligned indicator. Drawn only once the time is known (NTP or PC).
static void drawHeaderClock() {
  uint8_t h, m;
  if (!clockGet(h, m)) return;
  char t[6];
  snprintf(t, sizeof(t), "%02u:%02u", h, m);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(64 - u8g2.getStrWidth(t) / 2, 9, t);
}

static void drawListHeader(const char *title) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 10, title);
  drawHeaderClock();
  u8g2.setFont(u8g2_font_6x12_tr);   // restore: callers draw body text after the header
  u8g2.drawHLine(0, 13, 128);
}

// Legend for the 4 nav buttons. When screen is flipped the legend moves to the
// right side and hints are reversed (physical top button = last action).
static void drawNavLegend(const NavHint h[4]) {
  bool right = settings.flipped;
  int8_t cx = right ? 120 : 8;
  if (right) u8g2.drawVLine(111, 15, 48);
  else       u8g2.drawVLine(16,  15, 48);
  for (uint8_t r = 0; r < 4; r++) {
    const NavHint &hint = !settings.flipped ? h[3 - r] : h[r];
    uint8_t cy = 22 + r * 12;
    switch (hint.kind) {
      case H_UP:    u8g2.drawTriangle(cx, cy - 4, cx - 4, cy + 3, cx + 4, cy + 3); break;
      case H_DOWN:  u8g2.drawTriangle(cx - 4, cy - 3, cx + 4, cy - 3, cx, cy + 4); break;
      case H_LEFT:  u8g2.drawTriangle(cx - 4, cy, cx + 3, cy - 4, cx + 3, cy + 4); break;
      case H_RIGHT: u8g2.drawTriangle(cx + 4, cy, cx - 3, cy - 4, cx - 3, cy + 4); break;
      case H_PLAY:  u8g2.drawTriangle(cx - 3, cy - 4, cx - 3, cy + 4, cx + 4, cy); break;
      case H_PAUSE: u8g2.drawBox(cx - 3, cy - 4, 2, 8); u8g2.drawBox(cx + 1, cy - 4, 2, 8); break;
      case H_TEXT:  u8g2.setFont(u8g2_font_6x12_tr); u8g2.drawStr(cx - u8g2.getStrWidth(hint.label) / 2, cy + 4, hint.label); break;
      default: break;
    }
  }
}

static void fmtMembers(uint32_t mask, char *buf, size_t n) {
  size_t pos = 0; bool first = true;
  for (uint8_t i = 0; i < NUM_HID && pos < n - 1; i++)
    if (mask & (1u << i)) { pos += snprintf(buf + pos, n - pos, first ? "%u" : "+%u", i + 1); first = false; }
  if (first) snprintf(buf, n, "(none)");
}

static void drawHome() {
  drawListHeader("BUTTONS");
  int8_t ac = activeChordOutput();
  if (ac >= 0) {
    char c[8]; snprintf(c, sizeof(c), ">%u", ac + 1);
    u8g2.setFont(u8g2_font_5x7_tr); u8g2.drawStr(128 - u8g2.getStrWidth(c), 9, c);
  }
  const uint8_t perRow = 7, pitch = 18, bw = 16, bh = 18;
  u8g2.setFont(u8g2_font_5x7_tr);
  for (uint8_t i = 0; i < NUM_HID; i++) {
    uint8_t x = (i % perRow) * pitch + 2;
    uint8_t y = 18 + (i / perRow) * (bh + 4);
    bool on = hidHeld(i);
    if (on) u8g2.drawBox(x, y, bw, bh); else u8g2.drawFrame(x, y, bw, bh);
    char num[4]; snprintf(num, sizeof(num), "%u", settings.labelsGpio ? hidGpio(i) : (i + 1));
    u8g2.setDrawColor(on ? 0 : 1);
    u8g2.drawStr(x + 2, y + 12, num);
    u8g2.setDrawColor(1);
  }
  u8g2.sendBuffer();
}

static void drawList(const char *title, const char *const *items, uint8_t count, bool settingsValues) {
  drawListHeader(title);
  const uint8_t visible = 4;
  uint8_t start = (sel >= visible) ? (sel - visible + 1) : 0;
  for (uint8_t row = 0; row < visible && (start + row) < count; row++) {
    uint8_t idx = start + row;
    uint8_t y = 16 + row * 12;
    if (idx == sel) { u8g2.drawBox(cL(), y, 110, 12); u8g2.setDrawColor(0); }
    u8g2.drawStr(cL()+3, y + 10, items[idx]);
    if (settingsValues) {
      char val[12] = {0};
      switch (idx) {
        case 0: snprintf(val, sizeof(val), settings.flipped ? "Normal" : "Rotated"); break;
        case 1: snprintf(val, sizeof(val), settings.labelsGpio ? "GPIO" : "HID#");   break;
        case 2:
          if (settings.idleBlankSec == 0)      snprintf(val, sizeof(val), "Off");
          else if (settings.idleBlankSec < 60) snprintf(val, sizeof(val), "%us", settings.idleBlankSec);
          else                                 snprintf(val, sizeof(val), "%umin", settings.idleBlankSec / 60);
          break;
        case 3: snprintf(val, sizeof(val), "%ums", settings.chordWindowMs); break;
        case 4: {
          uint8_t bs = (settings.bootSel > APP_COUNT) ? 0 : settings.bootSel;
          snprintf(val, sizeof(val), "%s", bs == 0 ? "Apps" : APPS[bs - 1].name);
          break;
        }
        case 5: {
          const char *wm[] = {"Off", "On", "Auto"};
          snprintf(val, sizeof(val), "%s", wm[settings.wifiMode % 3]);
          break;
        }
      }
      if (val[0]) u8g2.drawStr(cR()-4 - u8g2.getStrWidth(val), y + 10, val);
    }
    u8g2.setDrawColor(1);
  }
  drawNavLegend(LIST_HINTS);
  u8g2.sendBuffer();
}

static void drawBtnTest() {
  drawListHeader("BTN TEST");
  u8g2.setFont(u8g2_font_6x12_tr);
  if (testLastHid < 0) u8g2.drawStr(0, 34, "Press any button");
  else {
    char line[24];
    snprintf(line, sizeof(line), "GPIO %u", testLastGpio);          u8g2.drawStr(0, 34, line);
    snprintf(line, sizeof(line), "HID button %u", testLastHid + 1); u8g2.drawStr(0, 50, line);
  }
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 63, "menu btn = exit");
  u8g2.sendBuffer();
}

// mm:ss.t, or h:mm:ss.t once past an hour.
static void fmtTime(uint32_t ms, char *buf, size_t n) {
  unsigned t = (ms / 100) % 10, s = (ms / 1000) % 60, m = (ms / 60000) % 60, h = ms / 3600000;
  if (h > 0) snprintf(buf, n, "%u:%02u:%02u.%u", h, m, s, t);
  else       snprintf(buf, n, "%02u:%02u.%u", m, s, t);
}

static void drawTimer() {
  uint32_t e = swElapsed(millis());
  drawListHeader("TIMER");
  char buf[16]; fmtTime(e, buf, sizeof(buf));
  u8g2.setFont(u8g2_font_10x20_tr);
  u8g2.drawStr(cL() + (110 - u8g2.getStrWidth(buf)) / 2, 40, buf);
  u8g2.setFont(u8g2_font_6x12_tr);
  char l[28];
  if (swLapCount() > 0) {
    char ts[16]; fmtTime(swLastSplit(), ts, sizeof(ts));
    snprintf(l, sizeof(l), "Lap %u: %s", (unsigned)swLapCount(), ts);
  } else snprintf(l, sizeof(l), "%s", swIsRunning() ? "running..." : "ready");
  u8g2.drawStr(cL()+2, 58, l);
  NavHint th[4] = {{swIsRunning() ? H_PAUSE : H_PLAY, ""}, {H_TEXT, "L"}, {H_TEXT, "R"}, {H_LEFT, ""}};
  drawNavLegend(th);
  u8g2.sendBuffer();
}

static void drawLapList() {
  drawListHeader("LAPS");
  uint8_t n = swLapsAvailable();
  u8g2.setFont(u8g2_font_6x12_tr);
  if (n == 0) {
    u8g2.drawStr(21, 34, "No laps yet");
  } else {
    const uint8_t visible = 4;
    uint8_t start = (sel >= visible) ? (sel - visible + 1) : 0;
    for (uint8_t row = 0; row < visible && (start + row) < n; row++) {
      uint8_t  idx = start + row;            // 0 = newest
      uint16_t lapNo = swLapCount() - idx;
      uint32_t split = swSplitByNumber(lapNo);
      uint8_t  y = 16 + row * 12;
      if (idx == sel) { u8g2.drawBox(cL(), y, 110, 12); u8g2.setDrawColor(0); }
      char lbl[8]; snprintf(lbl, sizeof(lbl), "L%u", (unsigned)lapNo);
      u8g2.drawStr(cL()+3, y + 10, lbl);
      char ts[16]; fmtTime(split, ts, sizeof(ts));
      u8g2.drawStr(cR()-4 - u8g2.getStrWidth(ts), y + 10, ts);
      u8g2.setDrawColor(1);
    }
  }
  NavHint h[4] = {{H_UP, ""}, {H_DOWN, ""}, {H_NONE, ""}, {H_LEFT, ""}};
  drawNavLegend(h);
  u8g2.sendBuffer();
}

static void drawChords() {
  drawListHeader("CHORDS");
  uint8_t count = chordCount + 2;
  const uint8_t visible = 4;
  uint8_t start = (sel >= visible) ? (sel - visible + 1) : 0;
  for (uint8_t row = 0; row < visible && (start + row) < count; row++) {
    uint8_t idx = start + row;
    uint8_t y = 16 + row * 12;
    if (idx == sel) { u8g2.drawBox(cL(), y, 110, 12); u8g2.setDrawColor(0); }
    char buf[24];
    if (idx < chordCount) { char mem[18]; fmtMembers(chords[idx].members, mem, sizeof(mem)); snprintf(buf, sizeof(buf), "%s>%u", mem, chords[idx].output + 1); }
    else if (idx == chordCount) snprintf(buf, sizeof(buf), "[ Add chord ]");
    else snprintf(buf, sizeof(buf), "Back");
    u8g2.drawStr(cL()+3, y + 10, buf);
    u8g2.setDrawColor(1);
  }
  drawNavLegend(LIST_HINTS);
  u8g2.sendBuffer();
}

static void drawCapture() {
  drawListHeader("ADD CHD");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 24, "Tap buttons to add/remove");
  char buf[40]; fmtMembers(captureMask, buf, sizeof(buf));
  u8g2.setFont(u8g2_font_6x12_tr); u8g2.drawStr(0, 44, buf);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 62, (__builtin_popcount(captureMask) >= 2) ? "MENU = save" : "need 2+ buttons");
  u8g2.sendBuffer();
}

static void drawChordOutput() {
  drawListHeader(editingOutput ? "EDIT OUT" : "OUTPUT");
  char mem[24]; fmtMembers(editingOutput ? chords[editChord].members : pendingMembers, mem, sizeof(mem));
  u8g2.setFont(u8g2_font_6x12_tr); u8g2.drawStr(cL()+2, 30, mem);
  char line[20]; snprintf(line, sizeof(line), "-> button %u", outputSel + 1);
  u8g2.drawStr(cL()+2, 48, line);
  drawNavLegend(LIST_HINTS);
  u8g2.sendBuffer();
}

static void drawChordEdit() {
  drawListHeader("EDIT CHD");
  char outItem[16]; snprintf(outItem, sizeof(outItem), "Output: %u", chords[editChord].output + 1);
  const char *items[3] = {outItem, "Delete", "Back"};
  for (uint8_t row = 0; row < 3; row++) {
    uint8_t y = 16 + row * 12;
    if (row == sel) { u8g2.drawBox(cL(), y, 110, 12); u8g2.setDrawColor(0); }
    u8g2.drawStr(cL()+3, y + 10, items[row]);
    u8g2.setDrawColor(1);
  }
  drawNavLegend(LIST_HINTS);
  u8g2.sendBuffer();
}

// ---- App icons: ~18px, drawn with primitives, centered on (cx,cy) ----
static void iconButtons(int cx, int cy) {            // 3x2 grid of small frames
  for (uint8_t r = 0; r < 2; r++)
    for (uint8_t c = 0; c < 3; c++)
      u8g2.drawFrame(cx - 9 + c * 6, cy - 6 + r * 6, 4, 4);
}
static void iconTimer(int cx, int cy) {              // stopwatch
  u8g2.drawCircle(cx, cy + 1, 8);
  u8g2.drawBox(cx - 2, cy - 11, 4, 3);               // top button
  u8g2.drawLine(cx, cy + 1, cx, cy - 4);             // minute hand
  u8g2.drawLine(cx, cy + 1, cx + 4, cy + 1);         // second hand
}
static void iconMenu(int cx, int cy) {               // three bars (hamburger)
  u8g2.drawBox(cx - 8, cy - 6, 16, 2);
  u8g2.drawBox(cx - 8, cy - 1, 16, 2);
  u8g2.drawBox(cx - 8, cy + 4, 16, 2);
}
static void iconPc(int cx, int cy) {                 // monitor on a stand
  u8g2.drawFrame(cx - 9, cy - 7, 18, 12);            // screen
  u8g2.drawHLine(cx - 4, cy + 7, 8);                 // base
  u8g2.drawVLine(cx, cy + 5, 2);                     // stem
}
static void iconShelly(int cx, int cy) {             // power plug
  u8g2.drawFrame(cx - 5, cy, 10, 7);                 // plug body
  u8g2.drawVLine(cx - 2, cy - 6, 7);                 // left prong
  u8g2.drawVLine(cx + 2, cy - 6, 7);                 // right prong
  u8g2.drawHLine(cx - 3, cy + 7, 6);                 // cord stub
}
static void iconMusic(int cx, int cy) {              // eighth note
  u8g2.drawDisc(cx - 3, cy + 5, 2);                  // note head
  u8g2.drawVLine(cx - 1, cy - 6, 12);                // stem
  u8g2.drawLine(cx - 1, cy - 6, cx + 4, cy - 4);     // flag
  u8g2.drawLine(cx - 1, cy - 2, cx + 4, cy);
}
static void iconWled(int cx, int cy) {               // light bulb with rays
  u8g2.drawCircle(cx, cy - 2, 5);                    // glass bulb
  u8g2.drawHLine(cx - 3, cy + 4, 6);                 // base
  u8g2.drawHLine(cx - 2, cy + 6, 4);
  u8g2.drawLine(cx - 9, cy - 2, cx - 6, cy - 2);     // side rays
  u8g2.drawLine(cx + 6, cy - 2, cx + 9, cy - 2);
  u8g2.drawLine(cx, cy - 11, cx, cy - 8);            // top ray
}
static void iconBeamng(int cx, int cy) {             // round gauge with a needle
  u8g2.drawCircle(cx, cy, 8);
  u8g2.drawPixel(cx - 5, cy - 5);                    // dial ticks
  u8g2.drawPixel(cx + 5, cy - 5);
  u8g2.drawPixel(cx, cy - 7);
  u8g2.drawLine(cx, cy, cx + 4, cy - 4);             // needle
  u8g2.drawDisc(cx, cy, 1);                          // hub
}

static void iconFlight(int cx, int cy) {             // top-down airplane
  u8g2.drawVLine(cx, cy - 8, 16);                    // fuselage
  u8g2.drawHLine(cx - 8, cy - 1, 17);               // main wing
  u8g2.drawHLine(cx - 3, cy + 6, 7);                // tailplane
  u8g2.drawPixel(cx, cy - 8);                        // nose tip
}

static void iconMcdu(int cx, int cy) {               // MCDU: screen over a keypad
  u8g2.drawFrame(cx - 8, cy - 9, 16, 8);             // screen
  for (uint8_t r = 0; r < 3; r++)                    // 3x3 keypad of dots
    for (uint8_t c = 0; c < 3; c++)
      u8g2.drawPixel(cx - 4 + c * 4, cy + 2 + r * 3);
}

// App launcher: 3-column icon grid (right of the nav legend); selected cell
// gets a frame. The left legend shows what the 4 nav buttons do here:
// Up / Down move the highlight, Select (►) opens, Back (◄) resumes last app.
static void drawLauncher() {
  u8g2.clearBuffer();
  const uint8_t cols = 3, cellW = 36, cellH = 32, visRows = 2;  // 2 rows fit the 64px panel
  const int gx = cL();                                // grid starts past the legend
  u8g2.setFont(u8g2_font_5x7_tr);
  uint8_t vis[APP_COUNT]; uint8_t vn = buildVisibleOrder(vis);
  if (sel >= vn) sel = vn - 1;
  // Scroll vertically so the selected cell's row stays on screen when apps overflow 2 rows.
  uint8_t selRow   = sel / cols;
  uint8_t startRow = (selRow >= visRows) ? (selRow - visRows + 1) : 0;
  for (uint8_t i = 0; i < vn; i++) {
    uint8_t c = i % cols, r = i / cols;
    if (r < startRow || r >= startRow + visRows) continue;   // outside the scroll window
    const App &app = APPS[vis[i]];
    int x0 = gx + c * cellW, y0 = (r - startRow) * cellH;
    int cx = x0 + cellW / 2, cy = y0 + 12;
    if (i == sel) u8g2.drawFrame(x0, y0, cellW, cellH);
    app.drawIcon(cx, cy);
    u8g2.drawStr(cx - u8g2.getStrWidth(app.name) / 2, y0 + 28, app.name);
  }
  drawNavLegend(LIST_HINTS);
  u8g2.sendBuffer();
}

// PC telemetry dashboard: a bar + number per enabled stat (right of the legend).
static void drawDash() {
  uint32_t now = millis();
  drawListHeader("PC STATS");
  NavHint legend[4] = {{H_NONE, ""}, {H_NONE, ""}, {H_TEXT, "C"}, {H_LEFT, ""}};  // ►=config ◄=back
  if (!pcStatsFresh(now)) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL()+2, 40, "Waiting for PC...");
    drawNavLegend(legend);
    u8g2.sendBuffer();
    return;
  }
  struct Metric { const char *lbl; int val; char unit; int barMin; int barMax; };
  const Metric ALL[PCSTAT_NUM] = {
    {"CPU",  pcStats.cpuLoad,   '%',  0, 100},
    {"RAM",  pcStats.ramUsed,   '%',  0, 100},
    {"GPU",  pcStats.gpuLoad,   '%',  0, 100},
    {"CTmp", pcStats.cpuTemp,   'C', 30, 100},
    {"GTmp", pcStats.gpuTemp,   'C', 30, 100},
    {"VRAM", pcStats.vramUsed,  '%',  0, 100},
    {"CPwr", pcStats.cpuPower,  'W',  0, 200},
    {"GPwr", pcStats.gpuPower,  'W',  0, 200},
  };
  u8g2.setFont(u8g2_font_5x7_tr);
  uint8_t shown = 0;
  for (uint8_t slot = 0; slot < PCSTAT_MAX_ON; slot++) {
    uint8_t i = settings.pcStatOrder[slot];
    if (i >= PCSTAT_NUM) continue;
    const Metric &m = ALL[i];
    uint8_t y = 16 + shown++ * 9;
    u8g2.drawStr(cL()+2, y + 7, m.lbl);
    u8g2.drawFrame(cL()+26, y, 58, 8);
    uint8_t fill = (uint8_t)((long)(constrain(m.val, m.barMin, m.barMax) - m.barMin) * 56 / (m.barMax - m.barMin));
    if (fill) u8g2.drawBox(cL()+27, y + 1, fill, 6);
    char num[10]; snprintf(num, sizeof(num), "%d%c", m.val, m.unit);
    u8g2.drawStr(cR()-2 - u8g2.getStrWidth(num), y + 7, num);
  }
  if (shown == 0) u8g2.drawStr(cL()+2, 40, "No stats enabled");
  drawNavLegend(legend);
  u8g2.sendBuffer();
}

static void drawPcStatsCfg() {
  drawListHeader("PC STATS");
  const uint8_t visible = 4;
  uint8_t start = (sel >= visible) ? (sel - visible + 1) : 0;
  for (uint8_t row = 0; row < visible && (start + row) < PCSTAT_TOTAL; row++) {
    uint8_t idx = start + row;
    uint8_t y = 16 + row * 12;
    if (idx == sel) { u8g2.drawBox(cL(), y, 110, 12); u8g2.setDrawColor(0); }
    if (idx < PCSTAT_MAX_ON) {
      uint8_t stat = settings.pcStatOrder[idx];
      char lbl[20];
      snprintf(lbl, sizeof(lbl), "%u. %s", idx + 1,
               stat < PCSTAT_NUM ? PCSTAT_ITEMS[stat] : "---");
      u8g2.drawStr(cL()+3, y + 10, lbl);
    } else {
      u8g2.drawStr(cL()+3, y + 10, "Back");
    }
    u8g2.setDrawColor(1);
  }
  drawNavLegend(LIST_HINTS);
  u8g2.sendBuffer();
}

// Launcher manage page: a list of all apps in order. Select picks the highlighted
// app up (drawn as an outline); while picked up, Up/Down move it and Select toggles
// its visibility (hidden apps show "off"). Menu can't be hidden. Back drops/exits.
static void drawAppOrder() {
  drawListHeader("APP ORD");
  const uint8_t visible = 4;
  uint8_t start = (sel >= visible) ? (sel - visible + 1) : 0;
  for (uint8_t row = 0; row < visible && (start + row) < APP_COUNT; row++) {
    uint8_t idx = start + row;
    uint8_t y = 16 + row * 12;
    uint8_t a = settings.appOrder[idx];
    bool hidden = (a < APP_COUNT) && (settings.appHidden & (1u << a));
    if (idx == sel) {
      if (appGrab) u8g2.drawFrame(cL(), y, 110, 12);            // picked up: outline, text stays normal
      else { u8g2.drawBox(cL(), y, 110, 12); u8g2.setDrawColor(0); }
    }
    u8g2.drawStr(cL() + 3, y + 10, a < APP_COUNT ? APPS[a].name : "---");
    if (hidden) { const char *o = "off"; u8g2.drawStr(cL() + 110 - 4 - u8g2.getStrWidth(o), y + 10, o); }
    u8g2.setDrawColor(1);
  }
  // Select label: browse = "Mv" (pick up); picked up = hide/show, or "--" for Menu.
  const char *selLbl = "Mv";
  if (appGrab) {
    uint8_t a = settings.appOrder[sel];
    selLbl = appIsMenu(a) ? "--" : ((settings.appHidden & (1u << a)) ? "Sh" : "Hi");
  }
  NavHint hints[4] = {{H_UP, ""}, {H_DOWN, ""}, {H_TEXT, ""}, {H_LEFT, ""}};
  strncpy(hints[2].label, selLbl, 2); hints[2].label[2] = 0;
  drawNavLegend(hints);
  u8g2.sendBuffer();
}

// Friendly name for a physical button: the 10 grid buttons are B1..B10, the 4 nav
// buttons are UP/DOWN/ENTER/BACK (HID 10..13, in NavAction order).
static const char *NAVBTN_NAMES[NUM_NAV] = {"UP", "DOWN", "ENTER", "BACK"};
static void mcduBtnLabel(uint8_t i, char *buf, size_t n) {
  if (i < NUM_ALWAYS)                 snprintf(buf, n, "B%u", i + 1);
  else if (i - NUM_ALWAYS < NUM_NAV)  snprintf(buf, n, "%s", NAVBTN_NAMES[i - NUM_ALWAYS]);
  else                                snprintf(buf, n, "?");
}

// MCDU button-remap editor (press-capture): press the physical button you want to
// remap and the output picker opens for it. The menu/toggle button exits.
static void drawMcduMap() {
  drawListHeader("MCDU KEY");
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(cL() + 2, 34, "Press a button");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(cL() + 2, 47, "to set what it does.");
  u8g2.drawStr(cL() + 2, 60, "Menu button = exit");
  u8g2.sendBuffer();
}

// Output picker submenu: choose the MCDU output for button mcduEditBtn.
static void drawMcduMapSet() {
  char bl[8]; mcduBtnLabel(mcduEditBtn, bl, sizeof(bl));
  char hdr[14]; snprintf(hdr, sizeof(hdr), "SET %s", bl);
  drawListHeader(hdr);
  const uint8_t visible = 4;
  uint8_t start = (sel >= visible) ? (sel - visible + 1) : 0;
  u8g2.setFont(u8g2_font_6x12_tr);
  for (uint8_t row = 0; row < visible && (start + row) < MCDU_OUTPUT_COUNT; row++) {
    uint8_t idx = start + row;
    uint8_t y = 16 + row * 12;
    if (idx == sel) { u8g2.drawBox(cL(), y, 110, 12); u8g2.setDrawColor(0); }
    u8g2.drawStr(cL() + 3, y + 10, mcduOutputLabel(idx));
    u8g2.setDrawColor(1);
  }
  drawNavLegend(LIST_HINTS);
  u8g2.sendBuffer();
}

static void drawShelly() {
  uint32_t now = millis();
  drawListHeader("SHELLY");
  NavHint hTg[4]   = {{H_NONE,""},{H_NONE,""},{H_TEXT,"TG"},{H_LEFT,""}};
  NavHint hNoTg[4] = {{H_NONE,""},{H_NONE,""},{H_NONE,""},{H_LEFT,""}};

  if (settings.wifiMode == WIFI_MODE_OFF) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL()+2, 38, "WiFi Off");
    drawNavLegend(hNoTg);
    u8g2.sendBuffer();
    return;
  }

  bool companion = shellyCompanionMode();
  if (!companion && !shellyWifiOk()) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL()+2, 38, shellyConfig.wifiSsid[0] ? "Connecting..." : "No WiFi set");
    drawNavLegend(hNoTg);
    u8g2.sendBuffer();
    return;
  }
  if (!shellyFresh(now)) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL()+2, 38, companion ? "Via PC..." : "Waiting...");
    drawNavLegend(hTg);
    u8g2.sendBuffer();
    return;
  }
  NavHint *h = hTg;

  // Snapshot (float reads on 32-bit arch are atomic enough for display)
  bool  on  = shellyState.output;
  float pwr = shellyState.apower;
  float vlt = shellyState.voltage;
  float cur = shellyState.current;
  float tmp = shellyState.tempC;

  // Large ON / OFF anchored to the content right edge
  const char *label = on ? "ON" : "OFF";
  u8g2.setFont(u8g2_font_10x20_tr);
  u8g2.drawStr(cR() - u8g2.getStrWidth(label), 40, label);

  u8g2.setFont(u8g2_font_5x7_tr);
  char line[24];
  snprintf(line, sizeof(line), "%.1fW", pwr);             u8g2.drawStr(cL()+2, 23, line);
  snprintf(line, sizeof(line), "%.0fV %.2fA", vlt, cur);  u8g2.drawStr(cL()+2, 33, line);
  snprintf(line, sizeof(line), "%.0fC dev", tmp);         u8g2.drawStr(cL()+2, 43, line);

  drawNavLegend(h);
  u8g2.sendBuffer();
}

static void drawWled() {
  uint32_t now = millis();
  drawListHeader("WLED");
  NavHint hCtl[4]  = {{H_UP,""},{H_DOWN,""},{H_TEXT,"Fn"},{H_LEFT,""}};  // ► cycles focus
  NavHint hBack[4] = {{H_NONE,""},{H_NONE,""},{H_NONE,""},{H_LEFT,""}};

  if (settings.wifiMode == WIFI_MODE_OFF) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL()+2, 38, "WiFi Off");
    drawNavLegend(hBack);
    u8g2.sendBuffer();
    return;
  }
  bool companion = shellyCompanionMode();
  if (!companion && !shellyWifiOk()) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL()+2, 38, shellyConfig.wifiSsid[0] ? "Connecting..." : "No WiFi set");
    drawNavLegend(hBack);
    u8g2.sendBuffer();
    return;
  }
  if (!wledFresh(now)) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL()+2, 38, companion ? "Via PC..." : "Waiting...");
    drawNavLegend(hCtl);
    u8g2.sendBuffer();
    return;
  }

  // Three controls; the focused one (SELECT cycles) is highlighted, Up/Down act on it.
  const char *labels[3] = {"Power", "Bright", "Preset"};
  char vals[3][12];
  uint8_t briShown = wledBriActive ? (uint8_t)wledBriValue : wledState.bri;  // scrolling value while held
  snprintf(vals[0], sizeof(vals[0]), "%s", wledState.on ? "ON" : "OFF");
  snprintf(vals[1], sizeof(vals[1]), "%u", briShown);
  if (wledState.preset >= 0) snprintf(vals[2], sizeof(vals[2]), "#%d", wledState.preset);
  else                       snprintf(vals[2], sizeof(vals[2]), "--");

  u8g2.setFont(u8g2_font_6x12_tr);
  for (uint8_t r = 0; r < 3; r++) {
    uint8_t y = 18 + r * 14;
    if (r == wledFocus) { u8g2.drawBox(cL(), y, 110, 13); u8g2.setDrawColor(0); }
    u8g2.drawStr(cL()+3, y + 10, labels[r]);
    u8g2.drawStr(cR()-4 - u8g2.getStrWidth(vals[r]), y + 10, vals[r]);
    u8g2.setDrawColor(1);
  }
  drawNavLegend(hCtl);
  u8g2.sendBuffer();
}

// Now-playing: title wrapped to two lines (scrolled only if a line is too wide) + play/paused state.
// Nav legend: << prev, >> next, play/pause toggle, ◄ back.
// One title line: centered if it fits, else stepped one character per second.
// The ST7920 smears continuous motion, so instead of scrolling pixel-by-pixel we
// hold the line still and jump a whole glyph each STEP_MS.
static const uint32_t STEP_MS = 1000;
static const uint32_t HOLD_MS = 5000;          // hold a freshly-changed title still, readable, before scrolling
static void drawMusicLine(const char *s, int y, int left, int avail, uint32_t now) {
  if (!s[0]) return;
  int w = u8g2.getUTF8Width(s);
  if (w <= avail) {
    u8g2.drawUTF8(left + (avail - w) / 2, y, s);
    return;
  }
  int cw = u8g2.getMaxCharWidth();
  if (cw < 1) cw = 6;
  const int period = w + cw * 2;                 // 2-glyph gap before it repeats
  // Anchor the scroll to the last song change: hold at the start for HOLD_MS so the
  // title is readable, then step one glyph per STEP_MS and loop.
  uint32_t elapsed = now - music.titleAt;
  int steps = (elapsed < HOLD_MS) ? 0 : (int)((elapsed - HOLD_MS) / STEP_MS);
  int off = (steps % (period / cw)) * cw;
  u8g2.setClipWindow(left, y - 11, left + avail, y + 2);
  u8g2.drawUTF8(left - off, y, s);
  u8g2.drawUTF8(left - off + period, y, s);      // second copy = seamless wrap
  u8g2.setMaxClipWindow();
}

// Split the title into up to two lines. Break at the first " - " (artist / song)
// with the dash dropped entirely; otherwise wrap at the last space that keeps
// line one within `avail`. A line still too wide gets scrolled by drawMusicLine.
static void splitTitle(int avail, char *l1, char *l2, size_t cap) {
  l1[0] = l2[0] = 0;
  const char *src = music.title;
  const char *dash = strstr(src, " - ");
  if (dash) {
    size_t n = (size_t)(dash - src);
    if (n > cap - 1) n = cap - 1;
    memcpy(l1, src, n); l1[n] = 0;
    strncpy(l2, dash + 3, cap - 1); l2[cap - 1] = 0;   // skip " - "
    return;
  }
  strncpy(l1, src, cap - 1); l1[cap - 1] = 0;
  if (u8g2.getUTF8Width(l1) <= avail) return;           // fits on one line
  int best = -1;
  char tmp[64];
  for (int i = 0; src[i] && i < (int)sizeof(tmp); i++) {
    if (src[i] != ' ') continue;
    memcpy(tmp, src, i); tmp[i] = 0;
    if (u8g2.getUTF8Width(tmp) <= avail) best = i; else break;
  }
  if (best > 0) {
    memcpy(l1, src, best); l1[best] = 0;
    strncpy(l2, src + best + 1, cap - 1); l2[cap - 1] = 0;
  }
}

static void drawMusic() {
  uint32_t now = millis();
  drawListHeader("MUSIC");
  NavHint mh[4]   = {{H_TEXT, "<<"}, {H_TEXT, ">>"},
                     {music.playState == 1 ? H_PAUSE : H_PLAY, ""}, {H_LEFT, ""}};
  NavHint hBack[4] = {{H_NONE, ""}, {H_NONE, ""}, {H_NONE, ""}, {H_LEFT, ""}};

  if (!musicFresh(now)) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL() + 2, 40, "Waiting for PC...");
    drawNavLegend(hBack);
    u8g2.sendBuffer();
    return;
  }
  if (music.playState == 0 || music.title[0] == 0) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(cL() + 2, 40, "Nothing playing");
    drawNavLegend(mh);
    u8g2.sendBuffer();
    return;
  }

  // Title across up to two lines (a " - " wraps artist/song; dash removed).
  // drawUTF8 + a Cyrillic-capable font so non-Latin track titles render too.
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  int left = cL() + 2, avail = (cR() - 2) - left;
  char l1[64], l2[64];
  splitTitle(avail, l1, l2, sizeof(l1));
  if (l2[0]) {
    drawMusicLine(l1, 25, left, avail, now);
    const int dashW = 24;                              // small centered divider
    u8g2.drawHLine(left + (avail - dashW) / 2, 33, dashW);
    drawMusicLine(l2, 46, left, avail, now);
  } else {
    drawMusicLine(l1, 36, left, avail, now);
  }

  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(left, 58, music.playState == 1 ? "Playing" : "Paused");
  drawNavLegend(mh);
  u8g2.sendBuffer();
}

// ---- BeamNG telemetry: read-only dash, Up/Down (or Select) cycle the sub-views ----
static void beamngGearStr(char *buf, size_t n) {
  if      (beamng.gear <= 0) snprintf(buf, n, "R");   // 0 = reverse (and <0 safety)
  else if (beamng.gear == 1) snprintf(buf, n, "N");
  else                       snprintf(buf, n, "%d", beamng.gear - 1);
}
static const char *beamngUnitStr() { return beamng.unit ? "mph" : "km/h"; }

// RPM bar scaled to the largest RPM seen this session — OutGauge carries no
// redline, so the bar self-calibrates; DL_SHIFT is the real upshift cue and
// lights the whole bar.
static uint16_t beamngRpmMax = 6000;
static void drawRpmBar(int x, int y, int w, int h, const char *txt = nullptr) {
  if (beamng.rpm > beamngRpmMax) beamngRpmMax = beamng.rpm;
  u8g2.drawFrame(x, y, w, h);
  if (txt) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(x + (w - u8g2.getStrWidth(txt)) / 2, y + (h + 7) / 2, txt);
  }
  int inner = w - 2;
  int fill = (beamng.lights & DL_SHIFT) ? inner
           : (int)((long)beamng.rpm * inner / (beamngRpmMax ? beamngRpmMax : 1));
  if (fill > 0) {
    if (fill > inner) fill = inner;
    u8g2.setDrawColor(2);                      // XOR so any text inside stays readable
    u8g2.drawBox(x + 1, y + 1, fill, h - 2);
    u8g2.setDrawColor(1);
  }
}

// A labelled value bar with the text drawn inside; the fill XORs over the text so
// it stays readable as the bar grows.
static void drawValueBar(int x, int y, int w, int h, int val, int vmin, int vmax, const char *txt) {
  u8g2.drawFrame(x, y, w, h);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(x + (w - u8g2.getStrWidth(txt)) / 2, y + (h + 7) / 2, txt);
  if (vmax > vmin) {
    int inner = w - 2;
    int fill = (int)((long)(constrain(val, vmin, vmax) - vmin) * inner / (vmax - vmin));
    if (fill > 0) { u8g2.setDrawColor(2); u8g2.drawBox(x + 1, y + 1, fill, h - 2); u8g2.setDrawColor(1); }
  }
}

// Filled blinker arrow centered at (cx,cy), pointing left or right.
static void drawBlinkArrow(int cx, int cy, bool left) {
  if (left) u8g2.drawTriangle(cx + 4, cy - 5, cx + 4, cy + 5, cx - 4, cy);
  else      u8g2.drawTriangle(cx - 4, cy - 5, cx - 4, cy + 5, cx + 4, cy);
}

struct BeamLight { const char *lbl; uint32_t bit; };

// Warning-light pictograms for the overview, ~16x12 centered on (cx,cy).
typedef void (*WarnIconFn)(int cx, int cy);
static void wiHandbrake(int cx, int cy) {            // (!) brake warning
  u8g2.drawCircle(cx, cy, 5);
  u8g2.drawVLine(cx, cy - 3, 4);
  u8g2.drawPixel(cx, cy + 3);
}
static void wiOil(int cx, int cy) {                  // oil pressure: genie lamp + drop
  u8g2.drawDisc(cx + 2, cy, 3);                              // reservoir (right)
  u8g2.drawTriangle(cx + 2, cy - 3, cx + 2, cy + 2, cx - 6, cy);  // spout tapering left
  u8g2.drawHLine(cx + 5, cy - 2, 2);                         // small handle
  u8g2.drawDisc(cx - 6, cy + 3, 1);                          // drop under the spout
}
static void wiBattery(int cx, int cy) {              // battery with + / - signs
  u8g2.drawFrame(cx - 6, cy - 2, 12, 7);
  u8g2.drawBox(cx - 4, cy - 4, 2, 2);
  u8g2.drawBox(cx + 2, cy - 4, 2, 2);
  u8g2.drawHLine(cx - 4, cy + 1, 3);                                 // minus
  u8g2.drawHLine(cx + 1, cy + 1, 3); u8g2.drawVLine(cx + 2, cy, 3);  // plus
}
static void wiAbs(int cx, int cy) {                  // ABS in a ring
  u8g2.drawCircle(cx, cy, 6);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(cx - 6, cy + 3, "ABS");
}
static void wiTc(int cx, int cy) {                   // TC in a ring
  u8g2.drawCircle(cx, cy, 6);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(cx - 4, cy + 3, "TC");
}
static void wiBeam(int cx, int cy) {                 // high beam: D + straight rays (rays on the right)
  u8g2.drawVLine(cx - 1, cy - 5, 11);
  u8g2.drawLine(cx - 1, cy - 5, cx - 5, cy - 3);
  u8g2.drawVLine(cx - 5, cy - 3, 7);
  u8g2.drawLine(cx - 5, cy + 3, cx - 1, cy + 5);
  for (int r = -3; r <= 3; r += 3) u8g2.drawHLine(cx + 2, cy + r, 3);
}
struct WarnIcon { uint32_t bit; WarnIconFn draw; };
// Priority order for the overview strip (first active ones shown, top-down).
static const WarnIcon WARN_ICONS[6] = {
  {DL_HANDBRAKE, wiHandbrake}, {DL_OILWARN, wiOil}, {DL_BATTERY, wiBattery},
  {DL_ABS, wiAbs}, {DL_TC, wiTc}, {DL_FULLBEAM, wiBeam},
};

static void drawBeamngOverview() {
  const int stripW = 18;                              // right column for warning lights
  const int xL = cL() + 2;
  const int xR = cR() - stripW;                       // left content region right edge
  const int WL = xR - xL;

  // Tall RPM bar across the top, its top edge on the header line, rpm value inside.
  char rpmTxt[12]; snprintf(rpmTxt, sizeof(rpmTxt), "%u rpm", beamng.rpm);
  drawRpmBar(xL, 13, WL, 16, rpmTxt);

  // Gear at far left, speed centered after it, blinkers flanking the speed.
  char g[4]; beamngGearStr(g, sizeof(g));
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(xL, 43, g);
  int gW = u8g2.getStrWidth(g);
  char spd[10]; snprintf(spd, sizeof(spd), "%u%s", beamng.speed, beamngUnitStr());
  int sW = u8g2.getStrWidth(spd);
  int sx = (xL + gW + 10 + xR - sW) / 2;
  if (sx < xL + gW + 12) sx = xL + gW + 12;
  if (sx + sW > xR)      sx = xR - sW;
  u8g2.drawStr(sx, 43, spd);
  if (beamng.lights & DL_SIGNAL_L) drawBlinkArrow(sx - 6, 39, true);
  if (beamng.lights & DL_SIGNAL_R) drawBlinkArrow(sx + sW + 6, 39, false);

  // Three slim value bars (fuel / engine / oil), number inside each, side by side.
  int bw = (WL - 4) / 3;
  char nb[8];
  snprintf(nb, sizeof(nb), "F%u", beamng.fuel);    drawValueBar(xL,            50, bw, 13, beamng.fuel,    0, 100, nb);
  snprintf(nb, sizeof(nb), "E%d", beamng.engTemp); drawValueBar(xL + bw + 2,   50, bw, 13, beamng.engTemp, 30, 120, nb);
  snprintf(nb, sizeof(nb), "O%d", beamng.oilTemp); drawValueBar(xL + 2*bw + 4, 50, bw, 13, beamng.oilTemp, 30, 120, nb);

  // Active warning lights as icons stacked from the top of the right column (up
  // to 4). Inactive ones don't appear; turn signals live by the speed, not here.
  int icx = cR() - stripW / 2;
  int shown = 0;
  for (uint8_t i = 0; i < 6 && shown < 4; i++) {
    if (!(beamng.lights & WARN_ICONS[i].bit)) continue;
    WARN_ICONS[i].draw(icx, 21 + shown * 12);
    shown++;
  }
}

static void drawBeamngDash() {
  int x = cL() + 2, w = (cR() - 2) - x;
  drawRpmBar(x, 16, w, 9);

  char g[4]; beamngGearStr(g, sizeof(g));
  u8g2.setFont(u8g2_font_10x20_tr);
  u8g2.drawStr(x, 50, g);                              // big gear, left

  char spd[6]; snprintf(spd, sizeof(spd), "%u", beamng.speed);
  int sW = u8g2.getStrWidth(spd);
  int sx = (cR() - 2) - sW - 10;                       // big speed toward the right
  u8g2.drawStr(sx, 46, spd);

  u8g2.setFont(u8g2_font_5x7_tr);
  const char *u = beamngUnitStr();
  u8g2.drawStr(sx + (sW - u8g2.getStrWidth(u)) / 2, 58, u);   // unit centered under speed
  char r[12]; snprintf(r, sizeof(r), "%u rpm", beamng.rpm);
  u8g2.drawStr(x, 62, r);

  // Blinkers left & right of the speed.
  if (beamng.lights & DL_SIGNAL_L) drawBlinkArrow(sx - 8, 37, true);
  if (beamng.lights & DL_SIGNAL_R) drawBlinkArrow(cR() - 6, 37, false);
}

static void drawBeamngStatus() {
  const int xl = cL() + 2, xr = cR() - 2, lblW = 26;
  const char *lbl[4] = {"Fuel", "Eng", "Oil", "Turbo"};
  int val[4]  = {beamng.fuel, beamng.engTemp, beamng.oilTemp, beamng.turbo};
  int vmin[4] = {0, 30, 30, 0};
  int vmax[4] = {100, 120, 120, 20};
  char txt[4][10];
  snprintf(txt[0], sizeof(txt[0]), "%u%%", beamng.fuel);
  snprintf(txt[1], sizeof(txt[1]), "%dC", beamng.engTemp);
  snprintf(txt[2], sizeof(txt[2]), "%dC", beamng.oilTemp);
  if (beamng.turboFlag) snprintf(txt[3], sizeof(txt[3]), "%u.%ub", beamng.turbo / 10, beamng.turbo % 10);
  else                { snprintf(txt[3], sizeof(txt[3]), "--"); vmax[3] = vmin[3]; }  // empty bar
  u8g2.setFont(u8g2_font_5x7_tr);
  for (uint8_t i = 0; i < 4; i++) {
    int y = 16 + i * 12;
    u8g2.drawStr(xl, y + 9, lbl[i]);
    drawValueBar(xl + lblW, y, xr - (xl + lblW), 11, val[i], vmin[i], vmax[i], txt[i]);
  }
}

static void drawBeamngLights() {
  const BeamLight items[8] = {
    {"HBRK", DL_HANDBRAKE}, {"ABS",  DL_ABS},
    {"TC",   DL_TC},        {"OIL",  DL_OILWARN},
    {"BATT", DL_BATTERY},   {"BEAM", DL_FULLBEAM},
    {"LEFT", DL_SIGNAL_L},  {"RIGHT",DL_SIGNAL_R},
  };
  u8g2.setFont(u8g2_font_5x7_tr);
  const int cw = 52, ch = 11;
  for (uint8_t i = 0; i < 8; i++) {
    int col = i % 2, row = i / 2;
    int x = cL() + 2 + col * (cw + 2);
    int y = 16 + row * (ch + 1);
    if (beamng.lights & items[i].bit) { u8g2.drawBox(x, y, cw, ch); u8g2.setDrawColor(0); }
    else                                u8g2.drawFrame(x, y, cw, ch);
    u8g2.drawStr(x + 3, y + 8, items[i].lbl);
    u8g2.setDrawColor(1);
  }
}

// Centered status icon for the two "no data" states (no caption text).
static void drawBeamngNoPc(int cx, int cy) {           // monitor with a slash = no PC link
  u8g2.drawFrame(cx - 15, cy - 11, 30, 20);
  u8g2.drawHLine(cx - 6, cy + 12, 12);
  u8g2.drawVLine(cx, cy + 9, 3);
  u8g2.drawLine(cx - 17, cy + 11, cx + 17, cy - 13);
}
static void drawBeamngIdle(int cx, int cy) {           // parked car = BeamNG not driving
  u8g2.drawBox(cx - 15, cy, 30, 6);                    // body
  u8g2.drawFrame(cx - 9, cy - 7, 18, 8);               // cabin
  u8g2.drawDisc(cx - 9, cy + 8, 3);                    // wheels
  u8g2.drawDisc(cx + 9, cy + 8, 3);
}

static void drawBeamng() {
  uint32_t now = millis();
  static const char *VIEW_NAMES[BEAMNG_VIEWS] = {"OVERVIEW", "DASH", "STATUS", "LIGHTS"};
  drawListHeader("BEAMNG");
  u8g2.setFont(u8g2_font_5x7_tr);
  const char *vn = VIEW_NAMES[beamngView % BEAMNG_VIEWS];
  u8g2.drawStr(126 - u8g2.getStrWidth(vn), 9, vn);    // view tag, right of the header

  NavHint h[4] = {{H_UP, ""}, {H_DOWN, ""}, {H_NONE, ""}, {H_LEFT, ""}};
  int icx = (cL() + cR()) / 2;

  if (!beamngFresh(now)) {                             // no companion / serial link
    drawBeamngNoPc(icx, 36);
    drawNavLegend(h);
    u8g2.sendBuffer();
    return;
  }
  if (!beamng.active) {                                // companion up, BeamNG not driving
    drawBeamngIdle(icx, 34);
    drawNavLegend(h);
    u8g2.sendBuffer();
    return;
  }

  switch (beamngView) {
    case 0: drawBeamngOverview(); break;
    case 1: drawBeamngDash();     break;
    case 2: drawBeamngStatus();   break;
    case 3: drawBeamngLights();   break;
  }
  drawNavLegend(h);
  u8g2.sendBuffer();
}

// ---- Flight (MSFS) telemetry sub-views ---------------------------------------

// A vertical "rolling tape" (airspeed / altitude). axisX is the scale line;
// leftTape=true puts ticks+labels to its left with the value box on the left and a
// pointer poking right (toward the attitude); false mirrors it. Center shows value.
static void drawVTape(int axisX, bool leftTape, int yT, int yB, long value,
                      int step, float ppu, const char *readout) {
  int cyT = (yT + yB) / 2;
  u8g2.drawVLine(axisX, yT, yB - yT + 1);
  u8g2.setFont(u8g2_font_4x6_tr);
  long range = (long)((yB - yT) / 2 / ppu) + step;
  long m0 = ((value - range) / step) * step;
  for (long m = m0; m <= value + range; m += step) {
    if (m < 0) continue;
    int y = cyT - (int)roundf((m - value) * ppu);
    if (y < yT + 2 || y > yB - 2) continue;
    char lab[8]; snprintf(lab, sizeof(lab), "%ld", m);
    if (leftTape) { u8g2.drawHLine(axisX - 3, y, 3); u8g2.drawStr(axisX - 5 - u8g2.getStrWidth(lab), y + 2, lab); }
    else          { u8g2.drawHLine(axisX + 1, y, 3); u8g2.drawStr(axisX + 5, y + 2, lab); }
  }
  u8g2.setFont(u8g2_font_5x7_tr);                       // current-value box + pointer
  int rw = u8g2.getStrWidth(readout) + 3, rh = 11, by = cyT - rh / 2;
  int bx = leftTape ? (axisX - rw) : axisX;
  u8g2.setDrawColor(0); u8g2.drawBox(bx, by, rw, rh); u8g2.setDrawColor(1);
  u8g2.drawFrame(bx, by, rw, rh);
  u8g2.drawStr(bx + 2, by + 8, readout);
  if (leftTape) u8g2.drawTriangle(axisX, cyT - 3, axisX, cyT + 3, axisX + 3, cyT);
  else          u8g2.drawTriangle(axisX, cyT - 3, axisX, cyT + 3, axisX - 3, cyT);
}

// Horizontal heading tape with a centered current-heading readout box.
static void drawHdgTape(int x0, int x1, int yTop, int hdg) {
  int cx = (x0 + x1) / 2, base = yTop + 8;
  u8g2.drawHLine(x0, base, x1 - x0);
  const float ppd = 1.2f;
  for (int d = -40; d <= 40; d += 10) {                 // ticks; longer at 30deg marks
    int tx = cx + (int)roundf(d * ppd);
    if (tx < x0 || tx > x1) continue;
    bool major = ((((hdg + d) % 30) + 30) % 30) == 0;
    u8g2.drawVLine(tx, base - (major ? 4 : 2), major ? 4 : 2);
  }
  char b[6]; snprintf(b, sizeof(b), "%03d", hdg);
  u8g2.setFont(u8g2_font_5x7_tr);
  int rw = u8g2.getStrWidth(b) + 4, bx = cx - rw / 2;
  u8g2.setDrawColor(0); u8g2.drawBox(bx, yTop, rw, 9); u8g2.setDrawColor(1);
  u8g2.drawFrame(bx, yTop, rw, 9);
  u8g2.drawStr(bx + 2, yTop + 7, b);
}

static void drawFlightCore() {                          // PFD: tapes + attitude + heading
  int gx0 = cL(), gx1 = cR();
  const int yT = 14, yB = 51;
  int asAxis = gx0 + 20, altAxis = gx1 - 30;
  int adiL = asAxis + 4, adiR = altAxis - 4;
  int adiCx = (adiL + adiR) / 2, adiCy = (yT + yB) / 2;

  // Attitude indicator with a pitch ladder. The aircraft symbol is fixed at the
  // centre; the horizon (0deg) and the +/-10/20deg rungs slide with pitch and roll
  // with bank. Clipped to the ADI box so it never spills into the tapes.
  // Frames: along-axis = (c,-s) (the rung direction), down-axis = (s,c).
  u8g2.setClipWindow(adiL, yT, adiR, yB);
  const float ppd = 0.9f;                                // pixels per degree of pitch
  float br = flight.bank * DEG_TO_RAD, c = cosf(br), s = sinf(br);
  float hyc = adiCy + flight.pitch * ppd;                // horizon centre (the 0deg line)
  static const int LADDER[] = {-20, -10, 0, 10, 20};
  u8g2.setFont(u8g2_font_4x6_tr);
  for (uint8_t i = 0; i < 5; i++) {
    int th = LADDER[i];
    float p = -th * ppd;                                 // perp offset from horizon (up = negative)
    float rcx = adiCx + p * s, rcy = hyc + p * c;        // rung centre
    int hw = (th == 0) ? (adiR - adiL) / 2 : 8;          // horizon spans the box; rungs are short
    int x1 = (int)roundf(rcx - hw * c), y1 = (int)roundf(rcy + hw * s);
    int x2 = (int)roundf(rcx + hw * c), y2 = (int)roundf(rcy - hw * s);
    u8g2.drawLine(x1, y1, x2, y2);
    if (th != 0) {                                       // degree label at the rung ends
      char d[4]; snprintf(d, sizeof(d), "%d", th < 0 ? -th : th);
      u8g2.drawStr(x2 + 1, y2 + 2, d);
      u8g2.drawStr(x1 - 1 - u8g2.getStrWidth(d), y1 + 2, d);
    }
  }
  u8g2.setMaxClipWindow();
  int rr = (yB - yT) / 2 - 1;                            // bank pointer on the top arc
  float pa = (-90.0f + flight.bank) * DEG_TO_RAD;
  int bpx = adiCx + (int)(rr * cosf(pa)), bpy = adiCy + (int)(rr * sinf(pa));
  u8g2.drawTriangle(bpx, bpy, bpx - 2, bpy - 3, bpx + 2, bpy - 3);
  u8g2.drawHLine(adiCx - 8, adiCy, 5);                   // fixed aircraft symbol
  u8g2.drawHLine(adiCx + 4, adiCy, 5);
  u8g2.drawPixel(adiCx, adiCy);

  char b[12];                                            // speed + altitude tapes (unit-aware)
  bool mph = settings.flightUnits & FU_SPEED_MPH, altM = settings.flightUnits & FU_ALT_M;
  long iasV = mph ? lroundf(flight.ias * 1.15078f) : (long)flight.ias;
  long altV = altM ? lroundf(flight.alt * 0.3048f) : (long)flight.alt;
  snprintf(b, sizeof(b), "%ld", iasV);
  drawVTape(asAxis, true, yT, yB, iasV, 20, 0.5f, b);
  snprintf(b, sizeof(b), "%ld", altV);
  drawVTape(altAxis, false, yT, yB, altV, altM ? 50 : 200, altM ? 0.16f : 0.05f, b);

  drawHdgTape(gx0, gx1 - 26, 53, flight.hdg);            // heading tape (bottom)
  snprintf(b, sizeof(b), "%+d", flight.vs);              // VS readout in its own corner
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(gx1 - 24, 58, "VS");
  u8g2.drawStr(gx1 - 24, 63, b);
}

// Annunciator chip: filled (inverse text) when active, outline when not.
static void drawChip(int x, int y, int w, int h, const char *label, bool active) {
  if (active) { u8g2.drawBox(x, y, w, h); u8g2.setDrawColor(0); }
  else          u8g2.drawFrame(x, y, w, h);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(x + (w - u8g2.getStrWidth(label)) / 2, y + (h + 7) / 2 - 1, label);
  u8g2.setDrawColor(1);
}

static void drawFlightAp() {                            // FMA mode strip + FCU windows
  int gx0 = cL(), gx1 = cR(), w = gx1 - gx0;
  const char *fl[5] = {"AP", "HDG", "ALT", "NAV", "APR"};
  bool on[5] = { (bool)flight.apMaster, (bool)(flight.apModes & AP_HDG),
                 (bool)(flight.apModes & AP_ALT), (bool)(flight.apModes & AP_NAV),
                 (bool)(flight.apModes & AP_APR) };
  int cw = (w - 4) / 5;                                  // FMA annunciator row
  for (int i = 0; i < 5; i++) drawChip(gx0 + i * (cw + 1), 14, cw, 11, fl[i], on[i]);

  const int w1 = 40, by = 28, bh = 28;                   // HDG | ALT selected windows
  int w2 = w - w1 - 3, ax = gx0 + w1 + 3;
  char v[12];
  u8g2.drawFrame(gx0, by, w1, bh);
  u8g2.setFont(u8g2_font_5x7_tr); u8g2.drawStr(gx0 + 3, by + 8, "HDG");
  snprintf(v, sizeof(v), "%03u", flight.apHdgSel);
  u8g2.setFont(u8g2_font_10x20_tr);
  u8g2.drawStr(gx0 + (w1 - u8g2.getStrWidth(v)) / 2, by + bh - 4, v);

  u8g2.drawFrame(ax, by, w2, bh);
  u8g2.setFont(u8g2_font_5x7_tr); u8g2.drawStr(ax + 3, by + 8, "ALT");
  snprintf(v, sizeof(v), "%ld", (long)flight.apAltSel);
  u8g2.setFont(u8g2_font_10x20_tr);
  if (u8g2.getStrWidth(v) > w2 - 4) u8g2.setFont(u8g2_font_6x12_tr);   // 6-digit fallback
  u8g2.drawStr(ax + (w2 - u8g2.getStrWidth(v)) / 2, by + bh - 4, v);
}

// Landing-gear indicator: solid = down & locked, hollow = up, crossed = in transit.
static void drawGear(int cx, int cy, uint8_t pct) {
  if (pct >= 100)    { u8g2.drawDisc(cx, cy, 3); u8g2.drawVLine(cx, cy - 6, 3); }
  else if (pct == 0)   u8g2.drawCircle(cx, cy, 3);
  else               { u8g2.drawCircle(cx, cy, 3); u8g2.drawLine(cx - 2, cy - 2, cx + 2, cy + 2); }
}

static void drawFlightConfig() {                        // gear diagram + flaps/splr/brake
  int gx0 = cL(), gx1 = cR(), mid = gx0 + 44;
  char b[12];
  // Gear cluster (left), drawn in an aircraft layout: nose + two mains.
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(gx0, 21, "GEAR");
  int gcx = gx0 + 20;
  drawGear(gcx, 32, flight.gearPct);
  drawGear(gcx - 12, 48, flight.gearPct);
  drawGear(gcx + 12, 48, flight.gearPct);
  const char *gs = flight.gearPct >= 100 ? "DOWN" : flight.gearPct == 0 ? "UP" : "TRANS";
  u8g2.drawStr(gcx - u8g2.getStrWidth(gs) / 2, 62, gs);
  // Flaps / spoilers bars + parking-brake annunciator (right).
  int rx = mid, rw = gx1 - mid;
  snprintf(b, sizeof(b), "FLAP %u%%", flight.flapsPct); drawValueBar(rx, 16, rw, 11, flight.flapsPct, 0, 100, b);
  snprintf(b, sizeof(b), "SPLR %u%%", flight.spoilers); drawValueBar(rx, 30, rw, 11, flight.spoilers, 0, 100, b);
  drawChip(rx, 45, rw, 13, flight.parkBrake ? "PARK BRK" : "P BRK OFF", flight.parkBrake);
}

// Round dial gauge: 240deg scale (lower-left -> top -> lower-right) with ticks, a
// needle for `val`, the label above and the numeric readout below.
static void drawDial(int cx, int cy, int r, long val, long vmin, long vmax,
                     const char *label, const char *valTxt) {
  const float A0 = 210.0f * DEG_TO_RAD, SW = 240.0f * DEG_TO_RAD;
  u8g2.drawCircle(cx, cy, r);
  for (uint8_t i = 0; i <= 4; i++) {
    float a = A0 - SW * i / 4.0f, ca = cosf(a), sa = sinf(a);
    u8g2.drawLine(cx + (int)((r - 3) * ca), cy - (int)((r - 3) * sa),
                  cx + (int)(r * ca),       cy - (int)(r * sa));
  }
  float frac = (vmax > vmin) ? (float)(constrain(val, vmin, vmax) - vmin) / (float)(vmax - vmin) : 0.0f;
  float a = A0 - SW * frac;
  u8g2.drawLine(cx, cy, cx + (int)((r - 2) * cosf(a)), cy - (int)((r - 2) * sinf(a)));
  u8g2.drawDisc(cx, cy, 1);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(cx - u8g2.getStrWidth(label) / 2, cy - r - 1, label);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(cx - u8g2.getStrWidth(valTxt) / 2, cy + r + 7, valTxt);
}

// Vertical EICAS-style bar: label above, fill from the bottom, value below.
static void drawVBar(int x, int y, int w, int h, float frac, const char *label, const char *val) {
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(x + (w - u8g2.getStrWidth(label)) / 2, y - 1, label);
  u8g2.drawFrame(x, y, w, h);
  int fill = (int)(frac * (h - 2));
  if (fill < 0) fill = 0; else if (fill > h - 2) fill = h - 2;
  if (fill) u8g2.drawBox(x + 1, y + h - 1 - fill, w - 2, fill);
  u8g2.drawStr(x + (w - u8g2.getStrWidth(val)) / 2, y + h + 6, val);
}

// Engine view: one gauge per engine (settings.engStyle: 0 dials, 1 EICAS bars), N
// from flight.nEng. Throttle + fuel shown alongside. RPM/N1 per flight.engType.
static void drawFlightEngine() {
  int gx0 = cL(), gx1 = cR(), w = gx1 - gx0;
  bool jet = (flight.engType == 1);
  uint16_t ev[4] = {flight.engPrimary, flight.eng2, flight.eng3, flight.eng4};
  uint8_t n = flight.nEng < 1 ? 1 : flight.nEng > 4 ? 4 : flight.nEng;
  long emax = jet ? 110 : 3000;                          // N1 % (x1) or RPM
  char b[10];

  if (settings.engStyle == 0) {                          // ---- dial gauges + THR/FUEL bars ----
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(gx0, 12, jet ? "N1" : "RPM");
    for (uint8_t k = 0; k < n; k++) {
      long pv = jet ? ev[k] / 10 : ev[k];
      int r = n >= 4 ? 8 : n == 3 ? 10 : n == 2 ? 13 : 15;
      int cx = gx0 + (2 * k + 1) * w / (2 * n);
      char lab[4]; snprintf(lab, sizeof(lab), "E%u", k + 1);
      if (jet) snprintf(b, sizeof(b), "%u.%u", ev[k] / 10, ev[k] % 10);
      else     snprintf(b, sizeof(b), "%u", ev[k]);
      drawDial(cx, 30, r, pv, 0, emax, lab, b);
    }
    snprintf(b, sizeof(b), "THR %u%%", flight.throttle); drawValueBar(gx0 + 2, 50, w - 4, 6, flight.throttle, 0, 100, b);
    snprintf(b, sizeof(b), "FUEL %u%%", flight.fuelPct); drawValueBar(gx0 + 2, 57, w - 4, 6, flight.fuelPct, 0, 100, b);
  } else {                                               // ---- EICAS vertical bars ----
    uint8_t cols = n + 2;                                // engines + THR + FUEL
    int bw = (w - 2) / cols;
    for (uint8_t k = 0; k < cols; k++) {
      int x = gx0 + 1 + k * bw;
      float frac; char lab[4];
      if (k < n) {
        long pv = jet ? ev[k] / 10 : ev[k];
        frac = emax ? (float)pv / emax : 0.0f;
        snprintf(lab, sizeof(lab), "%u", k + 1);
        snprintf(b, sizeof(b), "%ld", pv);
      } else if (k == n) {
        frac = flight.throttle / 100.0f; snprintf(lab, sizeof(lab), "TH");
        snprintf(b, sizeof(b), "%u", flight.throttle);
      } else {
        frac = flight.fuelPct / 100.0f; snprintf(lab, sizeof(lab), "FU");
        snprintf(b, sizeof(b), "%u", flight.fuelPct);
      }
      drawVBar(x, 20, bw - 2, 30, frac, lab, b);
    }
  }
}

static void drawFlight() {
  uint32_t now = millis();
  static const char *VIEW_NAMES[FLIGHT_VIEWS] = {"PFD", "A/P", "CONFIG", "ENGINE"};
  drawListHeader("FLIGHT");
  u8g2.setFont(u8g2_font_5x7_tr);
  const char *vn = VIEW_NAMES[flightView % FLIGHT_VIEWS];
  u8g2.drawStr(126 - u8g2.getStrWidth(vn), 9, vn);    // view tag, right of the header

  NavHint h[4] = {{H_UP, ""}, {H_DOWN, ""}, {H_NONE, ""}, {H_LEFT, ""}};
  int icx = (cL() + cR()) / 2;

  if (!flightFresh(now)) {                             // no companion / serial link
    drawBeamngNoPc(icx, 36);
    drawNavLegend(h);
    u8g2.sendBuffer();
    return;
  }
  if (!flight.active) {                                // companion up, MSFS not running
    iconFlight(icx, 36);
    drawNavLegend(h);
    u8g2.sendBuffer();
    return;
  }

  switch (flightView) {
    case 0: drawFlightCore();   break;
    case 1: drawFlightAp();     break;
    case 2: drawFlightConfig(); break;
    case 3: drawFlightEngine(); break;
  }
  drawNavLegend(h);
  u8g2.sendBuffer();
}

// ---- MCDU (FlyByWire) full-screen mirror --------------------------------------
// No header/legend: every button is repurposed as an MCDU key, so the whole panel
// is the screen. Title pinned at the top row, scratchpad pinned at the bottom, the
// 12 body rows scrolled in an MCDU_BODY_VIS-tall window. Font is 5x7 (24*5=120px).
static void drawMcdu() {
  uint32_t now = millis();
  u8g2.clearBuffer();
  if (!mcduFresh(now)) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(6, 36, "Waiting for MCDU...");
    u8g2.sendBuffer();
    return;
  }
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 7, mcdu.rows[0]);                       // title (row 0), pinned top
  int start = 1 + mcdu.scrollOff;                         // first body row shown
  for (uint8_t v = 0; v < MCDU_BODY_VIS; v++) {
    uint8_t row = start + v;
    if (row > MCDU_BODY) break;
    u8g2.drawStr(0, 15 + v * 8, mcdu.rows[row]);
  }
  u8g2.drawStr(0, 63, mcdu.rows[MCDU_ROWS - 1]);          // scratchpad (row 13), pinned bottom
  u8g2.sendBuffer();
}

static void render() {
  switch (page) {
    case PAGE_LAUNCHER:      drawLauncher();                                             break;
    case PAGE_BUTTONS:       drawHome();                                                 break;
    case PAGE_MENU:          drawList("MENU", MENU_ITEMS, MENU_COUNT, false);            break;
    case PAGE_SETTINGS:      drawList("SETTINGS", SETTINGS_ITEMS, SETTINGS_COUNT, true); break;
    case PAGE_BTNTEST:       drawBtnTest();                                              break;
    case PAGE_TIMER:         drawTimer();                                                break;
    case PAGE_LAPLIST:       drawLapList();                                              break;
    case PAGE_CHORDS:        drawChords();                                               break;
    case PAGE_CHORD_CAPTURE: drawCapture();                                              break;
    case PAGE_CHORD_OUTPUT:  drawChordOutput();                                          break;
    case PAGE_CHORD_EDIT:    drawChordEdit();                                            break;
    case PAGE_DASH:          drawDash();                                                 break;
    case PAGE_PCSTATS:       drawPcStatsCfg();                                           break;
    case PAGE_APPORDER:      drawAppOrder();                                             break;
    case PAGE_MCDUMAP:       drawMcduMap();                                              break;
    case PAGE_MCDUMAP_SET:   drawMcduMapSet();                                           break;
    case PAGE_SHELLY:        drawShelly();                                               break;
    case PAGE_MUSIC:         drawMusic();                                                break;
    case PAGE_WLED:          drawWled();                                                 break;
    case PAGE_BEAMNG:        drawBeamng();                                               break;
    case PAGE_FLIGHT:        drawFlight();                                               break;
    case PAGE_MCDU:          drawMcdu();                                                 break;
  }
}

// ----------------------------------------------------------------------------
//  Public interface
// ----------------------------------------------------------------------------
void uiBegin() {
  displayBegin();
  applyOrientation();
  uint8_t bs = (settings.bootSel > APP_COUNT) ? 0 : settings.bootSel;   // 0 = launcher, else app
  page = (bs == 0) ? PAGE_LAUNCHER : APPS[bs - 1].page;
  if (page != PAGE_LAUNCHER) { lastApp = page; lastAppSel = 0; }        // so menu btn resumes it
  // Render on core 0 so the slow ST7920 flush never stalls the input loop (core 1).
  xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 1, &displayTaskHandle, 0);
}
Page uiPage()  { return page; }

uint8_t uiAppCount()                   { return APP_COUNT; }
uint8_t uiGetAppOrder(uint8_t *out)    { return buildAppOrder(out); }
void    uiSetAppOrder(const uint8_t *order, uint8_t n) {
  for (uint8_t i = 0; i < APP_ORDER_MAX; i++) settings.appOrder[i] = (i < n) ? order[i] : 0xFF;
  normalizeAppOrder();
  saveSettings();
  if (page == PAGE_LAUNCHER || page == PAGE_APPORDER) displayDirty = true;
}
uint16_t uiGetAppHidden() { return settings.appHidden; }
void     uiSetAppHidden(uint16_t mask) {
  for (uint8_t i = 0; i < APP_COUNT; i++) if (appIsMenu(i)) mask &= ~(1u << i);   // Menu stays visible
  settings.appHidden = mask;
  saveSettings();
  if (page == PAGE_LAUNCHER || page == PAGE_APPORDER) displayDirty = true;
}
void uiNoteActivity(uint32_t now) { lastActivity = now; blanked = false; displayDirty = true; }
void uiApplyOrientation() { orientDirty = true; displayDirty = true; }   // host link: re-apply on render task
void uiEnterFlash()       { enterBootloader(); }

// Menu button = global home/switch key. Tap (short press, fires on release):
// in any app -> launcher; on the launcher -> resume the last app; capture saves.
// Hold (>= MENU_HOLD_MS): quick-switch straight to the previous app, bypassing the
// launcher and swapping last<->current so repeated holds toggle between two apps.
void uiHandleMenuButton(uint32_t now) {
  if (pressedEdge(toggleBtn)) { menuHoldStart = now; menuHoldHandled = false; }

  if (toggleBtn.pressed && !menuHoldHandled && page != PAGE_CHORD_CAPTURE &&
      (now - menuHoldStart) >= MENU_HOLD_MS) {
    menuHoldHandled = true;
    Page   cur    = (page == PAGE_LAUNCHER) ? lastApp : page;   // app we're leaving
    int8_t curSel = (page == PAGE_LAUNCHER) ? lastAppSel : sel;
    gotoPage(lastApp); sel = lastAppSel;       // jump to the previous app
    lastApp = cur; lastAppSel = curSel;        // ...and make it the next target
    return;
  }

  if (releasedEdge(toggleBtn)) {
    if (menuHoldHandled) return;               // hold already switched; ignore the tap
    if (page == PAGE_CHORD_CAPTURE) { confirmCapture(); return; }
    if (page == PAGE_LAUNCHER)      { gotoPage(lastApp); sel = lastAppSel; return; }
    gotoPage(PAGE_LAUNCHER);
  }
}

void uiHandlePageInput() {
  // Capture/Button-Test need every button for the page; other UI pages only need
  // the nav buttons, leaving the non-nav buttons live as HID (handled by the
  // chord engine). A "claimed" button is suppressed so it doesn't also send HID.
  // MCDU repurposes EVERY button as an MCDU key (forwarded raw to the companion),
  // so it grabs all of them too; the menu/toggle button stays the exit.
  // PAGE_MCDUMAP captures a button press to pick which one to remap, so it grabs all too.
  bool grabAll = (page == PAGE_CHORD_CAPTURE || page == PAGE_BTNTEST || page == PAGE_MCDU ||
                  page == PAGE_MCDUMAP);
  for (uint8_t i = 0; i < NUM_HID; i++) {
    if (!pressedEdge(physBtn(i))) continue;
    bool isNav = (i >= NUM_ALWAYS);
    if (!grabAll && !isNav) continue;             // non-nav button stays a live gamepad button
    uiSuppressedMask |= (1u << i);
    if (page == PAGE_CHORD_CAPTURE)   { captureMask ^= (1u << i); displayDirty = true; }
    else if (page == PAGE_BTNTEST)    { testLastHid = i; testLastGpio = hidGpio(i); displayDirty = true; }
    else if (page == PAGE_MCDU)       { mcduHandleButton(i); displayDirty = true; }   // map -> scroll/key
    else if (page == PAGE_MCDUMAP)    {                       // press a button -> pick its output
      mcduEditBtn = i; gotoPage(PAGE_MCDUMAP_SET); sel = settings.mcduMap[i]; return;
    }
    else if (isNav) {
      uint8_t ni = i - NUM_ALWAYS;
      ni = (NUM_NAV - 1) - ni;
      handleNav(ni);
    }
  }
}

void uiHandleTimerLap(uint32_t now) {
  // NAV_DOWN physical index, reversed when screen is flipped
  uint8_t lapIdx = NUM_NAV - 1 - NAV_DOWN;
  Button &lap = navBtns[lapIdx];
  if (pressedEdge(lap)) {
    lapHoldStart = now; lapHoldHandled = false;
    lapRecordedPress = swRecordLap(now);
    if (lapRecordedPress) displayDirty = true;
  }
  if (lap.pressed && !lapHoldHandled && (now - lapHoldStart) >= LAP_HOLD_MS) {
    if (lapRecordedPress) swUndoLastLap();
    gotoPage(PAGE_LAPLIST);
    lapHoldHandled = true;
  }
}

// WLED brightness is adjusted by holding Up/Down while "Bright" is focused: the
// value scrolls locally every WLED_BRI_REPEAT_MS and is sent to the device only
// once, on release. Self-gating, so it can be called every loop; leaving the page
// mid-hold also flushes the pending value.
void uiHandleWledBright(uint32_t now) {
  Button &upBtn   = navBtns[NUM_NAV - 1 - NAV_UP];
  Button &downBtn = navBtns[NUM_NAV - 1 - NAV_DOWN];
  int dir = 0;
  if      (upBtn.pressed && !downBtn.pressed) dir = +1;
  else if (downBtn.pressed && !upBtn.pressed) dir = -1;

  bool canAdjust = (page == PAGE_WLED) && (wledFocus == 1) &&
                   (settings.wifiMode != WIFI_MODE_OFF) && wledFresh(now);

  if (canAdjust && dir != 0) {
    if (!wledBriActive) {                           // hold just started
      wledBriActive = true;
      wledBriValue  = wledState.bri;
      wledBriStepAt = now - WLED_BRI_REPEAT_MS;     // first step fires immediately
    }
    if ((now - wledBriStepAt) >= WLED_BRI_REPEAT_MS) {
      wledBriValue  = constrain(wledBriValue + dir * WLED_BRI_STEP, 0, 255);
      wledBriStepAt = now;
      uiNoteActivity(now);                          // wake + redraw the scrolling number
    }
  } else if (wledBriActive) {                       // released (or left brightness focus)
    wledBriActive = false;
    wledSetBrightness((uint8_t)wledBriValue);       // send the final value once
    wledState.bri = (uint8_t)wledBriValue;          // optimistic echo until the next poll
    uiNoteActivity(now);
  }
}

// One pass of the display logic: idle-blank + heartbeat/dirty redraw. Runs only on
// the core-0 render task, which is the sole owner of the panel at runtime. Core 1
// merely raises displayDirty / orientDirty / blanked; this consumes them.
static void displayService() {
  uint32_t now = millis();
  if (orientDirty) { orientDirty = false; applyOrientation(); displayDirty = true; }

  bool keepAwake = (page == PAGE_TIMER && swIsRunning()) ||
                   (page == PAGE_DASH && pcStatsFresh(now)) ||
                   (page == PAGE_MUSIC && musicFresh(now)) ||
                   (page == PAGE_BEAMNG && beamngFresh(now)) ||
                   (page == PAGE_FLIGHT && flightFresh(now)) ||
                   (page == PAGE_MCDU && mcduFresh(now)) ||
                   ((page == PAGE_SHELLY || page == PAGE_WLED) &&
                    settings.wifiMode != WIFI_MODE_OFF &&
                    (shellyWifiOk() || shellyCompanionMode()));
  if (!blanked && !keepAwake && settings.idleBlankSec > 0 &&
      (now - lastActivity) > (uint32_t)settings.idleBlankSec * 1000) {
    u8g2.clearBuffer(); u8g2.sendBuffer(); blanked = true;
  }
  uint32_t heartbeat = keepAwake ? 100 : 200;
  bool dirty = displayDirty;
  displayDirty = false;            // clear before drawing so a change mid-render re-marks it
  if (!blanked && (dirty || (now - lastDraw) > heartbeat)) {
    render(); lastDraw = now;
  }
}

static void displayTask(void *) {
  for (;;) {
    displayService();
    vTaskDelay(pdMS_TO_TICKS(5));   // ~200 Hz cap; render self-throttles via dirty/heartbeat
  }
}
