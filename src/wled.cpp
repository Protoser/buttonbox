#include "wled.h"
#include "shelly.h"      // shellyCompanionMode(), shellyWifiOk() — shared WiFi lifecycle
#include "settings.h"
#include "hostlink.h"    // hostlinkSend() — bounded, non-blocking serial write
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>

WledState wledState;

static char wledConfigIp[20] = {};

static const uint32_t STALE_MS   = 6000;
static const uint32_t POLL_MS    = 2000;

// Direct-mode command bits passed through xTaskNotify (set only when NOT in
// companion mode, so they never collide with serial-routed control).
static const uint32_t C_ON      = 1u << 0;
static const uint32_t C_OFF     = 1u << 1;
static const uint32_t C_BRI_SET = 1u << 2;   // absolute brightness in wledBriTarget
static const uint32_t C_PS_NEXT = 1u << 3;
static const uint32_t C_PS_PREV = 1u << 4;

static TaskHandle_t      wledTaskHandle = nullptr;
static SemaphoreHandle_t stateMutex     = nullptr;
static volatile int      wledBriTarget  = -1;   // 0..255 pending absolute brightness

// ── Tiny JSON readers (WLED /json/state has on/bri/ps at the top level, before
//    the nested nl/udpn/seg objects, so first-match indexOf is correct) ────────

static long jsonInt(const String &s, const char *key, long def) {
  String k = String("\"") + key + "\":";
  int idx = s.indexOf(k);
  if (idx < 0) return def;
  idx += k.length();
  while (idx < (int)s.length() && s[idx] == ' ') idx++;
  return s.substring(idx).toInt();          // toInt() parses a leading sign too
}

static bool jsonBool(const String &s, const char *key, bool def) {
  String k = String("\"") + key + "\":";
  int idx = s.indexOf(k);
  if (idx < 0) return def;
  idx += k.length();
  while (idx < (int)s.length() && s[idx] == ' ') idx++;
  char c = (idx < (int)s.length()) ? s[idx] : 0;
  return c == 't' ? true : c == 'f' ? false : def;
}

// ── HTTP (WLED is unauthenticated by default — no Digest dance needed) ─────────

static bool wledGet(const char *path, String &body) {
  if (!wledConfigIp[0]) return false;
  char url[80];
  snprintf(url, sizeof(url), "http://%s%s", wledConfigIp, path);
  HTTPClient http;
  http.begin(url);
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  int code = http.GET();
  bool ok = (code == 200);
  if (ok) body = http.getString();
  http.end();
  return ok;
}

static bool wledPost(const char *jsonBody) {
  if (!wledConfigIp[0]) return false;
  char url[80];
  snprintf(url, sizeof(url), "http://%s/json/state", wledConfigIp);
  HTTPClient http;
  http.begin(url);
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t *)jsonBody, strlen(jsonBody));
  http.end();
  return code == 200;
}

// ── Background task (core 0; shares the WiFi the Shelly task manages) ──────────

static void wledTaskFn(void *) {
  for (;;) {
    uint32_t cmd = 0;
    xTaskNotifyWait(0, 0xFFFFFFFF, &cmd, pdMS_TO_TICKS(POLL_MS));

    // In companion mode the PC owns the HTTP; we neither poll nor send.
    if (shellyCompanionMode()) continue;
    if (!shellyWifiOk())       continue;
    if (!wledConfigIp[0])      continue;

    // Apply any queued control, then poll fresh state so the page updates fast.
    if (cmd & C_ON)      wledPost("{\"on\":true}");
    if (cmd & C_OFF)     wledPost("{\"on\":false}");
    if (cmd & C_BRI_SET) {
      int t = wledBriTarget;
      if (t >= 0) { char b[20]; snprintf(b, sizeof(b), "{\"bri\":%d}", t); wledPost(b); }
    }
    if (cmd & C_PS_NEXT) wledPost("{\"ps\":\"1~250~\"}");     // cycle to next saved preset (skips empties, wraps)
    if (cmd & C_PS_PREV) wledPost("{\"ps\":\"250~1~\"}");     // best-effort reverse; companion path does precise prev

    String body;
    if (wledGet("/json/state", body)) {
      WledState s;
      s.on     = jsonBool(body, "on", false);
      s.bri    = (uint8_t)jsonInt(body, "bri", 0);
      s.preset = (int16_t)jsonInt(body, "ps", -1);
      s.lastRx = millis();
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      wledState = s;
      xSemaphoreGive(stateMutex);
    }
  }
}

static void notify(uint32_t bit) {
  if (wledTaskHandle) xTaskNotify(wledTaskHandle, bit, eSetBits);
}

// ── Public API ────────────────────────────────────────────────────────────────

void wledLoadConfig() {
  Preferences p; p.begin("bbox", true);
  p.getString("wledip", wledConfigIp, sizeof(wledConfigIp));
  p.end();
}

void wledSaveConfig() {
  Preferences p; p.begin("bbox", false);
  p.putString("wledip", wledConfigIp);
  p.end();
}

void wledSetIp(const char *ip) {
  strncpy(wledConfigIp, ip, sizeof(wledConfigIp) - 1);
  wledConfigIp[sizeof(wledConfigIp) - 1] = '\0';
  wledSaveConfig();
}

const char *wledIp() { return wledConfigIp; }

void wledBegin() {
  memset(&wledState, 0, sizeof(wledState));
  memset(wledConfigIp, 0, sizeof(wledConfigIp));
  wledLoadConfig();
  stateMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(wledTaskFn, "wled", 8192, nullptr, 1, &wledTaskHandle, 0);
}

// Each action: serial request when the companion is driving, else queue a direct call.
void wledPowerOn()  { if (shellyCompanionMode()) hostlinkSend("wledcmd on\n");  else notify(C_ON); }
void wledPowerOff() { if (shellyCompanionMode()) hostlinkSend("wledcmd off\n"); else notify(C_OFF); }
void wledPresetNext() { if (shellyCompanionMode()) hostlinkSend("wledcmd ps+\n"); else notify(C_PS_NEXT); }
void wledPresetPrev() { if (shellyCompanionMode()) hostlinkSend("wledcmd ps-\n"); else notify(C_PS_PREV); }

void wledSetBrightness(uint8_t bri) {
  if (shellyCompanionMode()) {
    char buf[20]; snprintf(buf, sizeof(buf), "wledcmd bri %u\n", bri); hostlinkSend(buf);
  } else {
    wledBriTarget = bri;
    notify(C_BRI_SET);
  }
}

void wledApplyFromCompanion(char *line) {
  WledState s;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  s = wledState;
  xSemaphoreGive(stateMutex);

  char *tok = strtok(line, " ");
  while (tok) {
    char *colon = strchr(tok, ':');
    if (colon) {
      *colon = '\0';
      if      (!strcmp(tok, "on"))  s.on     = (atoi(colon + 1) != 0);
      else if (!strcmp(tok, "bri")) s.bri    = (uint8_t)atoi(colon + 1);
      else if (!strcmp(tok, "ps"))  s.preset = (int16_t)atoi(colon + 1);
    }
    tok = strtok(nullptr, " ");
  }
  s.lastRx = millis();

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  wledState = s;
  xSemaphoreGive(stateMutex);
}

bool wledFresh(uint32_t now) {
  return wledState.lastRx != 0 && (now - wledState.lastRx) < STALE_MS;
}
