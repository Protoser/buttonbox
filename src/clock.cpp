#include "clock.h"
#include "shelly.h"      // shellyWifiOk() — piggyback on the shared WiFi lifecycle
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

// Below this epoch the system clock has never been set (it powers up near 1970).
static const time_t  SYNCED_EPOCH = 1700000000;   // ~Nov 2023
static const int32_t TZ_UNKNOWN   = 0x7FFFFFFF;   // NVS sentinel: offset not learned yet

static int32_t tzOffsetSec = 0;       // local UTC offset, applied to UTC for display
static bool    tzKnown     = false;   // true once the companion has told us the offset
static bool    ntpStarted  = false;   // configTime() called (SNTP then resyncs hourly)

void clockBegin() {
  Preferences p; p.begin("bbox", true);
  int32_t v = p.getInt("tzoff", TZ_UNKNOWN);
  p.end();
  if (v != TZ_UNKNOWN) { tzOffsetSec = v; tzKnown = true; }
}

// "<epochUTC> <offsetSec>" — set the system clock and learn/persist the TZ offset.
void clockApplyHost(const char *args, uint32_t) {
  char *end = nullptr;
  long long epoch = strtoll(args, &end, 10);
  if (end == args || epoch < SYNCED_EPOCH) return;
  long off = strtol(end, nullptr, 10);

  struct timeval tv = { (time_t)epoch, 0 };
  settimeofday(&tv, nullptr);                       // UTC; offset is applied at read time

  if (!tzKnown || off != tzOffsetSec) {
    tzOffsetSec = off;
    tzKnown     = true;
    Preferences p; p.begin("bbox", false);
    p.putInt("tzoff", off);
    p.end();
  }
}

void clockUpdate(uint32_t) {
  // Bring SNTP up once WiFi associates; LWIP then re-syncs roughly hourly on its
  // own. Offset 0 here keeps the system clock in UTC — we apply tzOffsetSec at read.
  if (!ntpStarted && shellyWifiOk()) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    ntpStarted = true;
  }
}

bool clockGet(uint8_t &h, uint8_t &m) {
  if (!tzKnown) return false;                       // don't know the local offset yet
  time_t utc = time(nullptr);
  if (utc < SYNCED_EPOCH) return false;             // never synced by NTP or the PC
  time_t local = utc + tzOffsetSec;
  struct tm t; gmtime_r(&local, &t);                // gmtime: read the shifted epoch as wall time
  h = (uint8_t)t.tm_hour;
  m = (uint8_t)t.tm_min;
  return true;
}
