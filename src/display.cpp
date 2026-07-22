#include "display.h"
#include "config.h"
#include <math.h>
#include <SPI.h>                 // pre-init the SPI bus with MISO=-1 (see displayBegin)
#include "driver/gpio.h"        // gpio_hold_en/dis: latch the backlight pin through a reboot
#include "soc/soc.h"            // REG_CLR_BIT
#include "soc/rtc_cntl_reg.h"   // RTC_CNTL_DIG_ISO_REG / DG_PAD_FORCE_UNHOLD

// ST7920 in hardware SPI: E=SCK (clock), R/W=MOSI (data), RS=CS. The HW-SPI class
// only takes cs/reset; the bus is pinned to our SCK/MOSI in displayBegin(). HW SPI
// replaced the original bit-banged SW SPI to raise the flush rate (needed for
// temporal-dither / FRC). Rotation is applied via ui's applyOrientation().
U8G2_ST7920_128X64_F_HW_SPI u8g2(U8G2_R2, LCD_CS_PIN, U8X8_PIN_NONE);

// Default SPI bus clock. ST7920 is nominally rated ~2.5 MHz but usually tolerates
// more; the DEBUG page lets you push past this live to find the real ceiling.
static const uint32_t LCD_SPI_HZ_DEFAULT = 1000000;
static uint32_t g_spiHz = LCD_SPI_HZ_DEFAULT;

void displaySetSpiClock(uint32_t hz) { g_spiHz = hz; u8g2.setBusClock(hz); }
uint32_t displayGetSpiClock() { return g_spiHz; }

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

// Flash Mode: once the chip reboots into the ROM bootloader the LEDC PWM stops and
// the backlight pin floats -> screen goes dark while flashing. Drive the pin solid
// high and latch it with the pad-hold (RTC-domain, survives a software reset) so the
// "run upload now" prompt stays lit. displayBegin() releases the hold on next boot.
void displayBacklightHoldFull() {
  ledcDetachPin(LCD_BACKLIGHT_PIN);
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(LCD_BACKLIGHT_PIN, HIGH);
  gpio_hold_en((gpio_num_t)LCD_BACKLIGHT_PIN);
  // Per-pad hold on a digital pad is gated by a global force-unhold bit that
  // DEFAULTS TO 1 — with it set, the hold above does nothing and the pin floats
  // (backlight dark) as soon as the chip resets. Clear the gate so the latch is real.
  REG_CLR_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_FORCE_UNHOLD);
}

void displayBegin() {
  gpio_hold_dis((gpio_num_t)LCD_BACKLIGHT_PIN);   // release a Flash-Mode hold from last boot
  // Claim the SPI bus ourselves FIRST, with MISO = -1. The write-only ST7920 never
  // uses MISO, but U8g2's ESP32 HW-SPI init would otherwise call SPI.begin(clk,MISO,
  // mosi) with the board-default MISO (GPIO 13) — which is a HID button, and grabbing
  // it clears the button's pull-up and spews phantom key presses. SPIClass::begin()
  // no-ops once the bus is up ("if(_spi) return;"), so this pre-begin wins and U8g2's
  // later begin() leaves GPIO 13 alone.
  SPI.begin(LCD_CLOCK_PIN, -1, LCD_DATA_PIN, -1);
  u8g2.begin();
  displaySetSpiClock(LCD_SPI_HZ_DEFAULT);   // begin() inits at the driver default; pin us to a known clock
  ledcSetup(BL_CHANNEL, BL_FREQ_HZ, BL_RES_BITS);
  ledcAttachPin(LCD_BACKLIGHT_PIN, BL_CHANNEL);
  displaySetBacklight(100);   // full on until displayService applies the saved level
}
