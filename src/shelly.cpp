#include "shelly.h"
#include "settings.h"
#include "pcstats.h"
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "mbedtls/sha256.h"

ShellyState  shellyState;
ShellyConfig shellyConfig;

static const uint32_t STALE_MS      = 6000;
static const uint32_t POLL_MS       = 2000;
static const uint32_t WIFI_RETRY_MS = 20000;

static TaskHandle_t      shellyTaskHandle = nullptr;
static SemaphoreHandle_t stateMutex       = nullptr;

// ── Crypto ────────────────────────────────────────────────────────────────────

static void sha256hex(const char *input, char *out65) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256
  mbedtls_sha256_update(&ctx, (const uint8_t *)input, strlen(input));
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) snprintf(out65 + i * 2, 3, "%02x", hash[i]);
  out65[64] = '\0';
}

// ── Digest auth helpers ───────────────────────────────────────────────────────

// Extract a named token from a Digest WWW-Authenticate header.
// Handles both quoted (key="val") and unquoted (key=val) forms.
static bool extractDigestParam(const char *hdr, const char *key, char *out, size_t n) {
  const char *p = strstr(hdr, key);
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ') p++;
  if (*p != '=') return false;
  p++;
  while (*p == ' ') p++;
  bool q = (*p == '"');
  if (q) p++;
  size_t i = 0;
  while (*p && i < n - 1) {
    if (q  && *p == '"') break;
    if (!q && (*p == ',' || *p == '\r' || *p == '\n' || *p == ' ')) break;
    out[i++] = *p++;
  }
  out[i] = '\0';
  return i > 0;
}

// HTTP GET with automatic Digest SHA-256 auth (two-step: get nonce, then auth).
static bool shellyGet(const char *uriPath, String &body) {
  if (!shellyConfig.shellyIp[0]) return false;
  char url[80];
  snprintf(url, sizeof(url), "http://%s%s", shellyConfig.shellyIp, uriPath);

  // Step 1 — unauthenticated request to get the nonce
  HTTPClient http;
  http.begin(url);
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  {
    const char *hdrs[] = {"WWW-Authenticate"};
    http.collectHeaders(hdrs, 1);
  }
  int code = http.GET();
  if (code == 200) { body = http.getString(); http.end(); return true; }
  if (code != 401) { http.end(); return false; }

  // Step 2 — compute Digest response and retry
  String wwwAuth = http.header("WWW-Authenticate");
  http.end();

  char realm[64] = {}, nonce[64] = {};
  extractDigestParam(wwwAuth.c_str(), "realm", realm, sizeof(realm));
  extractDigestParam(wwwAuth.c_str(), "nonce", nonce, sizeof(nonce));
  if (!realm[0] || !nonce[0]) return false;

  char ha1[65], ha2[65], response[65], tmp[256];
  snprintf(tmp, sizeof(tmp), "%s:%s:%s", shellyConfig.shellyUser, realm, shellyConfig.shellyPass);
  sha256hex(tmp, ha1);
  snprintf(tmp, sizeof(tmp), "GET:%s", uriPath);
  sha256hex(tmp, ha2);
  const char *nc = "00000001", *cnonce = "ab12cd34";
  snprintf(tmp, sizeof(tmp), "%s:%s:%s:%s:auth:%s", ha1, nonce, nc, cnonce, ha2);
  sha256hex(tmp, response);

  char authHdr[512];
  snprintf(authHdr, sizeof(authHdr),
    "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\","
    " nc=%s, cnonce=\"%s\", qop=auth, response=\"%s\", algorithm=SHA-256",
    shellyConfig.shellyUser, realm, nonce, uriPath, nc, cnonce, response);

  HTTPClient http2;
  http2.begin(url);
  http2.setConnectTimeout(1500);
  http2.setTimeout(2000);
  http2.addHeader("Authorization", authHdr);
  code = http2.GET();
  bool ok = (code == 200);
  if (ok) body = http2.getString();
  http2.end();
  return ok;
}

// ── JSON helpers (no library; Shelly Gen2 responses are flat) ────────────────

