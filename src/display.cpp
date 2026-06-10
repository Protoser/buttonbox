#include "display.h"
#include "config.h"

// ST7920 in software SPI: clock=E, data=R/W, cs=RS. Rotation is applied from
// saved settings via ui's applyOrientation(); the constructor value is a default.
U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R2, LCD_CLOCK_PIN, LCD_DATA_PIN, LCD_CS_PIN, U8X8_PIN_NONE);

void displayBegin() { u8g2.begin(); }
