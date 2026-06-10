// USB HID gamepad (native TinyUSB). 32 buttons: phys 0..13, chord outputs 14..31.
#pragma once
#include "USBHIDGamepad.h"

extern USBHIDGamepad gamepad;
void hidBegin();
