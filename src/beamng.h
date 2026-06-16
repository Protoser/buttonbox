// BeamNG.drive telemetry received over the USB CDC serial link.
//
// The companion app listens for BeamNG's built-in OutGauge UDP protocol
// (Options > Other > Protocols > OutGauge, default 127.0.0.1:4444) and forwards
// one whitespace-separated "key:value" line per update, prefixed so hostlink can
// route it:
//   beamng act:1 gear:3 spd:120 unit:0 rpm:4200 fuel:78 et:90 ot:95 tb:8 tf:1 lights:84
// Keys: act 1=BeamNG sending telemetry / 0=idle; gear raw OutGauge byte
// (0=R,1=N,2=1st..); spd in display units; unit 0=km/h 1=mph; rpm; fuel %;
// et/ot engine/oil temp °C; tb turbo boost in tenths of a bar; tf turbo applies;
// lights = the OutGauge showLights bitfield (DL_* below). Unknown keys are
// ignored so the protocol can grow without firmware changes.
#pragma once
#include <Arduino.h>

// OutGauge showLights (DL_*) bits — must match the companion's mapping.
static const uint32_t DL_SHIFT     = 0x0001;  // upshift light
static const uint32_t DL_FULLBEAM  = 0x0002;
static const uint32_t DL_HANDBRAKE = 0x0004;
static const uint32_t DL_TC        = 0x0010;  // traction control
static const uint32_t DL_SIGNAL_L  = 0x0020;
static const uint32_t DL_SIGNAL_R  = 0x0040;
static const uint32_t DL_OILWARN   = 0x0100;
static const uint32_t DL_BATTERY   = 0x0200;
static const uint32_t DL_ABS       = 0x0400;

struct BeamNGState {
  uint8_t  active;     // 1 = BeamNG actively sending telemetry, 0 = idle
  int8_t   gear;       // raw OutGauge byte: 0 = R, 1 = N, 2 = 1st gear ...
  uint16_t speed;      // in display units (km/h or mph per `unit`)
  uint8_t  unit;       // 0 = km/h, 1 = mph
  uint16_t rpm;        // engine RPM
  uint8_t  fuel;       // %
  int8_t   engTemp;    // °C
  int8_t   oilTemp;    // °C
  uint8_t  turbo;      // boost, tenths of a bar (8 = 0.8 bar)
  uint8_t  turboFlag;  // 1 = turbo gauge applies to this car
  uint32_t lights;     // OutGauge showLights bitfield (DL_*)
  uint32_t lastRx;     // millis() of last complete line (0 = never)
};
extern BeamNGState beamng;

void beamngApply(char *line, uint32_t now);   // parse one telemetry line (modifies line)
bool beamngFresh(uint32_t now);               // true if a line arrived recently
