// Display panel handle (CR-10 ST7920 128x64, software SPI).
#pragma once
#include <U8g2lib.h>

extern U8G2_ST7920_128X64_F_HW_SPI u8g2;
void displayBegin();
void displaySetBacklight(uint8_t pct);   // backlight brightness, 0..100 %
void displayBacklightHoldFull();         // latch the backlight on through a reboot (Flash Mode)
void displaySetSpiClock(uint32_t hz);    // set the ST7920 SPI bus clock (DEBUG: find the ceiling)
uint32_t displayGetSpiClock();           // current SPI bus clock in Hz
