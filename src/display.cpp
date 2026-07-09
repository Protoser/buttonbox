#include "display.h"
#include "config.h"
#include <math.h>

// ST7920 in software SPI: clock=E, data=R/W, cs=RS. Rotation is applied from
// saved settings via ui's applyOrientation(); the constructor value is a default.
U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R2, LCD_CLOCK_PIN, LCD_DATA_PIN, LCD_CS_PIN, U8X8_PIN_NONE);

// Backlight PWM (Arduino-ESP32 2.x LEDC API). 20 kHz is above hearing and any
// visible flicker; 8-bit duty gives 256 brightness steps.
static const uint8_t  BL_CHANNEL  = 0;
static const uint32_t BL_FREQ_HZ  = 20000;
static const uint8_t  BL_RES_BITS = 8;

// Perceived LED brightness is roughly the PWM duty raised to ~1/gamma, so a linear
// duty ramp looks lopsided — big jumps near 0, almost none near full. Pre-distorting
// the duty by this gamma makes equal % steps look evenly spaced. Bump it up for more
// aggressive low-end dimming, down toward 1.0 for a near-linear response.
static const float BL_GAMMA = 2.2f;

void displaySetBacklight(uint8_t pct) {
  if (pct > 100) pct = 100;
  uint32_t duty = (uint32_t)lroundf(powf(pct / 100.0f, BL_GAMMA) * 255.0f);
  if (pct > 0 && duty == 0) duty = 1;   // never let a non-zero level fall to fully off
  ledcWrite(BL_CHANNEL, duty);
}

void displayBegin() {
  u8g2.begin();
  ledcSetup(BL_CHANNEL, BL_FREQ_HZ, BL_RES_BITS);
  ledcAttachPin(LCD_BACKLIGHT_PIN, BL_CHANNEL);
  displaySetBacklight(100);   // full on until displayService applies the saved level
}
