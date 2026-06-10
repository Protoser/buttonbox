#include "hid.h"
#include "USB.h"

USBHIDGamepad gamepad;

void hidBegin() {
  gamepad.begin();
  USB.begin();
}
