// Flight-sim telemetry received over the USB CDC serial link.
//
// The companion app reads Microsoft Flight Simulator via SimConnect and forwards
// one whitespace-separated "key:value" line per update, prefixed so hostlink can
// route it:
//   flight act:1 ias:120 alt:5500 hdg:270 vs:-300 pit:3 bnk:-12 apm:1 aphdg:270
//          apalt:6000 apmode:5 gear:100 flaps:25 pbrk:0 splr:0 thr:80 eng:2400 fuel:73 etyp:0
// Keys: act 1=sim sending telemetry / 0=idle; ias indicated airspeed (kt);
// alt indicated altitude (ft); hdg magnetic heading (deg); vs vertical speed (fpm,
// signed); pit pitch deg (+up); bnk bank deg (+right); apm autopilot master;
// aphdg/apalt selected heading/altitude;
// apmode AP_* bitfield; gear/flaps/splr extension %; pbrk parking brake;
// thr throttle %; eng/eng2/eng3/eng4 per-engine gauge (RPM, or N1 in tenths of %
// per etyp); neng engine count (1..4); fuel remaining %; etyp engine type
// (0 piston, 1 jet, 2 turboprop). Unknown keys are ignored so the protocol can grow.
#pragma once
#include <Arduino.h>

// Autopilot mode (AP_*) bits — must match the companion's mapping.
static const uint8_t AP_HDG = 0x01;  // heading hold
static const uint8_t AP_ALT = 0x02;  // altitude hold
static const uint8_t AP_NAV = 0x04;  // nav (GPS/VOR) tracking
static const uint8_t AP_APR = 0x08;  // approach hold

struct FlightState {
  uint8_t  active;     // 1 = sim actively sending telemetry, 0 = idle
  uint16_t ias;        // indicated airspeed, kt
  int32_t  alt;        // indicated altitude, ft
  uint16_t hdg;        // magnetic heading, deg (0..359)
  int16_t  vs;         // vertical speed, fpm (signed)
  int8_t   pitch;      // pitch attitude, deg (+ = nose up)
  int8_t   bank;       // bank attitude, deg (+ = roll right)
  uint8_t  apMaster;   // 1 = autopilot master on
  uint16_t apHdgSel;   // selected heading, deg
  int32_t  apAltSel;   // selected altitude, ft
  uint8_t  apModes;    // AP_* bitfield
  uint8_t  gearPct;    // gear extension %, 0..100
  uint8_t  flapsPct;   // flaps handle %, 0..100
  uint8_t  parkBrake;  // 1 = parking brake set
  uint8_t  spoilers;   // spoiler/speedbrake handle %, 0..100
  uint8_t  throttle;   // throttle lever %, 0..100
  uint16_t engPrimary; // engine #1 gauge: RPM (piston/turboprop) or N1 x10 (jet)
  uint16_t eng2;       // engine #2 gauge (same units as engPrimary)
  uint16_t eng3;       // engine #3 gauge
  uint16_t eng4;       // engine #4 gauge
  uint8_t  nEng;       // number of engines (1..4)
  uint8_t  fuelPct;    // fuel remaining %, 0..100
  uint8_t  engType;    // 0 piston, 1 jet, 2 turboprop
  uint32_t lastRx;     // millis() of last complete line (0 = never)
};
extern FlightState flight;

void flightApply(char *line, uint32_t now);   // parse one telemetry line (modifies line)
bool flightFresh(uint32_t now);               // true if a line arrived recently
