// HID output layer. The box is a native NKRO USB keyboard (see keyboard.cpp). The
// chord/HID engine drives it per OUTPUT index (0..13 physical, 14..31 chord outputs)
// through hidEmitPress/Release, which look up each output's key binding in settings.
#pragma once
#include <Arduino.h>

void hidBegin();                    // start the keyboard HID device (+ USB)
void hidEmitPress(uint8_t out);     // output pressed: send its bound key (if any)
void hidEmitRelease(uint8_t out);   // output released: release its bound key