static float jsonFloat(const String &s, const char *key, float def) {
  String k = String("\"") + key + "\":";
  int idx = s.indexOf(k);
  if (idx < 0) return def;
  idx += k.length();
  while (idx < (int)s.length() && s[idx] == ' ') idx++;
  return s.substring(idx).toFloat();
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

// "temperature":{"tC":48.5, ...}
static float jsonNestedFloat(const String &s, const char *outer, const char *inner, float def) {
  String ko = String("\"") + outer + "\":";
  int idx = s.indexOf(ko);
  if (idx < 0) return def;
  int brace = s.indexOf('{', idx + ko.length());
  if (brace < 0) return def;
  int end = s.indexOf('}', brace);
  if (end < 0) return def;
  return jsonFloat(s.substring(brace, end + 1), inner, def);
}

// ── WiFi mode helpers ─────────────────────────────────────────────────────────

static bool shouldUseWifi() {
  if (settings.wifiMode == WIFI_MODE_OFF) return false;
  if (settings.wifiMode == WIFI_MODE_ON)  return true;
  return !pcStatsFresh(millis());   // AUTO: use WiFi only when companion is absent
}

// ── Background task (runs on core 0; main loop on core 1 never blocks) ───────

static void shellyTaskFn(void *) {
  uint32_t wifiRetry = 0;
  for (;;) {
    uint32_t notifyVal = 0;
    xTaskNotifyWait(0, 0xFFFFFFFF, &notifyVal, pdMS_TO_TICKS(POLL_MS));

    // bit 1 = mode/credentials changed; reset retry so reconnect happens immediately
    if (notifyVal & 2) wifiRetry = 0;

    if (!shouldUseWifi()) {
      // Disconnect WiFi if it was running under a previous mode
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_MODE_NULL);
      }
      continue;
    }

    // WiFi management — reconnect if needed
    if (WiFi.status() != WL_CONNECTED) {
      uint32_t now = millis();
      if (shellyConfig.wifiSsid[0] && (now - wifiRetry) > WIFI_RETRY_MS) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(shellyConfig.wifiSsid, shellyConfig.wifiPass);
        wifiRetry = now;
      }
      continue;
    }
    if (!shellyConfig.shellyIp[0]) continue;

    // Toggle if requested (bit 0)
    if (notifyVal & 1) {
      String b;
      shellyGet("/rpc/Switch.Toggle?id=0", b);
    }

    // Poll status
    String body;
    if (shellyGet("/rpc/Switch.GetStatus?id=0", body)) {
      ShellyState s;
      s.output  = jsonBool(body,  "output",  false);
      s.apower  = jsonFloat(body, "apower",  0.0f);
      s.voltage = jsonFloat(body, "voltage", 0.0f);
      s.current = jsonFloat(body, "current", 0.0f);
      s.tempC   = jsonNestedFloat(body, "temperature", "tC", 0.0f);
      s.lastRx  = millis();
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      shellyState = s;
      xSemaphoreGive(stateMutex);
    }
  }
}

// ── Public API ────────────────────────────────────────────────────────────────

void shellyLoadConfig() {
  Preferences p; p.begin("bbox", true);
  p.getString("wssid",  shellyConfig.wifiSsid,  sizeof(shellyConfig.wifiSsid));
  p.getString("wpass",  shellyConfig.wifiPass,  sizeof(shellyConfig.wifiPass));
  p.getString("ship",   shellyConfig.shellyIp,  sizeof(shellyConfig.shellyIp));
  p.getString("shuser", shellyConfig.shellyUser, sizeof(shellyConfig.shellyUser));
  p.getString("shpass", shellyConfig.shellyPass, sizeof(shellyConfig.shellyPass));
  p.end();
}

void shellySaveConfig() {
  Preferences p; p.begin("bbox", false);
  p.putString("wssid",  shellyConfig.wifiSsid);
  p.putString("wpass",  shellyConfig.wifiPass);
  p.putString("ship",   shellyConfig.shellyIp);
  p.putString("shuser", shellyConfig.shellyUser);
  p.putString("shpass", shellyConfig.shellyPass);
  p.end();
}

void shellyRestartWifi() {
  // Notify task (bit 1) to re-evaluate mode/creds; it manages WiFi start/stop itself
  if (shellyTaskHandle) xTaskNotify(shellyTaskHandle, 2, eSetBits);
}

void shellyBegin() {
  memset(&shellyState, 0, sizeof(shellyState));
  memset(&shellyConfig, 0, sizeof(shellyConfig));
  shellyLoadConfig();
  stateMutex = xSemaphoreCreateMutex();
  // Task manages WiFi connect/disconnect based on mode; let it handle the initial state too
  xTaskCreatePinnedToCore(shellyTaskFn, "shelly", 8192, nullptr, 1, &shellyTaskHandle, 0);
}

void shellyQueueToggle() {
  if (shellyTaskHandle) xTaskNotify(shellyTaskHandle, 1, eSetBits);
}

void shellyToggle() {
  if (shellyCompanionMode()) {
    Serial.println("shelly_toggle");   // companion will do the HTTP call
  } else {
    shellyQueueToggle();
  }
}

bool shellyCompanionMode() {
  return settings.wifiMode == WIFI_MODE_AUTO && pcStatsFresh(millis());
}

void shellyApplyFromCompanion(char *line) {
  // Parse tokens like "out:1 apower:45.2 voltage:230.1 current:0.196 temp:40.5"
  ShellyState s;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  s = shellyState;
  xSemaphoreGive(stateMutex);

  char *tok = strtok(line, " ");
  while (tok) {
    char *colon = strchr(tok, ':');
    if (colon) {
      *colon = '\0';
      if      (!strcmp(tok, "out"))     s.output  = (atoi(colon + 1) != 0);
      else if (!strcmp(tok, "apower"))  s.apower  = atof(colon + 1);
      else if (!strcmp(tok, "voltage")) s.voltage = atof(colon + 1);
      else if (!strcmp(tok, "current")) s.current = atof(colon + 1);
      else if (!strcmp(tok, "temp"))    s.tempC   = atof(colon + 1);
    }
    tok = strtok(nullptr, " ");
  }
  s.lastRx = millis();

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  shellyState = s;
  xSemaphoreGive(stateMutex);
}

bool shellyFresh(uint32_t now) {
  return shellyState.lastRx != 0 && (now - shellyState.lastRx) < STALE_MS;
}

bool shellyWifiOk() { return WiFi.status() == WL_CONNECTED; }
